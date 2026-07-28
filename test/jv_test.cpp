// Behaviour test for JSON Viewer (Linux).
//
// dlopens the REAL plugin .so and invokes the real menu callbacks through the
// REAL LinuxViewBridge against a mock Scintilla document with an actual text
// buffer, so the rapidjson parse/format/sort paths, the panel tree build, the
// lazy expansion, the filter and the settings persistence all run for real.
//
// Two dlopen rounds:
//   round 1 (default settings): format / compress / sort / validate-error /
//     selection-scoped format / panel registration + tree content / lazy
//     expand / placeholder on parse error / live SCN_MODIFIED debounce /
//     search filter / NPPN_SHUTDOWN persistence
//   round 2 (pre-seeded config.json: tab indent, single-line arrays,
//     replaceUndefined): format honors every setting + undefined -> null
//
// Like the host, the mock is injected two ways: this executable EXPORTS
// scintilla_view_send_message (the plugin's undefined symbol binds to it at
// dlopen), and a minimal REAL widget tree gives the bridge something to walk.
//
// Usage: jv_test <so path>   (requires a display — run under Xvfb)

#include "NppPluginInterfaceLinux.h"
#include "Scintilla.h"

#include <gtk/gtk.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ── mock Scintilla document ─────────────────────────────────────────────────
struct MockSci {
    std::string buf;
    long selStart = 0, selEnd = 0;
    int  langSet  = -1;         // NPPM_SETBUFFERLANGTYPE record (host side)

    long lineFromPos(long pos) const {
        long line = 0;
        for (long i = 0; i < pos && i < (long)buf.size(); ++i)
            if (buf[i] == '\n') ++line;
        return line;
    }
    long posFromLine(long line) const {
        if (line <= 0) return 0;
        long seen = 0;
        for (long i = 0; i < (long)buf.size(); ++i)
            if (buf[i] == '\n' && ++seen == line) return i + 1;
        return (long)buf.size();
    }
};
static MockSci g_m;
static GtkWidget *g_mockSciWidget = nullptr;

static long clampPos(long long p) {
    if (p < 0) return 0;
    if (p > (long long)g_m.buf.size()) return (long)g_m.buf.size();
    return (long)p;
}

extern "C" __attribute__((visibility("default")))
intptr_t scintilla_view_send_message(void *view, unsigned int msg,
                                     uintptr_t w, intptr_t l) {
    if (view != g_mockSciWidget) return 0;
    MockSci &m = g_m;
    switch (msg) {
    case SCI_GETSELECTIONS:      return 1;
    case SCI_GETCURRENTPOS:      return m.selEnd;
    case SCI_GETSELECTIONSTART:  return m.selStart;
    case SCI_GETSELECTIONEND:    return m.selEnd;
    case SCI_GETLENGTH:
    case SCI_GETTEXTLENGTH:      return (intptr_t)m.buf.size();
    case SCI_GETTEXT: {
        char *out = (char *)(intptr_t)l;
        if (!out || w == 0) return 0;
        size_t n = m.buf.size();
        if (n > w - 1) n = w - 1;
        memcpy(out, m.buf.data(), n);
        out[n] = '\0';
        return (intptr_t)n;
    }
    case SCI_GETTEXTRANGEFULL: {
        Sci_TextRangeFull *tr = (Sci_TextRangeFull *)(intptr_t)l;
        if (!tr || !tr->lpstrText) return 0;
        long a = clampPos(tr->chrg.cpMin), b = clampPos(tr->chrg.cpMax);
        if (b < a) b = a;
        memcpy(tr->lpstrText, m.buf.data() + a, (size_t)(b - a));
        tr->lpstrText[b - a] = '\0';
        return b - a;
    }
    case SCI_LINEFROMPOSITION:   return m.lineFromPos(clampPos((long long)w));
    case SCI_POSITIONFROMLINE:   return m.posFromLine((long)w);
    case SCI_REPLACESEL: {
        const char *t = (const char *)(intptr_t)l;
        m.buf.replace((size_t)m.selStart, (size_t)(m.selEnd - m.selStart), t);
        m.selStart = m.selEnd = m.selStart + (long)strlen(t);
        return 0;
    }
    case SCI_SETSEL:  m.selStart = clampPos((long long)w); m.selEnd = clampPos((long long)l); return 0;
    case SCI_GOTOPOS: m.selStart = m.selEnd = clampPos((long long)w); return 0;
    case SCI_GRABFOCUS: return 0;
    default: return 0;
    }
}

