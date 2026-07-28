// JsonParser implementation. Uses RapidJSON SAX (Reader::Parse<Flags>) to
// stream-parse and build a JsonNode tree without ever materializing a
// DOM. Mirrors RapidJsonHandler.{h,cpp} + TrackingStream.h from the
// Windows plugin but with these differences:
//
//   1. Emits a std::vector<std::unique_ptr<JsonNode>> tree instead of
//      HTREEITEM nodes → no per-node UI message pump, so total time is
//      dominated by the parse itself.
//
//   2. Reference-stable parent chain via `std::stack<JsonNode*>` that
//      points into heap-allocated children[] slots. Keeps the invariant:
//      once a JsonNode is pushed onto a parent's `children`, its address
//      is stable for the entire parse — we never reallocate children
//      arrays in a way that invalidates the pointer because each level
//      pushes to its own vector and we only push to the CURRENT TOP.
//
//   3. Position tracking matches Windows semantics precisely: for object
//      members, the Position refers to the KEY token (so click-to-jump
//      lands on the key in the source). For array items that are
//      primitives, the Position refers to the VALUE token (there's no
//      key). Objects/arrays use the Position of their first token
//      ("{" or "[") — but since the tree row shows the parent's key
//      label, this rarely gets used.

#include "JsonParser.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <regex>
#include <stack>

#include <rapidjson/error/en.h>
#include <rapidjson/reader.h>

namespace rj = rapidjson;

namespace npj {

// ─────────────────────────────────────────────────────────────────────────
// TrackingStream — wraps rapidjson::StringStream and counts line/column.
// Every Take() advances: column++ in the normal case, (line++, column=0)
// on '\n'. We deliberately do NOT distinguish CR/LF pairs because Scintilla
// gives us the raw bytes and rapidjson treats both as whitespace for the
// grammar; the column math just needs to be consistent with what the
// editor will compute via SCI_POSITIONFROMLINE + offset.
// ─────────────────────────────────────────────────────────────────────────
class TrackingStream {
public:
    using Ch = char;

    explicit TrackingStream(const std::string& src) : m_ss(src.c_str()) {}

    // Current 0-based line of the NEXT character (after any Takes so far).
    std::size_t line()   const { return m_line; }
    std::size_t column() const { return m_column; }

    // rapidjson stream interface -----------------------------------------
    Ch Peek() const { return m_ss.Peek(); }

    Ch Take() {
        Ch c = m_ss.Take();
        if (c == '\n') { ++m_line; m_column = 0; }
        else           { ++m_column; }
        return c;
    }

    std::size_t Tell() const      { return m_ss.Tell(); }
    Ch*         PutBegin()        { return m_ss.PutBegin(); }
    std::size_t PutEnd(Ch* pCh)   { return m_ss.PutEnd(pCh); }
    void        Put(Ch ch)        { m_ss.Put(ch); }

private:
    rj::StringStream m_ss;
    std::size_t      m_line   = 0;
    std::size_t      m_column = 0;
};

// ─────────────────────────────────────────────────────────────────────────
// Handler — SAX callbacks produce a JsonNode tree. Mirrors the Windows
// RapidJsonHandler closely; the stack stores raw JsonNode* pointing into
// the children vectors of their parents, plus a counter for array-index
// generation.
// ─────────────────────────────────────────────────────────────────────────
class Handler : public rj::BaseReaderHandler<rj::UTF8<>, Handler> {
public:
    Handler(JsonNode* root, TrackingStream* ts) : m_root(root), m_ts(ts) {
        // Stack starts empty — the first Start{Object,Array} / scalar
        // callback takes ownership of the root directly. Matches Windows
        // RapidJsonHandler which also treats the outermost container as
        // the "JSON" root itself, so the tree doesn't grow an extra
        // synthetic level underneath it.
    }

    // Null, booleans, numbers, strings -----------------------------------
    bool Null()                                              { return emitPrimitive(JsonNodeType::Null,   "null",  /*isQuoted=*/false); }
    bool Bool(bool b)                                        { return emitPrimitive(JsonNodeType::Bool,   b ? "true" : "false", false); }
    bool Int(int)                                            { return true; }    // numbers go through RawNumber
    bool Uint(unsigned)                                      { return true; }
    bool Int64(std::int64_t)                                 { return true; }
    bool Uint64(std::uint64_t)                               { return true; }
    bool Double(double)                                      { return true; }
    bool RawNumber(const char* s, unsigned n, bool /*copy*/) { return emitPrimitive(JsonNodeType::Number, std::string(s, n), false); }

    bool String(const char* s, unsigned n, bool /*copy*/) {
        return emitPrimitive(JsonNodeType::String, std::string(s, n), /*isQuoted=*/true);
    }

