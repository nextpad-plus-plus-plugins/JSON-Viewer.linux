// NppJsonViewer — Linux (GTK4) port of the macOS port of
// NPP-JSONViewer/JSON-Viewer.
//
// Thin bootstrap: implements the plugin entry points, wires seven menu
// items (Toggle / Format / Compress / Sort / — / Settings / About),
// registers the panel via NPPM_DMM_REGISTERPANEL and forwards the three
// toolbar-row actions (Refresh / Validate / Format) from JsonPanel
// delegate callbacks into the parser + editor.
//
// Platform deltas vs the macOS bootstrap (everything else is logic-identical):
//   * NSAlert                     -> GtkAlertDialog
//   * NPPM_DMM_REGISTERPANEL      -> Linux param order (wParam=title,
//     lParam=widget — REVERSED from macOS; the host rejects sniffing)
//   * NPPM_SETCURRENTLANGTYPE     -> NPPM_SETBUFFERLANGTYPE(0, L_JSON):
//     the host has no SETCURRENTLANGTYPE case, but SETBUFFERLANGTYPE with
//     wParam=0 targets the current doc and fires NPPN_LANGCHANGED
//   * dispatch_after debounce     -> g_timeout_add with source removal
//   * toolbar icon needs the FULL path (bare names are g_file_test'ed
//     literally on this host); dark variant resolves host-side from the
//     _dark.png sibling
//   * FuncItem has no _pShKey     -> no pre-bound shortcuts (Shortcut Mapper)
//   * separators must be named "-" (empty names are skipped by this host)
//   * no floating-NSPanel fallback: every 1.1.0+ Linux host has the DMM
//     docking API, so a registration failure is reported, not worked around

#include "NppPluginInterfaceLinux.h"
#include "Scintilla.h"

#include <gtk/gtk.h>

#include <dlfcn.h>

#include <memory>
#include <string>

#include "JsonPanel.h"
#include "JsonParser.h"
#include "JsonFormatter.h"
#include "JsonSettings.h"
#include "JsonSettingsDialog.h"
#include "JsonViewerBridge.h"   // inter-plugin handshake (JSTool "JSON Viewer" item)

// ─────────────────────────────────────────────────────────────────────────
//  Plugin identification
// ─────────────────────────────────────────────────────────────────────────
static const char *PLUGIN_NAME = "JSON Viewer";

enum CmdIdx {
    kCmdShowPanel   = 0,
    kCmdFormat      = 1,
    kCmdCompress    = 2,
    kCmdSort        = 3,
    kCmdSep         = 4,   // separator slot
    kCmdSettings    = 5,
    kCmdAbout       = 6,
    kCmdCount       = 7
};

static FuncItem sFuncItem[kCmdCount];
NppData sNppData;

// ─────────────────────────────────────────────────────────────────────────
//  Plugin state
// ─────────────────────────────────────────────────────────────────────────
static std::unique_ptr<JsonPanel> sJsonPanel;
static intptr_t     g_panelHandle  = 0;     // NPPM_DMM_REGISTERPANEL handle; 0 = not docked
static bool         sPanelVisible  = false;
static bool         sNppReady      = false;
static std::string  sPluginDir;             // directory containing this .so
static guint        sModifiedDebounce = 0;  // g_timeout source for SCN_MODIFIED

static npj::Settings sSettings;

// ─────────────────────────────────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────────────────────────────────
static void cmdTogglePanel();
static void cmdFormat();
static void cmdCompress();
static void cmdSort();
static void cmdSettings();
static void cmdAbout();
static void refreshTree();

// ─────────────────────────────────────────────────────────────────────────
//  Scintilla / NPP helpers
// ─────────────────────────────────────────────────────────────────────────
static inline intptr_t npp(uint32_t msg, uintptr_t w = 0, intptr_t l = 0) {
    return sNppData._sendMessage(sNppData._nppHandle, msg, w, l);
}

static NppHandle curScintilla() {
    int which = -1;
    npp(NPPM_GETCURRENTSCINTILLA, 0, (intptr_t)&which);
    return (which == 1) ? sNppData._scintillaSecondHandle : sNppData._scintillaMainHandle;
}

static inline intptr_t sci(NppHandle h, uint32_t msg, uintptr_t w = 0, intptr_t l = 0) {
    return sNppData._sendMessage(h, msg, w, l);
}