// ── mock host ───────────────────────────────────────────────────────────────
static std::string g_configDir;
static GtkWidget  *g_registeredPanel = nullptr;
static std::string g_registeredTitle;
static int         g_showPanelCalls = 0;
static int         g_hidePanelCalls = 0;
static int         g_unregisterCalls = 0;

static long mockHostMsg(unsigned int msg, unsigned long wParam, long lParam) {
    switch (msg) {
    case NPPM_GETCURRENTSCINTILLA:
        if (lParam) *((int *)(intptr_t)lParam) = 0;
        return (long)(intptr_t)g_mockSciWidget;
    case NPPM_GETCURRENTLANGTYPE:
        if (lParam) *((int *)(intptr_t)lParam) = 57;   // L_JSON
        return 57;
    case NPPM_SETBUFFERLANGTYPE:
        g_m.langSet = (int)lParam;
        return 1;
    case NPPM_GETPLUGINSCONFIGDIR:
        if (lParam) g_strlcpy((char *)(intptr_t)lParam, g_configDir.c_str(), (gsize)wParam);
        return 1;
    case NPPM_DMM_REGISTERPANEL:
        // Linux ABI: wParam = title, lParam = widget.
        g_registeredTitle  = wParam ? (const char *)(intptr_t)wParam : "";
        g_registeredPanel  = (GtkWidget *)(intptr_t)lParam;
        return 1;   // handle
    case NPPM_DMM_SHOWPANEL:  ++g_showPanelCalls; return 1;
    case NPPM_DMM_HIDEPANEL:  ++g_hidePanelCalls; return 1;
    case NPPM_DMM_UNREGISTERPANEL: ++g_unregisterCalls; return 1;
    case NPPM_SETMENUITEMCHECK: return 1;
    default: return 0;
    }
}

// ── assertion helpers ───────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string &what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    ok ? ++g_pass : ++g_fail;
}
static void checkEq(const std::string &got, const std::string &want, const std::string &what) {
    bool ok = (got == want);
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) printf("         want: %s\n         got:  %s\n", want.c_str(), got.c_str());
    ok ? ++g_pass : ++g_fail;
}
static void checkHas(const std::string &hay, const std::string &needle, const std::string &what) {
    bool ok = hay.find(needle) != std::string::npos;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) printf("         looked for \"%s\" in:\n---\n%s\n---\n", needle.c_str(), hay.c_str());
    ok ? ++g_pass : ++g_fail;
}

static void pumpMs(int ms) {
    gint64 until = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < until)
        g_main_context_iteration(nullptr, FALSE);
}

// ── panel widget spelunking ─────────────────────────────────────────────────
static GtkWidget* findDescendant(GtkWidget *root, GType type) {
    if (!root) return nullptr;
    if (g_type_is_a(G_OBJECT_TYPE(root), type)) return root;
    for (GtkWidget *c = gtk_widget_get_first_child(root); c;
         c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *hit = findDescendant(c, type);
        if (hit) return hit;
    }
    return nullptr;
}

