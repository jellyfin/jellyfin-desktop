# Window State Saving & Restoration

## Storage

**File:** `~/.config/jellyfin-desktop/settings.json` (Linux/macOS) or `%APPDATA%\jellyfin-desktop\settings.json` (Windows)

**Persisted fields** (`settings.cpp:84-88`):

| JSON key | Type | Default | Notes |
|---|---|---|---|
| `windowWidth` | int | 0 (-> 1280) | Only written if > 0 |
| `windowHeight` | int | 0 (-> 720) | Only written if > 0 |
| `windowX` | int | -1 (-> centered) | Only written if >= 0; skipped on Wayland |
| `windowY` | int | -1 (-> centered) | Only written if >= 0; skipped on Wayland |
| `windowMaximized` | bool | false | Always written |

Fullscreen state is **never persisted** -- the app always launches in windowed/maximized mode.

## Restoration (startup)

**Sequence** (`main.cpp:609-658`, `window_geometry.cpp:15-39`):

1. **`Settings::instance().load()`** -- called early in `main()` (`main.cpp:327`), reads `settings.json` via cJSON.

2. **Window creation** (`main.cpp:610-627`) -- the saved `width`/`height` are passed directly to `SDL_CreateWindow()` to avoid a visible resize flash. Falls back to 1280x720 if not saved. On Windows, `SDL_WINDOW_MAXIMIZED` is included in creation flags if saved as maximized.

3. **`restoreWindowGeometry(window)`** (`main.cpp:658`, `window_geometry.cpp:15-39`):
   - **Size:** Sets `SDL_SetWindowSize()` if saved width/height > 0.
   - **Position:** Sets `SDL_SetWindowPosition()` if saved x/y >= 0, **but skipped entirely on Wayland** (compositor controls placement).
   - **Clamp:** Calls `clampWindowToDisplay()` to ensure the window fits the current display's usable bounds (taskbar-aware). Skipped if maximized or fullscreen.
   - **Maximize:** If `maximized == true`, calls `SDL_MaximizeWindow()` **after** position/size restore, so the maximize happens on the correct monitor with correct pre-maximize geometry.

## Saving (shutdown)

**`saveWindowGeometry(window, pre_fullscreen_maximized)`** (`window_geometry.cpp:65-105`) is called:

- **Normal shutdown** (`main.cpp:2370`) -- right before `SDL_DestroyWindow()`.
- **Windows force-exit** (`main.cpp:2338`) -- before `_exit()` to handle CEF/D3D driver hang avoidance.

**Logic branches:**

| Current state | What's saved |
|---|---|
| **Fullscreen** | Keeps the *previously-saved* windowed geometry intact; only updates `maximized` to `pre_fullscreen_maximized` (captured at fullscreen entry). This prevents fullscreen monitor dimensions from overwriting the user's windowed geometry. |
| **Maximized** | Saves `maximized=true` with zeroed width/height and -1 position. On next launch, `SDL_MaximizeWindow()` handles sizing -- the pre-maximize geometry is intentionally not captured. |
| **Normal windowed** | Saves actual `width`, `height`, and `x`/`y` position (position skipped on Wayland). `maximized=false`. |

The save is **synchronous** (`Settings::save()`) at shutdown. The `saveAsync()` variant (fire-and-forget on a detached thread with mutex protection) is only used for settings changes from the web UI (server URL, playback settings), **not** for window geometry.

## Fullscreen Tracking

The app tracks a `FullscreenSource` enum (`main.cpp:1165-1166`) to coordinate between three fullscreen initiators:

| Source | How entered | How exited |
|---|---|---|
| **WM** (window manager) | `SDL_EVENT_WINDOW_ENTER_FULLSCREEN` with `source == NONE` | `SDL_EVENT_WINDOW_LEAVE_FULLSCREEN` when `source == WM` |
| **CEF** (JS Fullscreen API) | `Client::OnFullscreenModeChange(true)` callback | `Client::OnFullscreenModeChange(false)` -- only honored if CEF initiated |
| **F11 shortcut** | `default_shortcuts.h` binds F11 -> `TOGGLE_FULLSCREEN`; sets `source = CEF` | Same toggle toggles off, resets `source = NONE` |

