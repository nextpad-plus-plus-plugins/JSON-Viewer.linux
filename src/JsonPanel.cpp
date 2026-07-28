// JsonPanel implementation (GTK4). See JsonPanel.h for the design notes.
//
// Store columns: label text, JsonNode* (G_TYPE_POINTER), foreground color
// string + set-flag (the AnalysePlugin FG-column pattern — an unset flag
// leaves the theme's default row color, so selection highlighting stays
// legible).
//
// Laziness: every container row gets one placeholder child so the expander
// shows; "test-expand-row" swaps it for the real children. NextZip's two
// hard-won rules apply verbatim: populate on test-expand-row (fires BEFORE
// the view expands; return FALSE to allow), and append the real rows BEFORE
// removing the placeholder — dropping the row to zero children mid-expand
// makes the view abandon the expansion.

#include "JsonPanel.h"

#include <cctype>
#include <cstring>
#include <functional>
#include <vector>

// ─── store columns ──────────────────────────────────────────────────────────
enum {
    COL_LABEL = 0,   // display text
    COL_NODE,        // const npj::JsonNode*  (nullptr on placeholder rows)
    COL_FG,          // foreground color string
    COL_FG_SET,      // whether COL_FG applies
    N_COLS
};

// Sentinel stored in COL_LABEL of lazy placeholder rows.
static const char* kPlaceholder = "__jv_placeholder__";

// Semantic per-type colors — near equivalents of the macOS system colors,
// picked to stay readable on both light and dark themes.
static const char* colorForType(npj::JsonNodeType t) {
    switch (t) {
        case npj::JsonNodeType::String: return "#2da44e";   // systemGreen
        case npj::JsonNodeType::Number: return "#3584e4";   // systemBlue
        case npj::JsonNodeType::Bool:   return "#e66100";   // systemOrange
        case npj::JsonNodeType::Null:   return "#86868b";   // systemGray
        default:                        return nullptr;      // theme default
    }
}

// ─── ctor / dtor ────────────────────────────────────────────────────────────

JsonPanel::JsonPanel(Delegate* delegate, const std::string& resourcesDir)
    : m_delegate(delegate) {
    buildLayout(resourcesDir);
}

JsonPanel::~JsonPanel() {
    if (m_ctxMenu) {
        gtk_widget_unparent(m_ctxMenu);
        m_ctxMenu = nullptr;
    }
    if (m_root) {
        g_object_unref(m_root);
        m_root = nullptr;
    }
}

// ─── layout ─────────────────────────────────────────────────────────────────

static GtkWidget* makePanelButton(const std::string& resourcesDir, const char* iconName,
                                  const char* tooltip, GCallback cb, gpointer self) {
    GtkWidget* b = gtk_button_new();
    std::string path = resourcesDir + "/" + iconName + ".png";
    GtkWidget* img = gtk_image_new_from_file(path.c_str());
    gtk_image_set_pixel_size(GTK_IMAGE(img), 11);   // 11px glyph like macOS
    gtk_button_set_child(GTK_BUTTON(b), img);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_set_size_request(b, 16, 16);
    g_signal_connect(b, "clicked", cb, self);
    return b;
}