// Store shape helpers (columns per JsonPanel.cpp: 0=label, 1=node ptr).
static std::string rowLabel(GtkTreeModel *m, GtkTreeIter *it) {
    gchar *s = nullptr;
    gtk_tree_model_get(m, it, 0, &s, -1);
    std::string out = s ? s : "";
    g_free(s);
    return out;
}
static std::string rowColor(GtkTreeModel *m, GtkTreeIter *it) {
    gchar *s = nullptr;
    gtk_tree_model_get(m, it, 2, &s, -1);
    std::string out = s ? s : "";
    g_free(s);
    return out;
}

// Dump child labels of an iter (or top level when parent==nullptr).
static std::vector<std::string> childLabels(GtkTreeModel *m, GtkTreeIter *parent) {
    std::vector<std::string> out;
    GtkTreeIter it;
    if (!gtk_tree_model_iter_children(m, &it, parent)) return out;
    do { out.push_back(rowLabel(m, &it)); }
    while (gtk_tree_model_iter_next(m, &it));
    return out;
}

static std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (const auto &x : v) { if (!s.empty()) s += " | "; s += x; }
    return s;
}

// ── plugin round driver ─────────────────────────────────────────────────────
struct Plugin {
    void *lib = nullptr;
    FuncItem *items = nullptr;
    int nItems = 0;
    void (*notify)(SCNotification *) = nullptr;
    intptr_t (*msgProc)(uint32_t, uintptr_t, intptr_t) = nullptr;

    bool load(const char *path) {
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!lib) { printf("dlopen FAIL: %s\n", dlerror()); return false; }
        auto pSetInfo  = (void (*)(LinuxHostNppData))dlsym(lib, "setInfo");
        auto pGetFuncs = (FuncItem * (*)(int *)) dlsym(lib, "getFuncsArray");
        notify         = (void (*)(SCNotification *))dlsym(lib, "beNotified");
        msgProc        = (intptr_t (*)(uint32_t, uintptr_t, intptr_t))dlsym(lib, "messageProc");
        if (!pSetInfo || !pGetFuncs || !notify || !msgProc) { printf("dlsym FAIL\n"); return false; }

        LinuxHostNppData nd{};
        nd.nppHandle             = gtk_widget_get_ancestor(g_mockSciWidget, GTK_TYPE_WINDOW);
        nd.scintillaMainHandle   = g_mockSciWidget;
        nd.scintillaSecondHandle = nullptr;
        nd.hostMsg               = mockHostMsg;
        pSetInfo(nd);
        items = pGetFuncs(&nItems);
        return true;
    }
    void sendNotif(unsigned code, int modType = 0) {
        SCNotification n = {};
        n.nmhdr.code = code;
        n.modificationType = modType;
        notify(&n);
    }
    void unload() { if (lib) { dlclose(lib); lib = nullptr; } }
};

