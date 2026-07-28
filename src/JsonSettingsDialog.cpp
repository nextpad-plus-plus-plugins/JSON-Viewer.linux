// JsonSettingsDialog implementation (GTK4), plus the settings JSON
// persistence that macOS implements with NSJSONSerialization — here it goes
// through the already-vendored rapidjson, so config.json stays key-for-key
// compatible with the macOS plugin's file.
//
// Dialog rules from PORTING_NOTES (learned the hard way in NextZip):
//   * gtk_window_set_hide_on_close(TRUE) in the ctor — the default
//     close-request handling DESTROYS the window tree, and every widget
//     read after the modal loop would be use-after-free;
//   * explicit gtk_window_destroy AFTER the last widget read.

#include "JsonSettingsDialog.h"

#include <gtk/gtk.h>

#include <fstream>
#include <sstream>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

namespace rj = rapidjson;

namespace npj {

// ─── persistence (same keys as the macOS NSJSONSerialization dictionary) ────

Settings loadSettings() {
    Settings s;
    std::ifstream f(settingsPath());
    if (!f.is_open()) return s;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();

    rj::Document d;
    if (d.Parse(text.c_str()).HasParseError() || !d.IsObject()) return s;

    auto getBool = [&d](const char* k, bool& out) {
        auto it = d.FindMember(k);
        if (it != d.MemberEnd() && it->value.IsBool()) out = it->value.GetBool();
    };
    auto getInt = [&d](const char* k, int& out) {
        auto it = d.FindMember(k);
        if (it != d.MemberEnd() && it->value.IsInt()) out = it->value.GetInt();
    };

    getBool("followCurrentTab",    s.followCurrentTab);
    getBool("autoFormatOnOpen",    s.autoFormatOnOpen);
    getBool("ignoreTrailingComma", s.ignoreTrailingComma);
    getBool("ignoreComments",      s.ignoreComments);
    getBool("useJsonHighlight",    s.useJsonHighlight);
    getBool("replaceUndefined",    s.replaceUndefined);

    int v;
    v = -1; getInt("indent", v);      if (v >= 0) s.indent     = static_cast<IndentStyle>(v & 0xFF);
    v = -1; getInt("indentCount", v); if (v >= 0) s.indentCount = (unsigned)v;
    v = -1; getInt("eol", v);         if (v >= 0) s.eol        = static_cast<LineEnding>(v & 0xFF);
    v = -1; getInt("lineFormat", v);  if (v >= 0) s.lineFormat = static_cast<LineFormat>(v & 0xFF);
    return s;
}

void saveSettings(const Settings& s) {
    rj::Document d(rj::kObjectType);
    auto& a = d.GetAllocator();
    d.AddMember("followCurrentTab",    s.followCurrentTab,    a);
    d.AddMember("autoFormatOnOpen",    s.autoFormatOnOpen,    a);
    d.AddMember("ignoreTrailingComma", s.ignoreTrailingComma, a);
    d.AddMember("ignoreComments",      s.ignoreComments,      a);
    d.AddMember("useJsonHighlight",    s.useJsonHighlight,    a);
    d.AddMember("replaceUndefined",    s.replaceUndefined,    a);
    d.AddMember("indent",      static_cast<int>(s.indent),     a);
    d.AddMember("indentCount", static_cast<int>(s.indentCount), a);
    d.AddMember("eol",         static_cast<int>(s.eol),        a);
    d.AddMember("lineFormat",  static_cast<int>(s.lineFormat), a);

    rj::StringBuffer sb;
    rj::PrettyWriter<rj::StringBuffer> w(sb);
    d.Accept(w);

    std::ofstream f(settingsPath());
    if (f.is_open()) f << sb.GetString() << "\n";
}

// ─── dialog ──────────────────────────────────────────────────────────────────

namespace {

struct DlgState {
    GMainLoop* loop = nullptr;
    bool ok = false;
};

GtkWidget* makeRadio(const char* label, GtkCheckButton* group) {
    GtkWidget* r = gtk_check_button_new_with_label(label);
    if (group) gtk_check_button_set_group(GTK_CHECK_BUTTON(r), group);
    return r;
}

// A titled group box of radios laid out in `columns` columns (the macOS
// _groupBoxWithTitle:radios:columns: layout, on a GtkGrid).
GtkWidget* groupBox(const char* title, GtkWidget** radios, int count, int columns) {
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 28);
    gtk_widget_set_margin_top(grid, 4);
    gtk_widget_set_margin_bottom(grid, 6);
    gtk_widget_set_margin_start(grid, 10);
    gtk_widget_set_margin_end(grid, 10);
    for (int i = 0; i < count; ++i)
        gtk_grid_attach(GTK_GRID(grid), radios[i], i % columns, i / columns, 1, 1);