void JsonPanel::buildLayout(const std::string& resourcesDir) {
    m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_object_ref_sink(m_root);
    gtk_widget_add_css_class(m_root, "npp-panel-content");

    // ── Toolbar row ─────────────────────────────────────────────────────
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_top(header, 4);
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 6);

    m_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(m_search), "Search in tree...");
    gtk_widget_set_hexpand(m_search, TRUE);
    g_signal_connect(m_search, "search-changed", G_CALLBACK(onSearchChanged), this);
    g_signal_connect(m_search, "stop-search",    G_CALLBACK(onSearchStop),    this);
    gtk_box_append(GTK_BOX(header), m_search);

    gtk_box_append(GTK_BOX(header),
        makePanelButton(resourcesDir, "jv_refresh", "Refresh JSON tree",
                        G_CALLBACK(dispatchRefresh), this));
    gtk_box_append(GTK_BOX(header),
        makePanelButton(resourcesDir, "jv_validate", "Validate JSON",
                        G_CALLBACK(dispatchValidate), this));
    gtk_box_append(GTK_BOX(header),
        makePanelButton(resourcesDir, "jv_format", "Format / Beautify JSON",
                        G_CALLBACK(dispatchFormat), this));
    gtk_box_append(GTK_BOX(m_root), header);

    // ── Tree ────────────────────────────────────────────────────────────
    m_store = gtk_tree_store_new(N_COLS,
                                 G_TYPE_STRING, G_TYPE_POINTER,
                                 G_TYPE_STRING, G_TYPE_BOOLEAN);
    m_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(m_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(m_tree), FALSE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(m_tree), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(m_tree), FALSE);  // our own search field

    GtkCellRenderer* r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(
        "node", r,
        "text", COL_LABEL,
        "foreground", COL_FG,
        "foreground-set", COL_FG_SET,
        NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(m_tree), col);

    // ~10pt rows like the macOS panel's default font size.
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "treeview.jv-tree { font-size: 10pt; }");
    gtk_widget_add_css_class(m_tree, "jv-tree");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(m_tree),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    g_signal_connect(m_tree, "test-expand-row", G_CALLBACK(onTestExpandRow), this);

    // Single left-click → jump to node (macOS outline action semantics:
    // click only, so arrow-keying the tree never steals editor focus).
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "released", G_CALLBACK(onRowReleased), this);
    gtk_widget_add_controller(m_tree, GTK_EVENT_CONTROLLER(click));

    // Right-click → context menu.
    GtkGesture* rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    g_signal_connect(rclick, "pressed", G_CALLBACK(onRightClick), this);
    gtk_widget_add_controller(m_tree, GTK_EVENT_CONTROLLER(rclick));

    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), m_tree);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(m_root), sw);

    // ── Context menu (GtkPopoverMenu + action group; items match Windows) ──
    m_ctxActions = g_simple_action_group_new();
    static const char* kActs[] = {"copy", "copy-name", "copy-value", "copy-path",
                                  "expand-all", "collapse-all"};
    for (const char* name : kActs) {
        GSimpleAction* a = g_simple_action_new(name, nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(ctxAction), this);
        g_action_map_add_action(G_ACTION_MAP(m_ctxActions), G_ACTION(a));
        g_object_unref(a);
    }
    gtk_widget_insert_action_group(m_tree, "jv", G_ACTION_GROUP(m_ctxActions));

    GMenu* menu = g_menu_new();
    GMenu* sec1 = g_menu_new();
    g_menu_append(sec1, "Copy", "jv.copy");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(sec1));
    GMenu* sec2 = g_menu_new();
    g_menu_append(sec2, "Copy name",  "jv.copy-name");
    g_menu_append(sec2, "Copy value", "jv.copy-value");
    g_menu_append(sec2, "Copy path",  "jv.copy-path");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(sec2));
    GMenu* sec3 = g_menu_new();
    g_menu_append(sec3, "Expand all",   "jv.expand-all");
    g_menu_append(sec3, "Collapse all", "jv.collapse-all");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(sec3));

    m_ctxMenu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_parent(m_ctxMenu, m_tree);
    gtk_popover_set_has_arrow(GTK_POPOVER(m_ctxMenu), FALSE);
    g_object_unref(sec1); g_object_unref(sec2); g_object_unref(sec3);
    g_object_unref(menu);
}

// ─── public API ─────────────────────────────────────────────────────────────

void JsonPanel::setTree(std::unique_ptr<npj::JsonNode> root) {
    // Clear the store FIRST while the old tree is still alive, so no view
    // callback can see a stale JsonNode* (macOS reloads before replacing).
    gtk_tree_store_clear(m_store);
    m_treeModel = std::move(root);
    m_placeholderText.clear();
    m_visibleSet.clear();
    m_filterText.clear();
    gtk_editable_set_text(GTK_EDITABLE(m_search), "");
    rebuildStore();
}