static void setDoc(const std::string &text, long selStart = 0, long selEnd = 0) {
    g_m.buf = text;
    g_m.selStart = selStart;
    g_m.selEnd = selEnd;
    g_m.langSet = -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <so>\n", argv[0]); return 2; }
    if (!gtk_init_check()) { fprintf(stderr, "gtk_init failed (need a display)\n"); return 2; }

    gchar *tmpl = g_strdup("/tmp/jv-test-XXXXXX");
    g_configDir = g_mkdtemp(tmpl) ? tmpl : "/tmp";

    // ── the minimal REAL widget tree the bridge discovers ────────────────
    GtkWidget *win = gtk_window_new();
    GtkWidget *nb = gtk_notebook_new();
    gtk_widget_add_css_class(nb, "npp-editor-tabs");
    GtkWidget *sw = gtk_scrolled_window_new();
    g_mockSciWidget = gtk_text_view_new();   // GtkScrollable, like ScintillaView
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), g_mockSciWidget);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new("mock"));
    gtk_window_set_child(GTK_WINDOW(win), nb);

    Plugin plug;
    if (!plug.load(argv[1])) return 1;
    auto cmdToggle   = [&] { plug.items[0]._pFunc(); };
    auto cmdFormat   = [&] { plug.items[1]._pFunc(); };
    auto cmdCompress = [&] { plug.items[2]._pFunc(); };
    auto cmdSort     = [&] { plug.items[3]._pFunc(); };

    // ── Format (defaults: 4-space indent, LF, default line format) ───────
    printf("== Format JSON (defaults) ==\n");
    setDoc("{\"b\":1,\"a\":[1,2]}");
    cmdFormat();
    checkEq(g_m.buf,
            "{\n    \"b\": 1,\n    \"a\": [\n        1,\n        2\n    ]\n}",
            "pretty-printed with 4-space indent + LF");
    check(g_m.langSet == 57, "JSON language applied (SETBUFFERLANGTYPE 57)");
    check(g_m.selStart == 0 && g_m.selEnd == (long)g_m.buf.size(),
          "selection restored over the formatted output");

    // ── Compress ──────────────────────────────────────────────────────────
    printf("\n== Compress JSON ==\n");
    setDoc("{\n  \"b\": 1,\n  \"a\": [1, 2]\n}");
    cmdCompress();
    checkEq(g_m.buf, "{\"b\":1,\"a\":[1,2]}", "minified to one line");

    // ── Sort by key (recursive) ───────────────────────────────────────────
    printf("\n== Sort by key ==\n");
    setDoc("{\"b\":{\"z\":1,\"y\":2},\"a\":3}");
    cmdSort();
    checkEq(g_m.buf,
            "{\n    \"a\": 3,\n    \"b\": {\n        \"y\": 2,\n        \"z\": 1\n    }\n}",
            "keys sorted alphabetically at every level");

    // ── RawNumber round-trip ──────────────────────────────────────────────
    printf("\n== Number formatting survives round-trip ==\n");
    setDoc("{\"v\":0.30000000000000004,\"big\":92233720368547758070}");
    cmdCompress();
    checkHas(g_m.buf, "0.30000000000000004", "float text preserved exactly");
    checkHas(g_m.buf, "92233720368547758070", "oversize integer preserved exactly");

    // ── Validate: error selects the offending offset ──────────────────────
    printf("\n== Parse error selects the offset ==\n");
    setDoc("{\"a\":}");   // error at offset 5
    cmdFormat();          // alert is async/non-blocking; the caret is the observable
    // selectError does SETSEL(pos,pos+1) then GOTOPOS(pos) — GOTOPOS collapses
    // the selection, so the final state (macOS-identical) is the caret AT the
    // error offset, not a 1-char selection.
    check(g_m.selStart == 5 && g_m.selEnd == 5, "caret moved to the error offset (5)");

    // ── Selection-scoped format ───────────────────────────────────────────
    printf("\n== Selection-scoped format ==\n");
    {
        std::string pre = "leading garbage ", json = "{\"k\":1}";
        setDoc(pre + json + " trailing", (long)pre.size(), (long)(pre.size() + json.size()));
        cmdFormat();
        checkHas(g_m.buf, "leading garbage {\n    \"k\": 1\n}", "only the selection was formatted");
        checkHas(g_m.buf, " trailing", "text after the selection untouched");
    }

    // ── Panel: register + tree content ────────────────────────────────────
    printf("\n== Panel registration + tree ==\n");
    setDoc("{\"name\":\"npp\",\"meta\":{\"ver\":1,\"ids\":[1,2]}}");
    cmdToggle();
    check(g_registeredPanel != nullptr, "NPPM_DMM_REGISTERPANEL received a widget (lParam)");
    checkEq(g_registeredTitle, "JSON Viewer", "panel title passed in wParam (Linux order)");
    check(g_showPanelCalls == 1, "NPPM_DMM_SHOWPANEL called");

    // Mount the panel in a window so rows can map/expand.
    GtkWidget *panelWin = gtk_window_new();
    gtk_window_set_child(GTK_WINDOW(panelWin), g_registeredPanel);
    gtk_window_present(GTK_WINDOW(panelWin));
    pumpMs(50);

    GtkWidget *tv = findDescendant(g_registeredPanel, GTK_TYPE_TREE_VIEW);
    check(tv != nullptr, "panel contains a GtkTreeView");
    GtkTreeModel *model = tv ? gtk_tree_view_get_model(GTK_TREE_VIEW(tv)) : nullptr;

    if (model) {
        GtkTreeIter root;
        check(gtk_tree_model_get_iter_first(model, &root), "store has a top-level row");
        checkEq(rowLabel(model, &root), "JSON", "root row is labeled 'JSON'");
        // Root was auto-expanded by setTree → children are materialized.
        checkEq(join(childLabels(model, &root)),
                "name : \"npp\" | meta {2}",
                "level-1 rows: leaf label + container count");
        // Lazy: expand 'meta' via the view (fires test-expand-row).
        GtkTreeIter meta;
        gtk_tree_model_iter_nth_child(model, &meta, &root, 1);
        GtkTreePath *mp = gtk_tree_model_get_path(model, &meta);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(tv), mp, FALSE);
        gtk_tree_path_free(mp);
        pumpMs(30);
        checkEq(join(childLabels(model, &meta)),
                "ver : 1 | ids [2]",
                "lazy expansion materializes 'meta' children");
        // Type colors on leaves.
        GtkTreeIter nameIt;
        gtk_tree_model_iter_nth_child(model, &nameIt, &root, 0);
        checkEq(rowColor(model, &nameIt), "#2da44e", "string leaf uses the green FG");
    }

    // ── SCN_MODIFIED debounce refresh ─────────────────────────────────────
    printf("\n== Debounced live refresh ==\n");
    setDoc("[true,null]");
    plug.sendNotif(SCN_MODIFIED, SC_MOD_INSERTTEXT);
    pumpMs(400);   // debounce is 200 ms
    if (model) {
        GtkTreeIter root;
        gtk_tree_model_get_iter_first(model, &root);
        checkEq(join(childLabels(model, &root)),
                "[0] : true | [1] : null",
                "tree re-parsed 200ms after SCN_MODIFIED");
        GtkTreeIter b0;
        gtk_tree_model_iter_nth_child(model, &b0, &root, 0);
        checkEq(rowColor(model, &b0), "#e66100", "bool leaf uses the orange FG");
    }

    // ── Placeholder on parse error ────────────────────────────────────────
    printf("\n== Placeholder on parse error ==\n");
    setDoc("{broken");
    plug.sendNotif(SCN_MODIFIED, SC_MOD_INSERTTEXT);
    pumpMs(400);
    if (model) {
        GtkTreeIter it;
        gtk_tree_model_get_iter_first(model, &it);
        checkHas(rowLabel(model, &it), "Parse error at offset", "placeholder row carries the error");
        check(!gtk_tree_model_iter_has_child(model, &it), "placeholder has no children");
    }

    // ── Search filter ─────────────────────────────────────────────────────
    printf("\n== Search filter ==\n");
    setDoc("{\"alpha\":{\"needle\":1},\"beta\":2}");
    plug.sendNotif(SCN_MODIFIED, SC_MOD_INSERTTEXT);
    pumpMs(400);
    GtkWidget *entry = findDescendant(g_registeredPanel, GTK_TYPE_SEARCH_ENTRY);
    check(entry != nullptr, "panel contains a GtkSearchEntry");
    if (entry && model) {
        gtk_editable_set_text(GTK_EDITABLE(entry), "needle");
        pumpMs(500);   // GtkSearchEntry emits search-changed after ~150 ms
        GtkTreeIter root;
        gtk_tree_model_get_iter_first(model, &root);
        checkEq(join(childLabels(model, &root)), "alpha {1}",
                "filter shows only the matching branch (beta pruned)");
        GtkTreeIter alpha;
        gtk_tree_model_iter_nth_child(model, &alpha, &root, 0);
        checkEq(join(childLabels(model, &alpha)), "needle : 1",
                "matched descendant materialized eagerly");
        // Clear via stop-search (Escape path)
        g_signal_emit_by_name(entry, "stop-search");
        pumpMs(300);
        gtk_tree_model_get_iter_first(model, &root);
        checkEq(join(childLabels(model, &root)), "alpha {1} | beta : 2",
                "clearing the filter restores the full tree");
    }

    // ── Toggle hides ──────────────────────────────────────────────────────
    printf("\n== Toggle hides the shown panel ==\n");
    cmdToggle();
    check(g_hidePanelCalls == 1, "NPPM_DMM_HIDEPANEL called on second toggle");

    // ── Inter-plugin bridge ───────────────────────────────────────────────
    printf("\n== JSTool bridge handshake ==\n");
    {
        setDoc("{\"x\":1}");
        CommunicationInfo ci = {};
        ci.internalMsg = 0x4A53544FL;  // JV_BRIDGE_MSG_PING
        ci.srcModuleName = "JSTool";
        intptr_t r = plug.msgProc(NPPM_MSGTOPLUGIN, 0, (intptr_t)&ci);
        check(r == 0x4A564F4BL, "PING answered with JV_BRIDGE_ACK");
        // The mock host's HIDEPANEL doesn't unmap the widget (the real host
        // does), so hide the mount window to make panelIsShown() report
        // hidden — otherwise the bridge correctly skips the redundant show.
        gtk_widget_set_visible(panelWin, FALSE);
        pumpMs(50);
        ci.internalMsg = 0x4A53544EL;  // JV_BRIDGE_MSG_SHOWPANEL
        int showBefore = g_showPanelCalls;
        plug.msgProc(NPPM_MSGTOPLUGIN, 0, (intptr_t)&ci);
        check(g_showPanelCalls == showBefore + 1, "SHOWPANEL request shows the panel");
    }

    // ── Shutdown persists settings ────────────────────────────────────────
    printf("\n== NPPN_SHUTDOWN persists config.json ==\n");
    plug.sendNotif(NPPN_SHUTDOWN);
    check(g_unregisterCalls == 1, "panel unregistered at shutdown");
    {
        std::ifstream f(g_configDir + "/NppJsonViewer/config.json");
        std::stringstream ss; ss << f.rdbuf();
        checkHas(ss.str(), "\"ignoreComments\"", "config.json written with settings keys");
    }
    // Window still holds the panel widget; drop it before dlclose so the
    // plugin's static JsonPanel isn't destroyed under GTK's feet.
    gtk_window_destroy(GTK_WINDOW(panelWin));
    pumpMs(30);
    g_registeredPanel = nullptr;
    plug.unload();

    // ── Round 2: pre-seeded settings ──────────────────────────────────────
    printf("\n== Round 2: tab indent + single-line arrays + replaceUndefined ==\n");
    {
        std::ofstream f(g_configDir + "/NppJsonViewer/config.json");
        f << "{\"indent\":2,\"lineFormat\":1,\"replaceUndefined\":true}\n";
    }
    Plugin plug2;
    if (!plug2.load(argv[1])) return 1;

    setDoc("{\"a\":[1,2,3]}");
    plug2.items[1]._pFunc();   // Format
    checkEq(g_m.buf, "{\n\t\"a\": [1, 2, 3]\n}",
            "tab indent + arrays on a single line");

    setDoc("{\"a\": undefined}");
    plug2.items[1]._pFunc();   // Format — plain parse fails, pre-pass retries
    checkEq(g_m.buf, "{\n\t\"a\": null\n}", "undefined replaced with null on retry");

    plug2.sendNotif(NPPN_SHUTDOWN);
    plug2.unload();

    printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    g_free(tmpl);
    return g_fail == 0 ? 0 : 1;
}
