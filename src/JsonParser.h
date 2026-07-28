// JsonParser — SAX-based JSON tree builder for NppJsonViewer (macOS port).
//
// Mirrors the Windows plugin's RapidJsonHandler + TrackingStream design (see
// /Users/leto/development/npp/nppPluginsWin64/JSON-Viewer/src/NppJsonViewer/
// RapidJsonHandler.{h,cpp} and TrackingStream.h) but builds a pure C++
// node tree instead of eagerly inserting HTREEITEMs into a Win32 TreeView.
//
// The node tree is then handed to an NSOutlineView data source, which is
// lazy — only visible rows are queried — so huge documents render in
// constant time relative to viewport rather than proportional to node
// count. On a 10 MB JSON doc this drops end-to-end time from multi-second
// (Win TVM_INSERTITEM per node) to sub-300 ms (single parse + single
// reloadData).
//
// Semantics preserved from the Windows port:
//   * Numbers parsed via RawNumber (kParseNumbersAsStringsFlag) so original
//     textual formatting survives round-trip
//   * Line/column tracking on every Take() — surfaced per-key AND per
//     array-item value so click-to-jump lands accurately
//   * kParseComments / kParseTrailingCommas configurable from settings
//   * undefined -> null pre-pass applied by the caller before Parse() if
//     the setting is enabled (matches Windows eMethod::FormatJson recovery
//     branch)

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstddef>
#include <cstdint>

namespace npj {

enum class JsonNodeType : uint8_t {
    Unknown,
    Null,
    Bool,
    Number,   // RawNumber passthrough; original text preserved
    String,
    Array,
    Object,
};

struct Position {
    std::size_t line   = 0;   // 0-based, referring to the KEY token's line in the source
    std::size_t column = 0;   // 0-based column where the key (or value for array items) starts
    std::size_t length = 0;   // length of the key or value in bytes (UTF-8)
};

// Tree node — owns its children. Position is populated only for nodes that
// map to a single token the user might want to navigate to (keys of object
// members; values of array elements). The synthetic ROOT node has no
// meaningful position.
struct JsonNode {
    JsonNodeType                              type    = JsonNodeType::Unknown;
    std::string                               key;          // "" for array items / root
    std::string                               value;        // primitive value; empty for containers
    Position                                  pos;
    std::vector<std::unique_ptr<JsonNode>>    children;
    // Raw back-pointer to the owning parent, or nullptr for the root.
    // Non-owning — the tree is a strict DAG rooted at the unique_ptr
    // chain, so as long as the caller holds the root this ptr is live.
    // Used by JsonPanel to draw tree-guideline connectors (we need to
    // walk up the ancestor chain to decide which indent columns should
    // carry a continuing dashed line past a given row).
    JsonNode                                 *parent = nullptr;
    // For containers, populated after EndObject/EndArray — the count we
    // show in the tree label as "{N}" or "[N]".
    std::uint32_t                             memberCount = 0;
};

struct ParseOptions {
    bool ignoreComments      = true;
    bool ignoreTrailingComma = true;
};

enum class ParseStatus : uint8_t {
    Ok,
    Error,
    Empty,        // empty input — treat as parse failure with friendly message
};

struct ParseResult {
    ParseStatus                status   = ParseStatus::Error;
    std::unique_ptr<JsonNode>  root;                   // valid only when status == Ok
    // When status == Error:
    int                        errorCode     = -1;     // rapidjson::ParseErrorCode value
    std::size_t                errorOffset   = 0;      // byte offset where the parser choked
    std::string                errorMessage;           // human-readable (English)
};

// Parse jsonText into a tree. Returns ParseResult with either a valid root
// or error info. Never throws. Never mutates jsonText.
ParseResult parseJson(const std::string& jsonText, const ParseOptions& opts);

// If the caller wants the undefined→null recovery pass (Windows plugin's
// "Replace value 'undefined' with 'null'" setting), it can run this on
// the input before re-parsing. Matches the Windows regex exactly.
std::string replaceUndefinedWithNull(const std::string& jsonText);

// Build a JSONPath-style string for a node, walking up via its parent chain.
// Returns e.g. "meta.lastTouchedAt" or "wizard.steps[3].name". Caller must
// supply the ancestors from root to (but not including) the target node.
// Matches Windows TreeViewCtrl::GetNodePath output shape.
std::string buildNodePath(const std::vector<const JsonNode*>& ancestors, const JsonNode& node);

// Returns "key : value" form used as the tree-view label for a leaf node,
// with quotes around string values (matching Windows output). For containers
// the caller renders "key {N}" / "[idx] [N]" separately.
std::string formatLeafLabel(const JsonNode& node);

} // namespace npj