void JsonPanel::showPlaceholderMessage(const std::string& text) {
    gtk_tree_store_clear(m_store);
    m_treeModel.reset();
    m_visibleSet.clear();
    m_filterText.clear();
    gtk_editable_set_text(GTK_EDITABLE(m_search), "");
    m_placeholderText = text;

    GtkTreeIter it;
    gtk_tree_store_append(m_store, &it, nullptr);
    gtk_tree_store_set(m_store, &it,
                       COL_LABEL, text.c_str(),
                       COL_NODE, nullptr,
                       COL_FG, "#86868b", COL_FG_SET, TRUE,
                       -1);
}

// ─── store building ─────────────────────────────────────────────────────────

std::string JsonPanel::containerLabel(const npj::JsonNode* n) const {
    std::string label = n->key;
    label += " ";
    label += (n->type == npj::JsonNodeType::Array) ? "[" : "{";
    label += std::to_string(n->memberCount);
    label += (n->type == npj::JsonNodeType::Array) ? "]" : "}";
    return label;
}

void JsonPanel::appendNodeRow(GtkTreeIter* out, GtkTreeIter* parentIter,
                              const npj::JsonNode* n, bool withPlaceholder) {
    std::string label;
    if (n == m_treeModel.get()) {
        // Root label: literally "JSON" (no count suffix) — Windows parity.
        label = "JSON";
    } else if (n->type == npj::JsonNodeType::Object || n->type == npj::JsonNodeType::Array) {
        label = containerLabel(n);
    } else {
        label = npj::formatLeafLabel(*n);
    }

    const char* fg = (n == m_treeModel.get()) ? nullptr : colorForType(n->type);

    gtk_tree_store_append(m_store, out, parentIter);
    // Colorless rows store NULL, never "": the "foreground" attribute
    // binding sets the renderer property for EVERY row regardless of the
    // foreground-set flag, and GtkCellRendererText parses the string before
    // checking the flag — "" spams `Gtk-WARNING: Don't know color ''` on
    // each redraw, while NULL is handled as unset.
    gtk_tree_store_set(m_store, out,
                       COL_LABEL, label.c_str(),
                       COL_NODE, (gpointer)n,
                       COL_FG, fg,
                       COL_FG_SET, fg ? TRUE : FALSE,
                       -1);

    if (withPlaceholder && !n->children.empty()) {
        GtkTreeIter ph;
        gtk_tree_store_append(m_store, &ph, out);
        gtk_tree_store_set(m_store, &ph,
                           COL_LABEL, kPlaceholder,
                           COL_NODE, nullptr,
                           COL_FG, (const char *)nullptr, COL_FG_SET, FALSE,
                           -1);
    }
}

void JsonPanel::populateChildren(GtkTreeIter* parentIter, const npj::JsonNode* parent) {
    for (const auto& c : parent->children) {
        if (!m_filterText.empty() &&
            m_visibleSet.find(c.get()) == m_visibleSet.end())
            continue;
        GtkTreeIter it;
        appendNodeRow(&it, parentIter, c.get(), /*withPlaceholder=*/m_filterText.empty());
        if (!m_filterText.empty())
            populateChildren(&it, c.get());   // filtered mode is eager
    }
}

// Eagerly materialize a lazily-pending subtree (used by "Expand all", which
// cannot rely on test-expand-row firing for rows created mid-expansion).
void JsonPanel::populateEagerly(GtkTreeIter* parentIter, const npj::JsonNode* parent) {
    GtkTreeIter child;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(m_store), &child, parentIter)) {
        gchar* label = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(m_store), &child, COL_LABEL, &label, -1);
        bool pending = label && strcmp(label, kPlaceholder) == 0;
        g_free(label);
        if (pending) {
            // Real rows first, placeholder removed after (NextZip rule).
            for (const auto& c : parent->children) {
                GtkTreeIter it;
                appendNodeRow(&it, parentIter, c.get(), /*withPlaceholder=*/true);
            }
            gtk_tree_store_remove(m_store, &child);
        }
    }
    // Recurse over the (now real) child rows.
    GtkTreeIter it;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(m_store), &it, parentIter)) {
        std::size_t idx = 0;
        do {
            gpointer p = nullptr;
            gtk_tree_model_get(GTK_TREE_MODEL(m_store), &it, COL_NODE, &p, -1);
            const npj::JsonNode* n = (const npj::JsonNode*)p;
            if (n && !n->children.empty())
                populateEagerly(&it, n);
            ++idx;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(m_store), &it) && idx < 1000000);
    }
}

