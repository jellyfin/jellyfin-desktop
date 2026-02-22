---
name: investigate-cef
description: Use when needing to understand CEF interfaces, look up CEF API documentation, debug CEF issues, or find how CEF features are implemented
---

# Investigating CEF Source Code

## Overview

CEF (Chromium Embedded Framework) provides the browser embedding layer for this app. The upstream source is the authoritative reference for available interfaces, callback signatures, and behavior.

## Repo Location

CEF source lives in `third_party/` (gitignored):
- `third_party/cef_src` - full upstream source for reference

**Note:** `third_party/cef` is the prebuilt CEF binary distribution used for builds -- it is NOT the source tree. Always use `third_party/cef_src` when investigating interfaces or implementation.

Clone if not present:

```sh
git clone https://github.com/chromiumembedded/cef.git third_party/cef_src
```

## Key Directories

| Path | Purpose |
|------|---------|
| `include/` | **Public C++ API headers** - the primary reference for all CEF interfaces |
| `include/capi/` | C API equivalents (auto-generated from C++ headers) |
| `include/views/` | Views framework for native UI widgets |
| `include/internal/` | Internal types, enums, platform defines |
| `include/wrapper/` | C++ wrapper utilities (CefMessageRouterBrowserSide, etc.) |
| `libcef/` | Core implementation (browser process, renderer, IPC) |
| `libcef/browser/` | Browser-process-side implementations of interfaces |
| `libcef/renderer/` | Renderer-process-side code |
| `libcef_dll/` | DLL wrapper / C-to-C++ translation layer |
| `tests/ceftests/` | Integration tests - great examples of API usage |
| `tests/cefclient/` | Full sample app - reference for real-world patterns |
| `tests/cefsimple/` | Minimal sample app |

## Common Lookup Patterns

### Finding an interface definition
CEF interfaces follow the naming convention `CefFoo` with headers at `include/cef_foo.h`:

- `CefClient` -> `include/cef_client.h`
- `CefRenderHandler` -> `include/cef_render_handler.h`
- `CefBrowserHost` -> `include/cef_browser.h`
- `CefApp` -> `include/cef_app.h`

### Finding handler callbacks
Handler interfaces define virtual methods that CEF calls into your code:

- Display events: `include/cef_display_handler.h`
- Keyboard input: `include/cef_keyboard_handler.h`
- Life span (create/close): `include/cef_life_span_handler.h`
- Load events: `include/cef_load_handler.h`
- Request handling: `include/cef_request_handler.h`
- Render/paint: `include/cef_render_handler.h`
- Focus: `include/cef_focus_handler.h`
- Download: `include/cef_download_handler.h`
- Dialog: `include/cef_dialog_handler.h`
- Context menu: `include/cef_context_menu_handler.h`
- Audio: `include/cef_audio_handler.h`
- Permission: `include/cef_permission_handler.h`
- Media access: `include/cef_media_access_handler.h` (if present)

### Finding how to use a feature
Search `tests/ceftests/` and `tests/cefclient/` for usage examples of any API.

### Understanding process architecture
CEF uses multi-process Chromium architecture:
- **Browser process**: Main app process, owns UI and CefBrowser instances
- **Renderer process**: Runs V8/Blink per render, CefRenderProcessHandler
- **GPU process**: Handles GPU compositing

IPC between browser and renderer uses `CefProcessMessage` via `include/cef_process_message.h`.

## Workflow

1. Clone the repo if not present
2. Search `include/` headers for interface definitions and documentation comments
3. Search `tests/cefclient/` for real usage examples
4. Search `libcef/browser/` or `libcef/renderer/` for implementation details when debugging
5. Apply findings to this project's CEF integration
