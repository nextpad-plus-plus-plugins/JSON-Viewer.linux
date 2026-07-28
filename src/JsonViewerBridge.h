// JsonViewerBridge.h — shared inter-plugin handshake contract between
// JSTool (JSMinNPP, this plugin) and the separate "JSON Viewer" plugin
// (module folder name "NppJsonViewer").
//
// JSTool no longer ships its own JSON tree viewer; its "JSON Viewer" menu
// item instead asks the installed JSON-Viewer plugin to show its panel.
// Communication goes through the host's NPPM_MSGTOPLUGIN: JSTool sends a
// CommunicationInfo whose `internalMsg` is one of the values below, with
// wParam = the target module name. The host returns the target plugin's
// messageProc return value, or 0 if no such plugin is loaded.
//
// This identical header is dropped into BOTH plugin source trees so the
// two sides can never drift. Keep it dependency-free.

#ifndef JSTOOL_JSONVIEWER_BRIDGE_H
#define JSTOOL_JSONVIEWER_BRIDGE_H

// Module (plugin folder) name the JSON-Viewer plugin installs under.
#define JSONVIEWER_MODULE_NAME "NppJsonViewer"

// CommunicationInfo.internalMsg values (the field is a `long`).
// Distinct positive 32-bit constants so they survive the long round-trip.
#define JV_BRIDGE_MSG_PING       0x4A53544FL   /* 'JSTO' — "are you there?" */
#define JV_BRIDGE_MSG_SHOWPANEL  0x4A53544EL   /* 'JSTN' — "show your panel" */

// Value the JSON-Viewer plugin returns from messageProc in reply to
// JV_BRIDGE_MSG_PING. Any other return (including the legacy `return 1`
// of an old JSON-Viewer that doesn't understand the handshake, or 0 when
// the plugin isn't installed) means "no compatible JSON Viewer present".
#define JV_BRIDGE_ACK            0x4A564F4BL   /* 'JVOK' */

#endif // JSTOOL_JSONVIEWER_BRIDGE_H
