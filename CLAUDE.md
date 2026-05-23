# Project Notes

## Constraints
- **No hand-rolled JSON** — never manually construct or parse JSON with string concatenation, manual escaping, or homebrew parsers. Use `serde_json`, or CEF's `CefParseJSON`/`CefWriteJSON` via the cef-dll-sys bindings when crossing the V8 boundary.
- **No artificial heartbeats/polling** - event-driven architecture only. Never use timeouts as a workaround for proper event integration. No arbitrary timeout-based bailouts in shutdown paths either — fix the root cause instead.
- **No texture stretching during resize** - CEF content must always render at 1:1 pixel mapping. Never scale/stretch textures to fill the viewport. Gaps from stale texture sizes are acceptable; stretching is not.

## Build / Run
All app code is Rust. The cargo workspace lives in `src/` and produces the `jellyfin-desktop` binary as a `[[bin]]` in `src/jfn_rust`. CMake is a thin orchestrator that drives mpv's meson build and the cargo invocation, then stages CEF resources + libmpv next to the binary. `project()` is `LANGUAGES NONE`.

Use `just` — recipes are OS-gated via `[macos]`/`[linux]`/`[windows]` attributes, so the same command works everywhere:
```
just deps     # one-time: submodules, CEF download, macOS brew packages
just build    # configure (if needed) + cmake build (meson mpv + cargo)
just test     # cargo test --workspace (ctest is empty)
just run      # run with debug logging → logs to build/run.log
just clean    # remove build/ and dist/ (keeps CEF SDK)
just lint     # rustfmt --check + clippy -D warnings across crates
just dmg      # [macos] build app bundle + distributable DMG
just appimage # [linux] build AppImage via podman/docker
just flatpak  # [linux] build Flatpak bundle
```
Platform-specific entry points live in `dev/linux/`, `dev/macos/`, `dev/windows/`, imported by the top-level justfile.

## Architecture
- **CEF** (Chromium Embedded Framework) — hosts jellyfin-web as an embedded browser; handles JS-to-Rust IPC for player control commands and renders the UI as an overlay texture above the video layer. Multi-process: browser process (main app, owns CefBrowser), renderer process (V8/Blink), GPU process. IPC via `CefProcessMessage`. Bindings via `cef-dll-sys`; project glue lives in `src/jfn_cef`.
- **mpv** (fork in `third_party/mpv`) — video playback; the desktop client injects native shims to override browser media playback. mpv owns its own window + GPU; libmpv is used only for the control plane (properties/commands/events).
- Wayland subsurface for video layer (Linux). Platform crates: `src/macos`, `src/windows`, plus Wayland/X11 paths under `src/`.

## mpv Integration
- **Never call sync mpv API (`mpv_get_property`, etc.) from event callbacks** - causes deadlock during video init. Use property observation or async variants instead.

## mpv Event Flow
mpv is the authoritative source of playback state. All state (position, speed, pause, seeking, etc.) flows from mpv property observations outward to the JS UI and OS media sessions. The JS UI and MPRIS/macOS media sessions are consumers — they never determine playback state, they only reflect what mpv reports. This means things like rate changes, seek completion, and position updates come from mpv, not from JS round-trips or manual bookkeeping.

## Debugging
- For mpv (third_party/mpv), jellyfin-web, and CEF: investigate source code directly before suggesting debug logs that require manual user action
