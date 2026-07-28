// JsonFormatter implementation — uses rapidjson::Document for sort (needs
// key access) and rapidjson::Writer / PrettyWriter for compress / format.
//
// RapidJSON's PrettyWriter accepts a LineEndingOption + PrettyFormatOptions
// + SetIndent(char, count), which lets us cover all three setting groups
// in the spec. For sort-by-key, we parse into a Document, recursively
// sort object keys alphabetically (arrays are traversed but not reordered),
// then serialize with the same PrettyWriter.
//
// Parse flags are the identical ones used by parseJson(); kept DRY via a
// dispatch helper.

#include "JsonFormatter.h"

#include <algorithm>
#include <cstring>
#include <regex>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace rj = rapidjson;

namespace npj {

// ─── Flag dispatch ───────────────────────────────────────────────────────
namespace {

constexpr unsigned kBaseFlags = rj::kParseEscapedApostropheFlag
                              | rj::kParseNanAndInfFlag
                              | rj::kParseNumbersAsStringsFlag;

template <typename Parser>
bool parseWithOptions(Parser& p, const std::string& text, const ParseOptions& opts) {
    constexpr unsigned c = kBaseFlags | rj::kParseCommentsFlag;
    constexpr unsigned k = kBaseFlags | rj::kParseTrailingCommasFlag;
    constexpr unsigned b = c | rj::kParseTrailingCommasFlag;

    const char* cstr = text.c_str();
    if (opts.ignoreComments && opts.ignoreTrailingComma) return !p.template Parse<b>(cstr).HasParseError();
    if (opts.ignoreComments)                              return !p.template Parse<c>(cstr).HasParseError();
    if (opts.ignoreTrailingComma)                         return !p.template Parse<k>(cstr).HasParseError();
    return !p.template Parse<kBaseFlags>(cstr).HasParseError();
}

// Reader-based parse (used when we don't need a DOM, e.g. format/compress).
template <typename Reader, typename Handler, typename Stream>
bool streamParseWithOptions(Reader& r, Stream& s, Handler& h, const ParseOptions& opts) {
    constexpr unsigned c = kBaseFlags | rj::kParseCommentsFlag;
    constexpr unsigned k = kBaseFlags | rj::kParseTrailingCommasFlag;
    constexpr unsigned b = c | rj::kParseTrailingCommasFlag;

    if (opts.ignoreComments && opts.ignoreTrailingComma) return r.template Parse<b>(s, h);
    if (opts.ignoreComments)                              return r.template Parse<c>(s, h);
    if (opts.ignoreTrailingComma)                         return r.template Parse<k>(s, h);
    return r.template Parse<kBaseFlags>(s, h);
}

rj::LineEndingOption toRjEol(LineEnding e) {
    switch (e) {
        case LineEnding::Windows:   return rj::kCrLf;
        case LineEnding::Unix:      return rj::kLf;
        case LineEnding::Macintosh: return rj::kCr;
        case LineEnding::Auto:      break;
    }
    return rj::kLf;   // sensible default on macOS when "auto" resolves
}

rj::PrettyFormatOptions toRjLineFormat(LineFormat f) {
    switch (f) {
        case LineFormat::SingleLineArrays: return rj::kFormatSingleLineArray;
        case LineFormat::Default:          break;
    }
    return rj::kFormatDefault;
}

std::pair<char, unsigned> resolveIndent(const FormatOptions& o) {
    switch (o.indent) {
        case IndentStyle::Tab:   return {'\t', 1};
        case IndentStyle::Space: return {' ', o.indentCount ? o.indentCount : 4};
        case IndentStyle::Auto:  break;
    }
    return {' ', o.indentCount ? o.indentCount : 4};
}

std::string prepareInput(const std::string& text, const FormatOptions& o) {
    if (o.replaceUndefined) return replaceUndefinedWithNull(text);
    return text;
}

FormatResult makeError(const std::string& reason, int code, std::size_t offset) {
    FormatResult r;
    r.success      = false;
    r.errorCode    = code;
    r.errorOffset  = offset;
    r.errorMessage = reason;
    return r;
}

} // namespace

// ─── Format (pretty-print) ───────────────────────────────────────────────
FormatResult formatJson(const std::string& jsonText, const FormatOptions& opts) {
    std::string input = prepareInput(jsonText, opts);

    rj::StringBuffer sb;
    rj::PrettyWriter<rj::StringBuffer,
                     rj::UTF8<>, rj::UTF8<>, rj::CrtAllocator,
                     rj::kWriteNanAndInfFlag> writer(sb);

    writer.SetLineEnding(toRjEol(opts.eol));
    writer.SetFormatOptions(toRjLineFormat(opts.format));
    auto [ch, n] = resolveIndent(opts);
    writer.SetIndent(ch, n);

    rj::StringStream ss(input.c_str());
    rj::Reader reader;
    bool ok = streamParseWithOptions(reader, ss, writer, opts.parse);
    if (!ok) {
        return makeError(rj::GetParseError_En(reader.GetParseErrorCode()),
                         static_cast<int>(reader.GetParseErrorCode()),
                         reader.GetErrorOffset());
    }

    FormatResult out;
    out.success = true;
    out.output  = sb.GetString();
    return out;
}

// ─── Compress (minify) ───────────────────────────────────────────────────
FormatResult compressJson(const std::string& jsonText, const FormatOptions& opts) {
    std::string input = prepareInput(jsonText, opts);

    rj::StringBuffer sb;
    rj::Writer<rj::StringBuffer,
               rj::UTF8<>, rj::UTF8<>, rj::CrtAllocator,
               rj::kWriteNanAndInfFlag> writer(sb);

    rj::StringStream ss(input.c_str());
    rj::Reader reader;
    bool ok = streamParseWithOptions(reader, ss, writer, opts.parse);
    if (!ok) {
        return makeError(rj::GetParseError_En(reader.GetParseErrorCode()),
                         static_cast<int>(reader.GetParseErrorCode()),
                         reader.GetErrorOffset());
    }

    FormatResult out;
    out.success = true;
    out.output  = sb.GetString();
    return out;
}

// ─── Sort by key (DOM-based) ─────────────────────────────────────────────
static void sortObjectRecursive(rj::Value& v, rj::Document::AllocatorType& alloc) {
    if (v.IsObject()) {
        std::vector<std::string> keys;
        keys.reserve(v.MemberCount());
        for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
            keys.emplace_back(it->name.GetString(), it->name.GetStringLength());
        }
        std::sort(keys.begin(), keys.end());

        rj::Value sorted(rj::kObjectType);
        for (const std::string& k : keys) {
            rj::Value name(k.c_str(), static_cast<rj::SizeType>(k.size()), alloc);
            auto it = v.FindMember(rj::Value(k.c_str(), static_cast<rj::SizeType>(k.size())));
            if (it != v.MemberEnd()) {
                // move the value so we don't deep-copy large subtrees
                sorted.AddMember(name, it->value, alloc);
            }
        }
        v = std::move(sorted);

        // Recurse into the freshly-ordered members
        for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
            sortObjectRecursive(it->value, alloc);
        }
    } else if (v.IsArray()) {
        for (rj::SizeType i = 0; i < v.Size(); ++i) {
            sortObjectRecursive(v[i], alloc);
        }
    }
}

FormatResult sortJsonByKey(const std::string& jsonText, const FormatOptions& opts) {
    std::string input = prepareInput(jsonText, opts);

    rj::Document doc;
    if (!parseWithOptions(doc, input, opts.parse)) {
        return makeError(rj::GetParseError_En(doc.GetParseError()),
                         static_cast<int>(doc.GetParseError()),
                         doc.GetErrorOffset());
    }

    sortObjectRecursive(doc, doc.GetAllocator());

    rj::StringBuffer sb;
    rj::PrettyWriter<rj::StringBuffer,
                     rj::UTF8<>, rj::UTF8<>, rj::CrtAllocator,
                     rj::kWriteNanAndInfFlag> writer(sb);
    writer.SetLineEnding(toRjEol(opts.eol));
    writer.SetFormatOptions(toRjLineFormat(opts.format));
    auto [ch, n] = resolveIndent(opts);
    writer.SetIndent(ch, n);
    doc.Accept(writer);

    FormatResult out;
    out.success = true;
    out.output  = sb.GetString();
    return out;
}

} // namespace npj
