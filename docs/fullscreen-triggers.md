# Fullscreen State — Trigger Points

All code paths that initiate, respond to, or synchronize fullscreen state changes.

## State Tracking

**`src/main.cpp:1164-1168`** — Runtime state (local to `main()`):

```cpp
enum class FullscreenSource { NONE, WM, CEF };
FullscreenSource fullscreen_source = FullscreenSource::NONE;
bool was_maximized_before_fullscreen = false;
int windowed_width = 0, windowed_height = 0;
```

`FullscreenSource` prevents feedback loops: WM-initiated fullscreen ignores CEF exit requests; CEF-initiated fullscreen honors them.

Fullscreen is **not persisted** across sessions — only `maximized` is saved (`src/settings.h:22`). No `--fullscreen` CLI flag exists.

---

## Trigger Points

### 1. F11 Keyboard Shortcut

| | |
|---|---|
| **Binding** | `src/input/default_shortcuts.h:5-16` — `SDLK_F11 → AppAction::TOGGLE_FULLSCREEN` |
| **Dispatch** | `src/input/shortcut_layer.h:10-40` — `ShortcutLayer::handleInput()` fires on `KEY_DOWN`, consumes both `KEY_DOWN` and `KEY_UP` |
| **Handler** | `src/main.cpp:1400-1415` — toggles `SDL_SetWindowFullscreen()`, sets `fullscreen_source` to `CEF` (enter) or `NONE` (exit) |
| **Invoked from** | `src/main.cpp:1770` — main event loop, before `input_stack.route()` |

### 2. JS Fullscreen API (jellyfin-web OSD Button)

The web content calls `document.documentElement.requestFullscreen()` or `document.exitFullscreen()`:

| | |
|---|---|
| **JS toggle** | `src/web/mpv-video-player.js:303-309` — `toggleFullscreen()` method called by jellyfin-web OSD |
| **JS cleanup** | `src/web/mpv-video-player.js:257-260` — `exitFullscreen()` on player destroy |
| **JS Escape** | `src/web/native-shim.js:19-23` — Escape key calls `document.exitFullscreen()` |
| **CEF handler** | `src/cef/cef_client.cpp:276-281` — `OnFullscreenModeChange()` invokes `on_fullscreen_change_` callback |
| **Native callback** | `src/main.cpp:1262-1278` — lambda sets `fullscreen_source = CEF` and calls `SDL_SetWindowFullscreen()`; exit only honored if `fullscreen_source == CEF` |

Feature declared at `src/web/native-shim.js:390` (`'fullscreenchange'` in `AppHost.supports()`).

### 3. Window Manager / Compositor

The WM (tiling WM, compositor gesture, etc.) sets or unsets fullscreen externally:

| | |
|---|---|
| **Enter** | `src/main.cpp:1825-1833` — `SDL_EVENT_WINDOW_ENTER_FULLSCREEN`: snapshots `was_maximized_before_fullscreen`, sets `fullscreen_source = WM` if unset, injects `requestFullscreen()` into browser |
| **Leave** | `src/main.cpp:1835-1852` — `SDL_EVENT_WINDOW_LEAVE_FULLSCREEN`: calls `client->exitFullscreen()`, clears WM source, restores windowed geometry (maximize or resize) |

No platform-specific fullscreen code exists — all goes through SDL's abstraction.

### 4. MPRIS D-Bus (Linux) — Stub / Unimplemented

| | |
|---|---|
| **Properties** | `src/player/mpris/media_session_mpris.cpp:30-41, 78-79` — `CanSetFullscreen` always `true`, `Fullscreen` always `false` (TODO) |
| **Callback** | `src/player/media_session.h:68` — `onSetFullscreen` declared but never wired |

A D-Bus client could theoretically set `Fullscreen` via MPRIS2 `org.mpris.MediaPlayer2`, but no handler exists yet.

---

## Synchronization Points

These don't initiate fullscreen but keep SDL and CEF/browser state in sync:

### 5. Focus Gain Re-sync

**`src/main.cpp:1794-1801`** — `SDL_EVENT_WINDOW_FOCUS_GAINED`:
- If SDL window is fullscreen → injects `requestFullscreen()` into browser
- If SDL window is not fullscreen → calls `client->exitFullscreen()`

Handles WM changing fullscreen state while the window was unfocused.

### 6. Browser Fullscreen Exit (Native → CEF)

**`src/cef/cef_client.cpp:838-842`** — `Client::exitFullscreen()`:
- Calls `CefBrowserHost::ExitFullscreen(true)`
- Called from `SDL_EVENT_WINDOW_LEAVE_FULLSCREEN` and `SDL_EVENT_WINDOW_FOCUS_GAINED`

### 7. Browser Fullscreen Change Event (JS Layer)