**Key invariant:** `was_maximized_before_fullscreen` is captured at `SDL_EVENT_WINDOW_ENTER_FULLSCREEN` (`main.cpp:1827-1828`) by checking `SDL_WINDOW_MAXIMIZED` flag at that moment. This is passed to `saveWindowGeometry()` at shutdown so the correct maximized state is persisted even if the user quits while fullscreen.

**Fullscreen exit restoration** (`main.cpp:1841-1851`): On `SDL_EVENT_WINDOW_LEAVE_FULLSCREEN`, the saved `windowed_width`/`windowed_height` are used to restore geometry. If the window was maximized before fullscreen, `SDL_MaximizeWindow()` is called instead of setting an explicit size. This works around CEF saving the fullscreen dimensions as its internal restore point.

**Windowed geometry tracking** (`main.cpp:1864-1868`): On every `SDL_EVENT_WINDOW_RESIZED`, if not fullscreen, the current size is saved to `windowed_width`/`windowed_height` for later fullscreen-exit restoration.

## Display Change Handling

**`clampWindowToDisplay(window)`** (`window_geometry.cpp:41-63`) is called on:

- `SDL_EVENT_WINDOW_DISPLAY_CHANGED` -- window moved to a different monitor
- `SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED` -- monitor resolution changed
- `SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED` -- desktop mode changed

It clamps the window size to the display's usable bounds. **Skipped** if the window is maximized or fullscreen (those states handle their own sizing).

## Window State Notifications (observer pattern)

`WindowStateNotifier` (`window_state.h`) broadcasts SDL window events to listeners:

| SDL Event | Notification | Listeners |
|---|---|---|
| `SDL_EVENT_WINDOW_MINIMIZED` | `notifyMinimized()` | **MpvLayer** -- pauses video if playing |
| `SDL_EVENT_WINDOW_RESTORED` | `notifyRestored()` | **MpvLayer** -- resumes video if it was playing before minimize |
| `SDL_EVENT_WINDOW_FOCUS_GAINED` | `notifyFocusGained()` | **BrowserLayer** -- sends `sendFocus(true)` to CEF |
| `SDL_EVENT_WINDOW_FOCUS_LOST` | `notifyFocusLost()` | **BrowserLayer** -- sends `sendFocus(false)` to CEF |

Additionally, on `SDL_EVENT_WINDOW_FOCUS_GAINED` (`main.cpp:1796-1801`), the app syncs CEF's fullscreen state with SDL's -- if SDL is fullscreen, it requests `document.documentElement.requestFullscreen()`, otherwise calls `client->exitFullscreen()`. This handles WM-initiated fullscreen changes that happened while the window was unfocused.

## Window Activation (single-instance raise)

`window_activation.cpp` handles bringing the window to front when a second instance is launched:

- On **Wayland**: uses `xdg-activation-v1` protocol with a token from the signaling instance
- On **all platforms**: calls `SDL_RestoreWindow()` + `SDL_RaiseWindow()`

This restores from minimized state and raises to front, but doesn't interact with geometry saving.

## Gaps / Design Decisions

1. **No periodic auto-save** of window geometry -- only saved at shutdown. A crash loses the current geometry.
2. **Position not saved on Wayland** -- by design, Wayland compositors control window placement.
3. **Maximized state discards windowed geometry** -- when maximized, width/height/position are zeroed. If the user was maximized, quit, and next launch the maximize fails for some reason, the window falls back to the 1280x720 default.
4. **Fullscreen is never persisted** -- always launches windowed/maximized. The `pre_fullscreen_maximized` flag ensures the correct maximized state is saved if the user quits while fullscreen.