// ─────────────────────────────────────────────────────────────────────────
//  Alerts — single point of UI interaction (macOS: NSAlert).
// ─────────────────────────────────────────────────────────────────────────
static void showAlert(const char *title, const std::string &message) {
    if (!gtk_is_initialized()) return;   // headless test: nothing to show
    GtkAlertDialog *a = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(a, message.c_str());
    const char *buttons[] = {"OK", nullptr};
    gtk_alert_dialog_set_buttons(a, buttons);
    GtkWidget *w = (GtkWidget *)cpHostWindow();
    gtk_alert_dialog_show(a, (w && GTK_IS_WINDOW(w)) ? GTK_WINDOW(w) : nullptr);
    g_object_unref(a);
}

// ─────────────────────────────────────────────────────────────────────────
//  Settings file path — host plugin config dir, namespaced under a
//  NppJsonViewer/ subfolder so it survives plugin updates. Same layout as
//  macOS ("<config>/NppJsonViewer/config.json"); the macOS dot-folder
//  migration is dropped — that legacy location never existed on Linux.
// ─────────────────────────────────────────────────────────────────────────
namespace npj {
std::string settingsPath() {
    char buf[1024] = {0};
    npp(NPPM_GETPLUGINSCONFIGDIR, (uintptr_t)sizeof(buf), (intptr_t)buf);
    std::string dir;
    if (buf[0] != '\0') {
        dir = buf;
    } else {
        dir = std::string(g_get_user_data_dir()) + "/nextpad++/plugins/Config";
    }
    dir += "/NppJsonViewer";
    g_mkdir_with_parents(dir.c_str(), 0755);
    return dir + "/config.json";
}
} // namespace npj