**`src/web/native-shim.js:4-17`** — `fullscreenchange` DOM event listener:
- Updates `window._isFullscreen`
- Fires `fullscreenchange` on `window._mpvVideoPlayerInstance` so jellyfin-web UI updates

### 8. Windowed Geometry Tracking

**`src/main.cpp:1864-1868`** — `SDL_EVENT_WINDOW_RESIZED`:
- Only updates `windowed_width`/`windowed_height` when NOT fullscreen
- Guards against capturing fullscreen dimensions as restore size

### 9. Geometry Save at Shutdown

**`src/window_geometry.cpp:65-105`** — `saveWindowGeometry()`:
- If fullscreen: preserves previously-saved windowed geometry, only updates `maximized` from `pre_fullscreen_maximized`
- Called at `src/main.cpp:2338` (Windows) and `src/main.cpp:2370` (Linux)

**`src/window_geometry.cpp:41-44`** — `clampWindowToDisplay()`:
- Skips clamping when fullscreen or maximized

---

## Data Flow

```
                    ┌─────────────────────────┐
                    │  jellyfin-web OSD button │
                    └────────────┬────────────┘
                                 │ toggleFullscreen()
                                 ▼
                    ┌─────────────────────────┐
                    │ document.requestFullscreen│
                    │ document.exitFullscreen   │
                    │ [mpv-video-player.js:304] │
                    └────────────┬────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                   │
              ▼                  │                   ▼
 ┌────────────────────┐         │      ┌─────────────────────┐
 │ Escape key handler  │         │      │ Player destroy       │
 │ [native-shim.js:19] │         │      │ [mpv-video-player    │
 │ → exitFullscreen()  │         │      │  .js:257]            │
 └─────────┬──────────┘         │      └──────────┬──────────┘
           │                    │                  │
           └────────────────────┼──────────────────┘
                                │ CEF browser process
                                ▼
                   ┌────────────────────────┐
                   │ OnFullscreenModeChange  │
                   │ [cef_client.cpp:276]    │
                   └────────────┬───────────┘
                                │ on_fullscreen_change_(bool)
                                ▼
  ┌──────────┐    ┌────────────────────────┐    ┌──────────────┐
  │ F11 key  │    │ Fullscreen callback    │    │ Window       │
  │ shortcut │    │ lambda                 │    │ Manager      │
  │ [main    │    │ [main.cpp:1262]        │    │ (external)   │
  │ .cpp:1404│    └────────────┬───────────┘    └──────┬───────┘
  └────┬─────┘                 │                       │
       │                       │                       │
       └───────────────────────┼───────────────────────┘
                               │ SDL_SetWindowFullscreen()
                               ▼
              ┌────────────────────────────────┐
              │ SDL_EVENT_WINDOW_ENTER/LEAVE_  │
              │ FULLSCREEN [main.cpp:1825/1835]│
              └───────────────┬────────────────┘
                              │
                ┌─────────────┼─────────────┐
                │             │             │
                ▼             ▼             ▼
        ┌────────────┐ ┌───────────┐ ┌──────────────┐
        │ executeJS  │ │ exit      │ │ Geometry     │
        │ request    │ │ Fullscreen│ │ restore      │
        │ Fullscreen │ │ [cef_     │ │ [main.cpp    │
        │ [main.cpp  │ │ client    │ │ :1844]       │
        │ :1832]     │ │ .cpp:838] │ └──────────────┘
        └─────┬──────┘ └──────────┘
              │
              ▼
     ┌─────────────────────────┐
     │ fullscreenchange event  │
     │ [native-shim.js:7]     │
     │ → window._isFullscreen  │
     │ → player event trigger  │
     └─────────────────────────┘
```

## Summary

| # | Trigger | Entry Point | Calls |
|---|---------|-------------|-------|
| 1 | **F11 key** | `main.cpp:1404` | `SDL_SetWindowFullscreen()` |
| 2 | **JS Fullscreen API** (OSD button) | `mpv-video-player.js:304` → `cef_client.cpp:276` → `main.cpp:1262` | `SDL_SetWindowFullscreen()` |
| 3 | **Escape key** (in JS) | `native-shim.js:19` → same CEF path as #2 | `document.exitFullscreen()` |
| 4 | **Player destroy** | `mpv-video-player.js:257` → same CEF path as #2 | `document.exitFullscreen()` |
| 5 | **Window Manager** | `main.cpp:1825` (SDL event) | `executeJS("requestFullscreen()")` |
| 6 | **Focus gain re-sync** | `main.cpp:1794` | `executeJS()` or `exitFullscreen()` |
| 7 | **MPRIS D-Bus** (stub) | `media_session_mpris.cpp:30` | Not wired — dead code |
