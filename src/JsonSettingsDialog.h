// JsonSettingsDialog — modal settings window (GTK4 port of the macOS
// NSWindow modal). Returns true when the user pressed OK (settings were
// written back and persisted).

#pragma once

#include "JsonSettings.h"

namespace npj {

// Present the dialog modally. On OK: *settings is updated and saved to disk.
// On Cancel / close: untouched. Blocks in a nested GMainLoop.
bool presentSettingsDialog(Settings* settings);

} // namespace npj
