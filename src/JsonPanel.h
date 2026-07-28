// JsonPanel — content widget for the JSON Viewer plugin (GTK4 port of the
// macOS JsonPanel NSView).
//
// Two-section layout matching the host's side-panel standard:
//
//   [ search field  ·  [refresh] [validate] [format] ]
//   ─────────────────────────────────────────────────
//   [ GtkTreeView — JSON tree, lazily populated from the JsonNode model ]
//
// Does not render its own title bar or close X — the host panel_frame
// supplies those when the widget is registered via NPPM_DMM_REGISTERPANEL.
//
// macOS uses a lazy NSOutlineView data source; GtkTreeStore is eager, so
// laziness is recreated with the placeholder-child pattern (populate on
// "test-expand-row", append real rows BEFORE removing the placeholder).
// A non-empty search filter switches to an eager build of only the visible
// subtree (matches + ancestors), auto-expanded — same semantics as the
// macOS _visibleSet.

#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <unordered_set>

#include "JsonParser.h"

class JsonPanel {
public:
    // Same shape as the macOS JsonPanelDelegate protocol.
    struct Delegate {
        virtual ~Delegate() = default;
        // User clicked a tree node. line/column are 0-based within the parsed
        // text; length is the key/value token length in bytes.
        virtual void onSelectNode(std::size_t line, std::size_t column, std::size_t length) = 0;
        virtual void onRefresh()  = 0;
        virtual void onValidate() = 0;
        virtual void onFormat()   = 0;
    };

    JsonPanel(Delegate* delegate, const std::string& resourcesDir);
    ~JsonPanel();

    // The widget to hand to NPPM_DMM_REGISTERPANEL. Owned by this object
    // (one strong ref) — the host takes its own ref when docking.
    GtkWidget* root() const { return m_root; }

    // Install a fresh tree model (pass nullptr to clear).
    void setTree(std::unique_ptr<npj::JsonNode> root);

    // Show a placeholder message row (e.g. parse error details).
    void showPlaceholderMessage(const std::string& text);

private:
    static void dispatchRefresh(GtkButton*, gpointer self);
    static void dispatchValidate(GtkButton*, gpointer self);
    static void dispatchFormat(GtkButton*, gpointer self);

    void buildLayout(const std::string& resourcesDir);
    void rebuildStore();
    void populateChildren(GtkTreeIter* parentIter, const npj::JsonNode* parent);
    void populateEagerly(GtkTreeIter* parentIter, const npj::JsonNode* parent);
    void appendNodeRow(GtkTreeIter* out, GtkTreeIter* parentIter, const npj::JsonNode* n,
                       bool withPlaceholder);
    bool nodeMatchesFilter(const npj::JsonNode* n) const;
    bool populateVisible(const npj::JsonNode* n);
    const npj::JsonNode* selectedNode() const;
    void copyToClipboard(const std::string& s);
    std::string containerLabel(const npj::JsonNode* n) const;
    void collapseRecursive(GtkTreeIter* iter);

    // Signal thunks
    static gboolean onTestExpandRow(GtkTreeView*, GtkTreeIter*, GtkTreePath*, gpointer self);
    static void     onSearchChanged(GtkSearchEntry*, gpointer self);
    static void     onSearchStop(GtkSearchEntry*, gpointer self);
    static void     onRowReleased(GtkGestureClick*, int, double, double, gpointer self);
    static void     onRightClick(GtkGestureClick*, int, double, double, gpointer self);
    static void     ctxAction(GSimpleAction*, GVariant*, gpointer self);

    Delegate*   m_delegate = nullptr;

    GtkWidget*  m_root        = nullptr;   // vertical GtkBox
    GtkWidget*  m_search      = nullptr;   // GtkSearchEntry
    GtkWidget*  m_tree        = nullptr;   // GtkTreeView
    GtkTreeStore* m_store     = nullptr;
    GMenu*      m_ctxModel    = nullptr;   // menu model (popover built per popup)
    GSimpleActionGroup* m_ctxActions = nullptr;

    std::unique_ptr<npj::JsonNode> m_treeModel;
    std::string m_placeholderText;
    std::string m_filterText;
    // Visible set under an active filter: node shown iff itself or any
    // descendant matches (macOS _visibleSet).
    std::unordered_set<const npj::JsonNode*> m_visibleSet;
};