void JsonPanel::rebuildStore() {
    gtk_tree_store_clear(m_store);
    if (!m_treeModel) {
        if (!m_placeholderText.empty()) {
            GtkTreeIter it;
            gtk_tree_store_append(m_store, &it, nullptr);
            gtk_tree_store_set(m_store, &it,
                               COL_LABEL, m_placeholderText.c_str(),
                               COL_NODE, nullptr,
                               COL_FG, "#86868b", COL_FG_SET, TRUE, -1);
        }
        return;
    }

    if (!m_filterText.empty() &&
        m_visibleSet.find(m_treeModel.get()) == m_visibleSet.end()) {
        // Filter active but nothing matches — empty tree.
        return;
    }

    GtkTreeIter rootIt;
    if (m_filterText.empty()) {
        appendNodeRow(&rootIt, nullptr, m_treeModel.get(), /*withPlaceholder=*/true);
    } else {
        appendNodeRow(&rootIt, nullptr, m_treeModel.get(), /*withPlaceholder=*/false);
        populateChildren(&rootIt, m_treeModel.get());
    }

    if (m_filterText.empty()) {
        // Expand the root so the user sees immediate structure (macOS does
        // the same). Expanding fires test-expand-row → first level loads.
        GtkTreePath* p = gtk_tree_path_new_first();
        gtk_tree_view_expand_row(GTK_TREE_VIEW(m_tree), p, FALSE);
        gtk_tree_path_free(p);
    } else {
        gtk_tree_view_expand_all(GTK_TREE_VIEW(m_tree));
    }
}

// ─── lazy expansion ─────────────────────────────────────────────────────────

gboolean JsonPanel::onTestExpandRow(GtkTreeView*, GtkTreeIter* iter, GtkTreePath*,
                                    gpointer selfp) {
    JsonPanel* self = (JsonPanel*)selfp;
    GtkTreeModel* model = GTK_TREE_MODEL(self->m_store);

    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, iter)) return FALSE;

    gchar* label = nullptr;
    gtk_tree_model_get(model, &child, COL_LABEL, &label, -1);
    bool pending = label && strcmp(label, kPlaceholder) == 0;
    g_free(label);
    if (!pending) return FALSE;   // already populated

    gpointer p = nullptr;
    gtk_tree_model_get(model, iter, COL_NODE, &p, -1);
    const npj::JsonNode* n = (const npj::JsonNode*)p;
    if (!n) return FALSE;

    // Append the real rows BEFORE removing the placeholder — removing first
    // drops the row to zero children and the view abandons the expansion.
    self->populateChildren(iter, n);
    gtk_tree_store_remove(self->m_store, &child);
    return FALSE;   // allow the expansion
}

// ─── filter ─────────────────────────────────────────────────────────────────

bool JsonPanel::nodeMatchesFilter(const npj::JsonNode* n) const {
    if (m_filterText.empty()) return true;
    auto contains = [this](const std::string& s) -> bool {
        if (s.size() < m_filterText.size()) return false;
        for (std::size_t i = 0; i + m_filterText.size() <= s.size(); ++i) {
            bool hit = true;
            for (std::size_t j = 0; j < m_filterText.size(); ++j) {
                char a = std::tolower(static_cast<unsigned char>(s[i + j]));
                char b = std::tolower(static_cast<unsigned char>(m_filterText[j]));
                if (a != b) { hit = false; break; }
            }
            if (hit) return true;
        }
        return false;
    };
    return contains(n->key) || contains(n->value);
}

