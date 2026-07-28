// Settings model + JSON persistence. Matches the Windows INI keys (see
// Define.h STR_INI_* constants) but serializes as JSON so we reuse
// NSJSONSerialization.

#pragma once

#include <string>
#include "JsonFormatter.h"  // LineEnding, LineFormat, IndentStyle

namespace npj {

struct Settings {
    // Editor/UX flags
    bool followCurrentTab   = false;
    bool autoFormatOnOpen   = false;
    bool ignoreTrailingComma = true;
    bool ignoreComments     = true;
    bool useJsonHighlight   = true;
    bool replaceUndefined   = false;

    // Formatting
    IndentStyle indent      = IndentStyle::Auto;
    unsigned    indentCount = 4;
    LineEnding  eol         = LineEnding::Auto;
    LineFormat  lineFormat  = LineFormat::Default;
};

// Load / save under the host plugin config dir (NPPM_GETPLUGINSCONFIGDIR):
//   <config>/NppJsonViewer/config.json  (defined in NppJsonViewer.mm).
std::string settingsPath();
Settings    loadSettings();
void        saveSettings(const Settings& s);

// Convenience: fold the two parse-related flags into a ParseOptions struct.
inline ParseOptions toParseOptions(const Settings& s) {
    ParseOptions p;
    p.ignoreComments      = s.ignoreComments;
    p.ignoreTrailingComma = s.ignoreTrailingComma;
    return p;
}

// Convenience: fold the formatting flags into a FormatOptions struct.
inline FormatOptions toFormatOptions(const Settings& s) {
    FormatOptions f;
    f.parse             = toParseOptions(s);
    f.eol               = s.eol;
    f.format            = s.lineFormat;
    f.indent            = s.indent;
    f.indentCount       = s.indentCount;
    f.replaceUndefined  = s.replaceUndefined;
    return f;
}

} // namespace npj