    bool Key(const char* s, unsigned n, bool /*copy*/) {
        m_pendingKey.assign(s, n);
        // Position of the KEY — at the moment RapidJSON fires this callback,
        // m_ts->column() is *past* the closing quote. Back up length+2 (one
        // for each surrounding quote) to land on the opening quote column.
        // Matches Windows code: nColumn = column - length - 1 (their
        // formula assumes column points at first char of the closing quote;
        // we land at the char AFTER it, so the off-by-one is absorbed).
        std::size_t col = m_ts->column();
        m_pendingKeyPos.line   = m_ts->line();
        m_pendingKeyPos.column = (col >= n + 1) ? (col - n - 1) : 0;
        m_pendingKeyPos.length = n;
        m_pendingKeyValid = true;
        return true;
    }

    bool StartObject() { return enterContainer(JsonNodeType::Object); }
    bool StartArray()  { return enterContainer(JsonNodeType::Array);  }

    bool EndObject(unsigned memberCount) { return exitContainer(memberCount); }
    bool EndArray (unsigned memberCount) { return exitContainer(memberCount); }

private:
    struct Frame {
        JsonNode* node    = nullptr;
        bool      isArray = false;
        std::uint32_t index = 0;   // used to label array items "[0]", "[1]", ...
    };

    // Emit a primitive value (null / bool / number / string). If we are
    // inside an object the key has been set via Key(); if inside an array
    // we synthesize "[N]" as the key. For a bare scalar document
    // (e.g. just `42` or `"hello"`) we add a single child under the root
    // so the tree displays "JSON" → "<value>" — matches Windows
    // RapidJsonHandler::String's empty-stack branch.
    bool emitPrimitive(JsonNodeType t, std::string valueText, bool isQuoted) {
        if (m_stack.empty()) {
            auto child = std::make_unique<JsonNode>();
            child->type   = t;
            child->value  = std::move(valueText);
            child->parent = m_root;
            std::size_t valLen  = child->value.size();
            std::size_t col     = m_ts->column();
            std::size_t adjust  = valLen + (isQuoted ? 1 : 0);
            child->pos.line    = m_ts->line();
            child->pos.column  = (col >= adjust) ? (col - adjust) : 0;
            child->pos.length  = valLen;
            m_root->children.push_back(std::move(child));
            return true;
        }
        Frame& top = m_stack.top();

        auto child = std::make_unique<JsonNode>();
        child->type  = t;
        child->value = std::move(valueText);

        if (top.isArray) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "[%u]", top.index);
            child->key = buf;

            // Position of the value token — column is past the end of the
            // literal (and for strings, past the closing quote), so back up.
            std::size_t valLen  = child->value.size();
            std::size_t col     = m_ts->column();
            std::size_t adjust  = valLen + (isQuoted ? 1 : 0);
            child->pos.line   = m_ts->line();
            child->pos.column = (col >= adjust) ? (col - adjust) : 0;
            child->pos.length = valLen;

            ++top.index;
        } else {
            if (m_pendingKeyValid) {
                child->key = std::move(m_pendingKey);
                child->pos = m_pendingKeyPos;
                m_pendingKey.clear();
                m_pendingKeyValid = false;
            }
        }

        child->parent = top.node;
        top.node->children.push_back(std::move(child));
        return true;
    }

    bool enterContainer(JsonNodeType t) {
        if (m_stack.empty()) {
            // Outermost container IS the root — don't create a child,
            // just set the root's type and push it onto the stack. This
            // matches the Windows plugin's RapidJsonHandler::StartObject
            // early-return path so the tree ends up with the document's
            // top-level members as direct children of "JSON".
            m_root->type = t;
            Frame f;
            f.node    = m_root;
            f.isArray = (t == JsonNodeType::Array);
            f.index   = 0;
            m_stack.push(f);
            return true;
        }

        Frame& top = m_stack.top();

        auto child = std::make_unique<JsonNode>();
        child->type   = t;
        child->parent = top.node;

        if (top.isArray) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "[%u]", top.index);
            child->key = buf;
            ++top.index;
        } else if (m_pendingKeyValid) {
            child->key = std::move(m_pendingKey);
            child->pos = m_pendingKeyPos;
            m_pendingKey.clear();
            m_pendingKeyValid = false;
        }

        JsonNode* childPtr = child.get();
        top.node->children.push_back(std::move(child));

        Frame f;
        f.node    = childPtr;
        f.isArray = (t == JsonNodeType::Array);
        f.index   = 0;
        m_stack.push(f);
        return true;
    }

    bool exitContainer(unsigned memberCount) {
        if (m_stack.empty()) return false;
        Frame f = m_stack.top();
        m_stack.pop();
        f.node->memberCount = memberCount;
        return true;
    }

    JsonNode*         m_root = nullptr;    // non-owning; lives as long as parseJson call
    TrackingStream*   m_ts = nullptr;
    std::stack<Frame> m_stack;

    // Pending key (object context only).
    std::string m_pendingKey;
    Position    m_pendingKeyPos;
    bool        m_pendingKeyValid = false;
};