bool JsonPanel::populateVisible(const npj::JsonNode* n) {
    bool selfMatches = nodeMatchesFilter(n);
    bool anyChildVisible = false;
    for (const auto& c : n->children) {
        if (populateVisible(c.get())) anyChildVisible = true;
    }
    if (selfMatches || anyChildVisible) {
        m_visibleSet.insert(n);
        return true;
    }
    return false;
}

void JsonPanel::onSearchChanged(GtkSearchEntry* e, gpointer selfp) {
    JsonPanel* self = (JsonPanel*)selfp;
    const char* q = gtk_editable_get_text(GTK_EDITABLE(e));
    std::string trimmed = q ? q : "";
    // trim whitespace (macOS trims via whitespaceCharacterSet)
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front())) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))  trimmed.pop_back();

    self->m_filterText = trimmed;
    self->m_visibleSet.clear();
    if (!self->m_filterText.empty() && self->m_treeModel)
        self->populateVisible(self->m_treeModel.get());
    if (self->m_treeModel)
        self->rebuildStore();
}

void JsonPanel::onSearchStop(GtkSearchEntry* e, gpointer selfp) {
    // Escape clears the filter (macOS cancelOperation:).
    JsonPanel* self = (JsonPanel*)selfp;
    if (self->m_filterText.empty()) return;
    gtk_editable_set_text(GTK_EDITABLE(e), "");
    self->m_filterText.clear();
    self->m_visibleSet.clear();
    if (self->m_treeModel) self->rebuildStore();
}

// ─── row click → jump ───────────────────────────────────────────────────────

void JsonPanel::onRowReleased(GtkGestureClick* g, int, double x, double y, gpointer selfp) {
    JsonPanel* self = (JsonPanel*)selfp;
    GtkTreeView* tv = GTK_TREE_VIEW(self->m_tree);

    int bx = 0, by = 0;
    gtk_tree_view_convert_widget_to_bin_window_coords(tv, (int)x, (int)y, &bx, &by);

    GtkTreePath* path = nullptr;
    GtkTreeViewColumn* col = nullptr;
    if (!gtk_tree_view_get_path_at_pos(tv, bx, by, &path, &col, nullptr, nullptr))
        return;

    // Clicks in the expander gutter toggle the row — don't also jump.
    GdkRectangle cell = {};
    gtk_tree_view_get_cell_area(tv, path, col, &cell);
    if (bx < cell.x) { gtk_tree_path_free(path); return; }

    GtkTreeIter it;
    gpointer p = nullptr;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(self->m_store), &it, path))
        gtk_tree_model_get(GTK_TREE_MODEL(self->m_store), &it, COL_NODE, &p, -1);
    gtk_tree_path_free(path);

    const npj::JsonNode* n = (const npj::JsonNode*)p;
    if (!n || n == self->m_treeModel.get()) return;   // root has no position
    if (!n->pos.length) return;

    if (self->m_delegate)
        self->m_delegate->onSelectNode(n->pos.line, n->pos.column, n->pos.length);
}

// ─── context menu ───────────────────────────────────────────────────────────

const npj::JsonNode* JsonPanel::selectedNode() const {
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(m_tree));
    GtkTreeModel* model = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return nullptr;
    gpointer p = nullptr;
    gtk_tree_model_get(model, &it, COL_NODE, &p, -1);
    return (const npj::JsonNode*)p;
}