// ─────────────────────────────────────────────────────────────────────────
//  Plugin directory — resolved from this .so's own path (same dladdr
//  pattern as the macOS resolveResourcesDir).
// ─────────────────────────────────────────────────────────────────────────
static std::string resolvePluginDir() {
    Dl_info info = {};
    if (dladdr((const void *)&resolvePluginDir, &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        auto slash = p.rfind('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
    return "";
}

// ─────────────────────────────────────────────────────────────────────────
//  Panel hosting
// ─────────────────────────────────────────────────────────────────────────
class PanelBridge : public JsonPanel::Delegate {
public:
    void onSelectNode(std::size_t line, std::size_t column, std::size_t length) override;
    void onRefresh() override  { refreshTree(); }
    void onValidate() override;
    void onFormat() override   { cmdFormat(); }
};
static PanelBridge sBridge;

static void ensureContentView() {
    if (sJsonPanel) return;
    sJsonPanel = std::make_unique<JsonPanel>(&sBridge, sPluginDir + "/resources");
}

static bool panelIsShown() {
    // The host's panel_frame ✕ hides the content without telling us, so ask
    // the widget itself (the NppMCP live-mapped pattern) — the toggle then
    // self-corrects instead of getting stuck.
    return sJsonPanel && g_panelHandle > 0 &&
           gtk_widget_get_mapped(sJsonPanel->root());
}

// ─────────────────────────────────────────────────────────────────────────
//  Editor-text collection. Selection if there is one, else the whole
//  document; multi-selections are refused (Windows behavior).
// ─────────────────────────────────────────────────────────────────────────
struct EditorSnapshot {
    bool        ok          = false;
    bool        fromSelection = false;
    std::string text;
    std::size_t selStart    = 0;
    std::size_t selEnd      = 0;
};

static EditorSnapshot snapshotEditorText() {
    EditorSnapshot out;
    NppHandle h = curScintilla();
    if (!h) return out;

    if (sci(h, SCI_GETSELECTIONS) > 1) return out;

    std::size_t selStart = (std::size_t)sci(h, SCI_GETSELECTIONSTART);
    std::size_t selEnd   = (std::size_t)sci(h, SCI_GETSELECTIONEND);
    if (selEnd < selStart) std::swap(selStart, selEnd);
    bool hasSel = (selEnd > selStart);

    if (hasSel) {
        std::size_t len = selEnd - selStart;
        if (len > 0) {
            std::string buf;
            buf.resize(len);
            Sci_TextRangeFull tr = { {(Sci_Position)selStart, (Sci_Position)selEnd}, buf.data() };
            sci(h, SCI_GETTEXTRANGEFULL, 0, (intptr_t)&tr);
            out.text.swap(buf);
            out.fromSelection = true;
        }
    } else {
        std::size_t total = (std::size_t)sci(h, SCI_GETLENGTH);
        if (total > 0) {
            std::string buf;
            buf.resize(total);
            sci(h, SCI_GETTEXT, (uintptr_t)(total + 1), (intptr_t)buf.data());
            out.text.swap(buf);
            selEnd = total;
        }
    }
    out.ok        = true;
    out.selStart  = selStart;
    out.selEnd    = selEnd;
    return out;
}

static void replaceEditorSelection(const std::string& text) {
    NppHandle h = curScintilla();
    if (!h) return;
    std::size_t selStart = (std::size_t)sci(h, SCI_GETSELECTIONSTART);
    std::size_t selEnd   = (std::size_t)sci(h, SCI_GETSELECTIONEND);
    if (selEnd == selStart) {
        // No selection → replace whole doc
        sci(h, SCI_SETSEL, 0, (intptr_t)sci(h, SCI_GETLENGTH));
        selStart = 0;
    }
    sci(h, SCI_REPLACESEL, 0, (intptr_t)text.c_str());
    // Restore a selection over the newly-inserted region so subsequent
    // format/compress chains operate on the same target.
    sci(h, SCI_SETSEL, selStart, (intptr_t)(selStart + text.size()));
}

static void applyJsonLanguage() {
    if (!sSettings.useJsonHighlight) return;
    // Linux host: SETBUFFERLANGTYPE(buffer=0 → current doc, langType).
    npp(NPPM_SETBUFFERLANGTYPE, 0, (intptr_t)/*L_JSON=*/57);
}

// Line where the last-parsed tree's text begins in the document: 0 when the
// whole document was parsed, the selection's first line when a selection
// was. Captured by refreshTree() at parse time.
//
// DELIBERATE FIX over the macOS port (worth backporting): macOS derives this
// base from the editor's LIVE selection at CLICK time. TrackingStream
// positions are relative to the parsed text, so that is only correct until
// the first click — which itself selects the clicked token, making every
// subsequent jump offset by the previous one (user-reported: "clicking
// entries sometimes jumps to wrong lines"). Freezing the base when the tree
// is built implements the documented intent.
static std::size_t sTreeBaseLine = 0;

static void jumpEditorToLine(std::size_t line, std::size_t column, std::size_t length) {
    NppHandle h = curScintilla();
    if (!h) return;

    // Resolve line → byte position within the doc, then add column.
    std::size_t absLine  = sTreeBaseLine + line;
    std::size_t lineStart = (std::size_t)sci(h, SCI_POSITIONFROMLINE, (uintptr_t)absLine);
    std::size_t target   = lineStart + column;

    sci(h, SCI_GOTOPOS, (uintptr_t)target);
    if (length > 0) {
        sci(h, SCI_SETSEL, (uintptr_t)target, (intptr_t)(target + length));
    }
    sci(h, SCI_GRABFOCUS);
}

static void selectError(std::size_t offsetWithinAnalyzed) {
    NppHandle h = curScintilla();
    if (!h) return;
    std::size_t base = (std::size_t)sci(h, SCI_GETSELECTIONSTART);
    std::size_t end  = (std::size_t)sci(h, SCI_GETSELECTIONEND);
    if (end < base) std::swap(base, end);
    if (end == base) base = 0;  // no selection → error offset is absolute
    std::size_t pos = base + offsetWithinAnalyzed;
    sci(h, SCI_SETSEL, pos, (intptr_t)(pos + 1));
    sci(h, SCI_GOTOPOS, pos);
}

// ─────────────────────────────────────────────────────────────────────────
//  refreshTree — the central "re-parse + repopulate the panel" flow.
// ─────────────────────────────────────────────────────────────────────────
static void refreshTree() {
    if (!sJsonPanel) return;
    EditorSnapshot s = snapshotEditorText();
    if (!s.ok || s.text.empty()) {
        sJsonPanel->showPlaceholderMessage(
            "Unable to parse JSON. Please ensure a valid JSON string is selected.");
        return;
    }
    // Freeze the jump base for THIS parse (see sTreeBaseLine).
    sTreeBaseLine = s.fromSelection
        ? (std::size_t)sci(curScintilla(), SCI_LINEFROMPOSITION, (uintptr_t)s.selStart)
        : 0;
    npj::ParseResult r = npj::parseJson(s.text, npj::toParseOptions(sSettings));

    // If plain parse failed AND user has "Replace undefined with null" on,
    // retry with the regex pre-pass.
    if (r.status != npj::ParseStatus::Ok && sSettings.replaceUndefined) {
        std::string replaced = npj::replaceUndefinedWithNull(s.text);
        if (replaced != s.text) {
            r = npj::parseJson(replaced, npj::toParseOptions(sSettings));
        }
    }

    if (r.status == npj::ParseStatus::Ok) {
        sJsonPanel->setTree(std::move(r.root));
    } else {
        char msg[512];
        g_snprintf(msg, sizeof(msg), "Parse error at offset %zu (code %d): %s",
                   r.errorOffset, r.errorCode,
                   r.errorMessage.empty() ? "unknown" : r.errorMessage.c_str());
        sJsonPanel->showPlaceholderMessage(msg);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Menu-command implementations
// ─────────────────────────────────────────────────────────────────────────
static bool ensurePanelRegistered() {
    ensureContentView();
    if (g_panelHandle > 0) return true;
    // Linux ABI: wParam = title, lParam = widget (REVERSED from macOS —
    // this host's strict order; the GTK_IS_WIDGET param sniff segfaults
    // and was rejected, so there is no auto-accept of the macOS order).
    intptr_t h = npp(NPPM_DMM_REGISTERPANEL,
                     (uintptr_t)"JSON Viewer",
                     (intptr_t)sJsonPanel->root());
    if (h > 0) {
        g_panelHandle = h;
        // Declare the reopen command so the host restores the panel after a
        // restart (GH linux#18): module = getName() ("JSON Viewer"),
        // cmdIndex 0 = "Show JSON Viewer". Hosts < 1.1.0 return 0 — ignored.
        NppPanelInfo info;
        info.moduleName = PLUGIN_NAME;
        info.cmdIndex   = 0;
        npp(NPPM_DMM_SETPANELINFO, (uintptr_t)g_panelHandle, (intptr_t)&info);
        return true;
    }
    showAlert("JSON Viewer", "The host rejected the panel registration.");
    return false;
}

static void cmdTogglePanel() {
    if (!ensurePanelRegistered()) return;

    bool target = !panelIsShown();
    sPanelVisible = target;
    npp(NPPM_SETMENUITEMCHECK, (uintptr_t)sFuncItem[kCmdShowPanel]._cmdID, target ? 1 : 0);

    if (target) {
        npp(NPPM_DMM_SHOWPANEL, (uintptr_t)g_panelHandle, 0);
        refreshTree();
    } else {
        npp(NPPM_DMM_HIDEPANEL, (uintptr_t)g_panelHandle, 0);
    }
}

static void cmdFormat() {
    EditorSnapshot s = snapshotEditorText();
    if (!s.ok || s.text.empty()) {
        showAlert("JSON Viewer",
                  "Unable to parse JSON. Please ensure a valid JSON string is selected.");
        return;
    }
    npj::FormatResult r = npj::formatJson(s.text, npj::toFormatOptions(sSettings));
    if (!r.success && sSettings.replaceUndefined) {
        r = npj::formatJson(npj::replaceUndefinedWithNull(s.text), npj::toFormatOptions(sSettings));
    }
    if (!r.success) {
        char msg[512];
        g_snprintf(msg, sizeof(msg), "Offset %zu (code %d): %s",
                   r.errorOffset, r.errorCode, r.errorMessage.c_str());
        showAlert("JSON Viewer: Parse error", msg);
        selectError(r.errorOffset);
        return;
    }
    replaceEditorSelection(r.output);
    applyJsonLanguage();
    if (sPanelVisible) refreshTree();
}

static void cmdCompress() {
    EditorSnapshot s = snapshotEditorText();
    if (!s.ok || s.text.empty()) return;
    npj::FormatResult r = npj::compressJson(s.text, npj::toFormatOptions(sSettings));
    if (!r.success && sSettings.replaceUndefined) {
        r = npj::compressJson(npj::replaceUndefinedWithNull(s.text), npj::toFormatOptions(sSettings));
    }
    if (!r.success) {
        selectError(r.errorOffset);
        showAlert("JSON Viewer: Parse error", r.errorMessage);
        return;
    }
    replaceEditorSelection(r.output);
    applyJsonLanguage();
    if (sPanelVisible) refreshTree();
}

static void cmdSort() {
    EditorSnapshot s = snapshotEditorText();
    if (!s.ok || s.text.empty()) return;
    npj::FormatResult r = npj::sortJsonByKey(s.text, npj::toFormatOptions(sSettings));
    if (!r.success && sSettings.replaceUndefined) {
        r = npj::sortJsonByKey(npj::replaceUndefinedWithNull(s.text), npj::toFormatOptions(sSettings));
    }
    if (!r.success) {
        selectError(r.errorOffset);
        showAlert("JSON Viewer: Parse error", r.errorMessage);
        return;
    }
    replaceEditorSelection(r.output);
    applyJsonLanguage();
    if (sPanelVisible) refreshTree();
}

static void cmdValidate() {
    EditorSnapshot s = snapshotEditorText();
    if (!s.ok || s.text.empty()) {
        showAlert("JSON Viewer", "The text is empty.");
        return;
    }
    npj::ParseResult r = npj::parseJson(s.text, npj::toParseOptions(sSettings));
    if (r.status == npj::ParseStatus::Ok) {
        showAlert("JSON Viewer",
                  "The JSON appears valid. No errors were found during validation.");
    } else {
        char msg[512];
        g_snprintf(msg, sizeof(msg), "Offset %zu (code %d): %s",
                   r.errorOffset, r.errorCode, r.errorMessage.c_str());
        selectError(r.errorOffset);
        showAlert("JSON Viewer: Validation error", msg);
    }
}

static void cmdSettings() {
    if (npj::presentSettingsDialog(&sSettings)) {
        if (sPanelVisible) refreshTree();
    }
}

static void cmdAbout() {
    showAlert("JSON Viewer — Linux port",
        "Tree-view navigator, formatter, compressor and validator for JSON documents.\n\n"
        "Linux port of NPP-JSONViewer (GPLv2), via the macOS port. "
        "Parser powered by RapidJSON SAX.\n\n"
        "This host has no per-item plugin shortcuts, so the Windows/macOS "
        "Ctrl+Alt+Shift+J/M/C/K defaults are not pre-bound — assign them in "
        "Settings > Shortcut Mapper > Plugin commands.");
}

// ─────────────────────────────────────────────────────────────────────────
//  Bridge delegate (panel toolbar buttons → our commands)
// ─────────────────────────────────────────────────────────────────────────
void PanelBridge::onSelectNode(std::size_t line, std::size_t column, std::size_t length) {
    jumpEditorToLine(line, column, length);
}
void PanelBridge::onValidate() { cmdValidate(); }

// ─────────────────────────────────────────────────────────────────────────
//  Plugin exports
// ─────────────────────────────────────────────────────────────────────────
extern "C" NPP_EXPORT void setInfo(LinuxHostNppData data) {
    // Swap the host's flat single-view NppData for the macOS-shaped one
    // (sentinel handles + routing cpSendMessage) — see LinuxViewBridge.cpp.
    cpBridgeInit(&data);
    sNppData._nppHandle             = kHandleNpp;
    sNppData._scintillaMainHandle   = kHandleScintillaMain;
    sNppData._scintillaSecondHandle = kHandleScintillaSub;
    sNppData._sendMessage           = cpSendMessage;

    sPluginDir = resolvePluginDir();
    sSettings  = npj::loadSettings();

    memset(sFuncItem, 0, sizeof(sFuncItem));

    auto setItem = [&](int idx, const char *name, PFUNCPLUGINCMD fn) {
        strlcpy(sFuncItem[idx]._itemName, name, NPP_MENU_ITEM_SIZE);
        sFuncItem[idx]._pFunc      = fn;
        sFuncItem[idx]._init2Check = false;
    };

    setItem(kCmdShowPanel, "Show JSON Viewer",        cmdTogglePanel);
    setItem(kCmdFormat,    "Format JSON",             cmdFormat);
    setItem(kCmdCompress,  "Compress JSON",           cmdCompress);
    setItem(kCmdSort,      "Sort by key (ascending)", cmdSort);
    // Separator: macOS uses an EMPTY name; this host SKIPS empty-named
    // items entirely and renders a FuncItem named exactly "-" as a divider.
    setItem(kCmdSep, "-", nullptr);
    setItem(kCmdSettings,  "Settings",                cmdSettings);
    setItem(kCmdAbout,     "About",                   cmdAbout);
    // No _pShKey on this host's FuncItem — the macOS Ctrl+Alt+Shift
    // shortcuts can't be pre-bound; users assign them in the Shortcut Mapper.
}

extern "C" NPP_EXPORT const char *getName() { return PLUGIN_NAME; }

extern "C" NPP_EXPORT FuncItem *getFuncsArray(int *nbF) {
    *nbF = kCmdCount;
    return sFuncItem;
}

extern "C" NPP_EXPORT void beNotified(SCNotification *n) {
    if (!n) return;
    switch (n->nmhdr.code) {
        case NPPN_TBMODIFICATION: {
            // Register our toolbar icon. This host needs the FULL path
            // (bare names are tested literally and silently yield no
            // button); the _dark.png sibling is resolved host-side.
            std::string icon = sPluginDir + "/resources/toolbar.png";
            npp(NPPM_ADDTOOLBARICON_FORDARKMODE,
                (uintptr_t)sFuncItem[kCmdShowPanel]._cmdID,
                (intptr_t)icon.c_str());
            break;
        }
        case NPPN_READY:
            sNppReady = true;
            break;
        case NPPN_BUFFERACTIVATED:
            if (sPanelVisible && sSettings.followCurrentTab) refreshTree();
            break;
        case NPPN_FILEOPENED:
            if (sSettings.autoFormatOnOpen) {
                // Match Windows: only auto-format files that look like JSON.
                int lang = 0;
                npp(NPPM_GETCURRENTLANGTYPE, 0, (intptr_t)&lang);
                if (lang == /*L_JSON=*/57) cmdFormat();
            }
            break;
        case SCN_MODIFIED:
            if (sPanelVisible && (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
                // Debounced live re-parse (macOS: dispatch_after 200 ms).
                if (sModifiedDebounce) g_source_remove(sModifiedDebounce);
                sModifiedDebounce = g_timeout_add(200, +[](gpointer) -> gboolean {
                    sModifiedDebounce = 0;
                    refreshTree();
                    return G_SOURCE_REMOVE;
                }, nullptr);
            }
            break;
        case NPPN_SHUTDOWN:
            npj::saveSettings(sSettings);
            if (sModifiedDebounce) {
                g_source_remove(sModifiedDebounce);
                sModifiedDebounce = 0;
            }
            if (g_panelHandle > 0) {
                npp(NPPM_DMM_UNREGISTERPANEL, (uintptr_t)g_panelHandle, 0);
                g_panelHandle = 0;
            }
            sJsonPanel.reset();
            break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Inter-plugin bridge: make the panel visible on request (does NOT
//  toggle). Used by the JSTool (JSMinNPP) "JSON Viewer" menu item via
//  NPPM_MSGTOPLUGIN. NOTE for a future JSTool port: this host routes
//  MSGTOPLUGIN by getName() — target "JSON Viewer", not the module folder
//  name "NppJsonViewer" that the shared bridge header documents for macOS.
// ─────────────────────────────────────────────────────────────────────────
static void bridgeShowPanel() {
    if (!ensurePanelRegistered()) return;

    if (!panelIsShown()) {
        sPanelVisible = true;
        npp(NPPM_SETMENUITEMCHECK, (uintptr_t)sFuncItem[kCmdShowPanel]._cmdID, 1);
        npp(NPPM_DMM_SHOWPANEL, (uintptr_t)g_panelHandle, 0);
    }
    refreshTree();
}

extern "C" NPP_EXPORT intptr_t messageProc(uint32_t Message, uintptr_t wParam, intptr_t lParam) {
    // The host delivers inter-plugin messages as (NPPM_MSGTOPLUGIN, 0, ci),
    // synchronously on the GTK main thread.
    if (Message == NPPM_MSGTOPLUGIN && lParam) {
        struct CommunicationInfo *ci = (struct CommunicationInfo *)lParam;
        switch (ci->internalMsg) {
            case JV_BRIDGE_MSG_PING:
                // Identify ourselves as a bridge-capable JSON Viewer.
                return JV_BRIDGE_ACK;
            case JV_BRIDGE_MSG_SHOWPANEL:
                bridgeShowPanel();
                return 1;
            default:
                break;
        }
    }
    return 1;
}

extern "C" NPP_EXPORT int isUnicode() { return 1; }
