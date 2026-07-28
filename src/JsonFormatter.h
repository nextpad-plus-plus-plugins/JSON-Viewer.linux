// JsonFormatter — DOM-based format / compress / sort-by-key operations.
// Mirrors Windows JsonHandler::{GetCompressedJson, FormatJson, SortJsonByKey}
// using RapidJSON's Document / Writer / PrettyWriter.

#pragma once

#include <string>
#include "JsonParser.h"   // for ParseOptions

namespace npj {

enum class LineEnding : uint8_t {
    Auto,      // match editor's current EOL mode
    Windows,   // CRLF
    Unix,      // LF
    Macintosh, // CR
};

enum class LineFormat : uint8_t {
    Default,        // PrettyWriter default — each element on its own line
    SingleLineArrays, // inline arrays that contain only primitives
};

enum class IndentStyle : uint8_t {
    Auto,   // match editor setting (tab vs space + width)
    Space,
    Tab,
};

struct FormatOptions {
    ParseOptions parse;
    LineEnding   eol    = LineEnding::Auto;
    LineFormat   format = LineFormat::Default;
    IndentStyle  indent = IndentStyle::Auto;
    unsigned     indentCount = 4;
    // When the "replace undefined with null" setting is on, caller flips
    // this to true and format/compress runs the pre-pass before parsing.
    bool         replaceUndefined = false;
};

struct FormatResult {
    bool        success = false;
    std::string output;       // populated on success
    int         errorCode     = -1;
    std::size_t errorOffset   = 0;
    std::string errorMessage;
};

// Pretty-print jsonText to `output`, respecting all format options.
FormatResult formatJson  (const std::string& jsonText, const FormatOptions& opts);

// Minify jsonText to one-line form (preserves RawNumber exactness).
FormatResult compressJson(const std::string& jsonText, const FormatOptions& opts);

// Recursively sort object keys alphabetically (stable), then pretty-print.
FormatResult sortJsonByKey(const std::string& jsonText, const FormatOptions& opts);

} // namespace npj