void JsonPanel::onRightClick(GtkGestureClick* g, int, double x, double y, gpointer selfp) {
    JsonPanel* self = (JsonPanel*)selfp;
    GtkTreeView* tv = GTK_TREE_VIEW(self->m_tree);

    // Select the row under the pointer first so the copy actions have a
    // valid target (macOS menuForEvent: does the same).
    int bx = 0, by = 0;
    gtk_tree_view_convert_widget_to_bin_window_coords(tv, (int)x, (int)y, &bx, &by);
    GtkTreePath* path = nullptr;
    if (gtk_tree_view_get_path_at_pos(tv, bx, by, &path, nullptr, nullptr, nullptr)) {
        gtk_tree_view_set_cursor(tv, path, nullptr, FALSE);
        gtk_tree_path_free(path);
    }

    // Enable/disable to match the Windows/macOS rules.
    const npj::JsonNode* n = self->selectedNode();
    bool isRoot      = (n == self->m_treeModel.get());
    bool isContainer = n && (n->type == npj::JsonNodeType::Object ||
                             n->type == npj::JsonNodeType::Array);
    auto setEnabled = [self](const char* name, bool on) {
        GAction* a = g_action_map_lookup_action(G_ACTION_MAP(self->m_ctxActions), name);
        if (a) g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
    };
    setEnabled("copy",         n != nullptr);
    setEnabled("copy-name",    n && !isRoot && !isContainer);
    setEnabled("copy-value",   n && !isRoot && !isContainer);
    setEnabled("copy-path",    n && !isRoot);
    setEnabled("expand-all",   n && isContainer);
    setEnabled("collapse-all", n && isContainer);

    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(self->m_ctxMenu), &r);
    gtk_popover_popup(GTK_POPOVER(self->m_ctxMenu));
}

void JsonPanel::copyToClipboard(const std::string& s) {
    if (s.empty()) return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(m_root), s.c_str());
}

void JsonPanel::ctxAction(GSimpleAction* a, GVariant*, gpointer selfp) {
    JsonPanel* self = (JsonPanel*)selfp;
    const char* name = g_action_get_name(G_ACTION(a));
    const npj::JsonNode* n = self->selectedNode();
    if (!n) return;

    if (strcmp(name, "copy") == 0) {
        bool isContainer = (n->type == npj::JsonNodeType::Object ||
                            n->type == npj::JsonNodeType::Array);
        self->copyToClipboard(isContainer ? self->containerLabel(n)
                                          : npj::formatLeafLabel(*n));
    } else if (strcmp(name, "copy-name") == 0) {
        self->copyToClipboard(n->key);
    } else if (strcmp(name, "copy-value") == 0) {
        self->copyToClipboard(n->value);
    } else if (strcmp(name, "copy-path") == 0) {
        if (n == self->m_treeModel.get()) return;
        // Ancestors root→parent via the parent chain, then drop the
        // synthetic root so paths start at the first real level.
        std::vector<const npj::JsonNode*> ancestors;
        for (const npj::JsonNode* p = n->parent; p; p = p->parent)
            ancestors.insert(ancestors.begin(), p);
        if (!ancestors.empty() && ancestors.front() == self->m_treeModel.get())
            ancestors.erase(ancestors.begin());
        self->copyToClipboard(npj::buildNodePath(ancestors, *n));
    } else if (strcmp(name, "expand-all") == 0 || strcmp(name, "collapse-all") == 0) {
        GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(self->m_tree));
        GtkTreeModel* model = nullptr;
        GtkTreeIter it;
        if (!gtk_tree_selection_get_selected(sel, &model, &it)) return;
        GtkTreePath* path = gtk_tree_model_get_path(model, &it);
        if (strcmp(name, "expand-all") == 0) {
            self->populateEagerly(&it, n);   // materialize pending rows first
            gtk_tree_view_expand_row(GTK_TREE_VIEW(self->m_tree), path, TRUE);
        } else {
            gtk_tree_view_collapse_row(GTK_TREE_VIEW(self->m_tree), path);
        }
        gtk_tree_path_free(path);
    }
}

// ─── header buttons ─────────────────────────────────────────────────────────

void JsonPanel::dispatchRefresh(GtkButton*, gpointer selfp) {
    JsonPanel* self = (JsonPanel*)selfp;
    if (self->m_delegate) self->m_delegate->onRefresh();
}
void JsonPanel::dispatchValidate(GtkButton*, gpointer selfp) {
    JsonPanel* self = (JsonPanel*)selfp;
    if (self->m_delegate) self->m_delegate->onValidate();
}
void JsonPanel::dispatchFormat(GtkButton*, gpointer selfp) {
    JsonPanel* self = (JsonPanel*)selfp;
    if (self->m_delegate) self->m_delegate->onFormat();
}