// ─────────────────────────────────────────────────────────────────────────
// Top-level parseJson — picks the right parse-flag combination from
// options and dispatches. Matches the 4-way branch in Windows
// JsonHandler::ParseJson.
// ─────────────────────────────────────────────────────────────────────────
static bool s_stringTrim(const std::string& s) {
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// Strip UTF-8 BOM if present so it doesn't trip up the parser (RapidJSON
// doesn't silently skip the BOM for StringStream inputs).
static const char* skipUtf8Bom(const std::string& s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF
                      && static_cast<unsigned char>(s[1]) == 0xBB
                      && static_cast<unsigned char>(s[2]) == 0xBF) {
        return s.c_str() + 3;
    }
    return s.c_str();
}

ParseResult parseJson(const std::string& jsonText, const ParseOptions& opts) {
    ParseResult out;

    if (jsonText.empty() || s_stringTrim(jsonText)) {
        out.status = ParseStatus::Empty;
        out.errorMessage = "The text is empty.";
        return out;
    }

    // RapidJSON SAX reader needs a stream it can Take() from. We wrap a
    // mutable copy of jsonText so we can strip a BOM without mutating the
    // caller's string.
    std::string textCopy(skipUtf8Bom(jsonText));
    TrackingStream ts(textCopy);

    auto root = std::make_unique<JsonNode>();
    // Type is set by the SAX handler: Object/Array for containers,
    // or a primitive type for bare-scalar documents. Key stays empty
    // (synthetic root has no name — the UI renders it as "JSON").
    Handler handler(root.get(), &ts);

    constexpr unsigned kBase = rj::kParseEscapedApostropheFlag
                             | rj::kParseNanAndInfFlag
                             | rj::kParseNumbersAsStringsFlag;
    constexpr unsigned kWithComments = kBase | rj::kParseCommentsFlag;
    constexpr unsigned kWithCommas   = kBase | rj::kParseTrailingCommasFlag;
    constexpr unsigned kWithBoth     = kWithComments | rj::kParseTrailingCommasFlag;

    rj::Reader reader;
    bool ok = false;
    if (opts.ignoreComments && opts.ignoreTrailingComma) {
        ok = reader.Parse<kWithBoth>(ts, handler);
    } else if (opts.ignoreComments) {
        ok = reader.Parse<kWithComments>(ts, handler);
    } else if (opts.ignoreTrailingComma) {
        ok = reader.Parse<kWithCommas>(ts, handler);
    } else {
        ok = reader.Parse<kBase>(ts, handler);
    }

    if (!ok) {
        out.status       = ParseStatus::Error;
        out.errorCode    = static_cast<int>(reader.GetParseErrorCode());
        out.errorOffset  = reader.GetErrorOffset();
        out.errorMessage = rj::GetParseError_En(reader.GetParseErrorCode());
        return out;
    }

    // The synthetic root stays — Windows always shows "JSON" as the top
    // label of the tree, with the parsed document as its first-level
    // children. The single-child promotion we used to do here broke that
    // visual, so the root is preserved as-is. JsonPanel renders this
    // synthetic node as literally "JSON".
    out.status = ParseStatus::Ok;
    out.root   = std::move(root);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────
// undefined → null pre-pass. Matches Windows regex verbatim:
//     ([:\[,])([\s]*?)undefined([\s,}]*?)  ->  $1$2null$3
// Only runs when caller (Format/Compress) opted in via the setting.
// ─────────────────────────────────────────────────────────────────────────
std::string replaceUndefinedWithNull(const std::string& jsonText) {
    static const std::regex kRe(R"(([:\[,])([\s]*?)undefined([\s,}]*?))",
                                std::regex::icase);
    return std::regex_replace(jsonText, kRe, "$1$2null$3");
}

// ─────────────────────────────────────────────────────────────────────────
// buildNodePath — walk ancestors + node, emit dot-separated path with
// array indexes attached to the previous key (no separator before a
// bracket). Matches Windows GetNodePath behavior.
// ─────────────────────────────────────────────────────────────────────────
std::string buildNodePath(const std::vector<const JsonNode*>& ancestors, const JsonNode& node) {
    std::string path;
    auto append = [&path](const std::string& key) {
        if (key.empty()) return;
        if (!key.empty() && key.front() == '[') {
            // "[N]" — no dot separator
            path += key;
        } else {
            if (!path.empty()) path += '.';
            path += key;
        }
    };
    for (const JsonNode* a : ancestors) append(a->key);
    append(node.key);
    return path;
}

// ─────────────────────────────────────────────────────────────────────────
// formatLeafLabel — what shows up in the tree cell for a non-container
// row. Matches Windows RapidJsonHandler::InsertToTree formatting (strings
// get quotes around the value, everything else is bare).
// ─────────────────────────────────────────────────────────────────────────
std::string formatLeafLabel(const JsonNode& node) {
    std::string label = node.key;
    label += " : ";
    if (node.type == JsonNodeType::String) {
        label += '"';
        label += node.value;
        label += '"';
    } else {
        label += node.value;
    }
    return label;
}

} // namespace npj