    GtkWidget* frame = gtk_frame_new(title);
    gtk_frame_set_child(GTK_FRAME(frame), grid);
    return frame;
}

} // namespace

bool presentSettingsDialog(Settings* settings) {
    if (!settings) return false;
    Settings cur = *settings;

    GtkWidget* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "JSON Viewer Settings");
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    // MANDATORY (PORTING_NOTES dialog rule): close must HIDE, not destroy —
    // we read the widgets after the modal loop exits.
    gtk_window_set_hide_on_close(GTK_WINDOW(win), TRUE);

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(outer, 18);
    gtk_widget_set_margin_bottom(outer, 14);
    gtk_widget_set_margin_start(outer, 20);
    gtk_widget_set_margin_end(outer, 16);
    gtk_window_set_child(GTK_WINDOW(win), outer);

    GtkWidget* columnsBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_append(GTK_BOX(outer), columnsBox);

    // ── Left column: checkboxes (labels match Windows/macOS verbatim) ──
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* cbFollow      = gtk_check_button_new_with_label("Follow current tab if it is json file");
    GtkWidget* cbAutoFormat  = gtk_check_button_new_with_label("Auto format json file when opened");
    GtkWidget* cbIgnoreComma = gtk_check_button_new_with_label("Ignore trailing comma");
    GtkWidget* cbIgnoreCmnts = gtk_check_button_new_with_label("Ignore comments in json");
    GtkWidget* cbHighlight   = gtk_check_button_new_with_label("Use json highlighting");
    GtkWidget* cbReplaceUndef= gtk_check_button_new_with_label("Replace value 'undefined' with 'null'");
    for (GtkWidget* cb : {cbFollow, cbAutoFormat, cbIgnoreComma, cbIgnoreCmnts,
                          cbHighlight, cbReplaceUndef})
        gtk_box_append(GTK_BOX(left), cb);
    gtk_box_append(GTK_BOX(columnsBox), left);

    // ── Right column: three radio groups ──
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    // Indentation (2 columns: [Auto detect | Use tab] / [Use space]) —
    // same order as the macOS box.
    GtkWidget* rIndAuto  = makeRadio("Auto detect", nullptr);
    GtkWidget* rIndTab   = makeRadio("Use tab",   GTK_CHECK_BUTTON(rIndAuto));
    GtkWidget* rIndSpace = makeRadio("Use space", GTK_CHECK_BUTTON(rIndAuto));
    GtkWidget* indentRadios[] = { rIndAuto, rIndTab, rIndSpace };
    gtk_box_append(GTK_BOX(right), groupBox("Indentation:", indentRadios, 3, 2));

    // Line Ending (2 columns). Label quirk "Macintosh (LF)" kept verbatim
    // from Windows/macOS (it is CR, but the shipped label says LF).
    GtkWidget* rEolAuto = makeRadio("Auto detect",    nullptr);
    GtkWidget* rEolWin  = makeRadio("Window (CR LF)", GTK_CHECK_BUTTON(rEolAuto));
    GtkWidget* rEolUnix = makeRadio("Unix (LF)",      GTK_CHECK_BUTTON(rEolAuto));
    GtkWidget* rEolMac  = makeRadio("Macintosh (LF)", GTK_CHECK_BUTTON(rEolAuto));
    GtkWidget* eolRadios[] = { rEolAuto, rEolWin, rEolUnix, rEolMac };
    gtk_box_append(GTK_BOX(right), groupBox("Line Ending:", eolRadios, 4, 2));

    // Line formatting (1 column)
    GtkWidget* rFmtDefault = makeRadio("Default formatting", nullptr);
    GtkWidget* rFmtSingle  = makeRadio("Format arrays on a single line",
                                       GTK_CHECK_BUTTON(rFmtDefault));
    GtkWidget* fmtRadios[] = { rFmtDefault, rFmtSingle };
    gtk_box_append(GTK_BOX(right), groupBox("Line formatting:", fmtRadios, 2, 1));

    gtk_box_append(GTK_BOX(columnsBox), right);

    // ── Buttons: OK on the LEFT of Cancel — deliberate Windows-parity
    //    order carried over from the macOS port. ──
    GtkWidget* btnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(btnRow, GTK_ALIGN_END);
    GtkWidget* btnOk     = gtk_button_new_with_label("OK");
    GtkWidget* btnCancel = gtk_button_new_with_label("Cancel");
    gtk_box_append(GTK_BOX(btnRow), btnOk);
    gtk_box_append(GTK_BOX(btnRow), btnCancel);
    gtk_box_append(GTK_BOX(outer), btnRow);
    gtk_widget_add_css_class(btnOk, "suggested-action");
    gtk_window_set_default_widget(GTK_WINDOW(win), btnOk);

    // ── Populate from current settings ──
    auto setActive = [](GtkWidget* w, bool on) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(w), on);
    };
    setActive(cbFollow,       cur.followCurrentTab);
    setActive(cbAutoFormat,   cur.autoFormatOnOpen);
    setActive(cbIgnoreComma,  cur.ignoreTrailingComma);
    setActive(cbIgnoreCmnts,  cur.ignoreComments);
    setActive(cbHighlight,    cur.useJsonHighlight);
    setActive(cbReplaceUndef, cur.replaceUndefined);

    setActive(rIndAuto,  cur.indent == IndentStyle::Auto);
    setActive(rIndSpace, cur.indent == IndentStyle::Space);
    setActive(rIndTab,   cur.indent == IndentStyle::Tab);

    setActive(rEolAuto, cur.eol == LineEnding::Auto);
    setActive(rEolWin,  cur.eol == LineEnding::Windows);
    setActive(rEolUnix, cur.eol == LineEnding::Unix);
    setActive(rEolMac,  cur.eol == LineEnding::Macintosh);

    setActive(rFmtDefault, cur.lineFormat == LineFormat::Default);
    setActive(rFmtSingle,  cur.lineFormat == LineFormat::SingleLineArrays);

    // ── Modal loop ──
    DlgState st;
    st.loop = g_main_loop_new(nullptr, FALSE);

    g_signal_connect(btnOk, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        DlgState* s = (DlgState*)p;
        s->ok = true;
        g_main_loop_quit(s->loop);
    }), &st);
    g_signal_connect(btnCancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        DlgState* s = (DlgState*)p;
        s->ok = false;
        g_main_loop_quit(s->loop);
    }), &st);
    // Titlebar ✕ (window hides itself, we just end the loop).
    g_signal_connect(win, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer p) -> gboolean {
        DlgState* s = (DlgState*)p;
        s->ok = false;
        g_main_loop_quit(s->loop);
        return FALSE;   // let hide-on-close hide the window
    }), &st);
    // Escape cancels (plain GtkWindow has no built-in Esc handling).
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed",
        G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType,
                       gpointer p) -> gboolean {
            if (keyval == GDK_KEY_Escape) {
                DlgState* s = (DlgState*)p;
                s->ok = false;
                g_main_loop_quit(s->loop);
                return TRUE;
            }
            return FALSE;
        }), &st);
    gtk_widget_add_controller(win, keys);

    gtk_window_present(GTK_WINDOW(win));
    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);

    // ── Read back BEFORE destroying (dialog UAF rule) ──
    bool ok = st.ok;
    if (ok) {
        auto active = [](GtkWidget* w) {
            return gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) == TRUE;
        };
        cur.followCurrentTab    = active(cbFollow);
        cur.autoFormatOnOpen    = active(cbAutoFormat);
        cur.ignoreTrailingComma = active(cbIgnoreComma);
        cur.ignoreComments      = active(cbIgnoreCmnts);
        cur.useJsonHighlight    = active(cbHighlight);
        cur.replaceUndefined    = active(cbReplaceUndef);

        if (active(rIndTab))        cur.indent = IndentStyle::Tab;
        else if (active(rIndSpace)) cur.indent = IndentStyle::Space;
        else                        cur.indent = IndentStyle::Auto;

        if (active(rEolWin))        cur.eol = LineEnding::Windows;
        else if (active(rEolUnix))  cur.eol = LineEnding::Unix;
        else if (active(rEolMac))   cur.eol = LineEnding::Macintosh;
        else                        cur.eol = LineEnding::Auto;

        cur.lineFormat = active(rFmtSingle) ? LineFormat::SingleLineArrays
                                            : LineFormat::Default;

        *settings = cur;
        saveSettings(cur);
    }

    gtk_window_destroy(GTK_WINDOW(win));
    return ok;
}

} // namespace npj
