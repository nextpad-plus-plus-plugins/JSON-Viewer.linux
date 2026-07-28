# JSON Viewer — Linux (GTK4) port

Tree-view navigator, formatter, compressor, and validator for JSON documents
inside Nextpad++. Linux port of the macOS port of the Windows
[NPP-JSONViewer/JSON-Viewer](https://github.com/NPP-JSONViewer/JSON-Viewer)
plugin (GPLv2).

The engine is untouched: `JsonParser`, `JsonFormatter`, the settings model and
the bundled RapidJSON fork are the macOS sources **byte-for-byte** (only the
`.mm` extension became `.cpp` — they were already pure C++). The bundled
RapidJSON is a fork with a `PrettyWriter::SetLineEnding` extension and the
RawNumber fix; it is **not interchangeable with stock rapidjson**. The Cocoa
layer (panel, settings dialog, bootstrap) was rewritten on GTK4.

## Features

- **JSON tree panel** docked via the host's `NPPM_DMM_*` API (pop-out via the
  host frame). Lazy tree population — huge documents load the visible level
  only.
- **Click-to-jump** — click any tree node to jump the editor caret to its
  key/value token, with the token selected.
- **Live re-parse** on buffer edits (debounced 200 ms) and optional
  follow-current-tab.
- **Format / Compress / Sort by key** — configurable indent (tab / N spaces /
  auto), EOL (CRLF / LF / CR / auto), line format (default / single-line
  arrays). Number formatting survives round-trips exactly (RawNumber).
- **Validate** with the caret moved to the offending offset on error.
- **Right-click context menu**: Copy / Copy name / Copy value / Copy path /
  Expand all / Collapse all — same enable/disable semantics as Windows.
- **Live search** in the tree (matches + their ancestors, auto-expanded;
  Escape clears).
- **Settings dialog** with all the toggles from the Windows version.
- **JSTool bridge**: answers the `JV_BRIDGE_MSG_PING` / `SHOWPANEL`
  inter-plugin handshake. NOTE: this host routes `NPPM_MSGTOPLUGIN` by
  `getName()` — a JSTool port must target `"JSON Viewer"`, not the module
  folder name `"NppJsonViewer"` used on macOS.

## Differences from macOS

- **No pre-bound shortcuts.** This host's `FuncItem` has no `_pShKey`, so the
  ⌘⌥⇧J/M/C/K defaults are not bound — assign them in
  *Settings ▸ Shortcut Mapper ▸ Plugin commands*.
- **No floating-panel fallback.** The macOS port falls back to an NSPanel on
  pre-v1.0.2 hosts without the docking API; every Linux host has
  `NPPM_DMM_REGISTERPANEL`, so a registration failure is reported instead.
- **No panel zoom hooks.** macOS routes ⌘+/−/0 into the panel through host
  plumbing that has no Linux equivalent; the tree renders at a fixed 10 pt.
- `NPPM_SETCURRENTLANGTYPE` has no handler on this host — JSON highlighting
  is applied through `NPPM_SETBUFFERLANGTYPE(0, 57)`, which targets the
  current buffer and fires `NPPN_LANGCHANGED`.

## Build

```sh
cmake -B build -S .
cmake --build build -j"$(nproc)"
cmake --install build   # -> ~/.local/share/nextpad++/plugins/NppJsonViewer/
```

Requires `libgtk-4-dev` and the Nextpad++ GTK4 tree checked out alongside this
folder (for `plugin.h` and the Scintilla headers).

### Tests

```sh
xvfb-run -a ctest --test-dir build --output-on-failure
```

`loader` is a dlopen smoke test (exports, menu shape, `FuncItem` ABI).
`jv_test` drives the real `.so` through the real view bridge against a mock
Scintilla document — 34 assertions covering format/compress/sort, RawNumber
round-trips, error-offset selection, selection-scoped operations, panel
registration and tree content, lazy expansion, the debounced live refresh,
the search filter, the JSTool handshake and settings persistence.

## License

GNU General Public License v2, as upstream. Bundled RapidJSON under the MIT
license (see `external/rapidjson_license.txt`).
