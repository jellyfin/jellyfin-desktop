#pragma once

#include "../mpv/event.h"

// Player-subsystem consumer of the global MessageBus. Owns:
//   - Inbound `player.*` handler registration with `g_bus`.
//   - The cached MediaStream[] for the currently-loaded item (consulted by
//     player.selectAudio / player.selectSubtitle).
//   - The reverse path: translation of mpv property-observer events into
//     `player.*` outbound notifications via `g_bus.emit`.
//
// `install` runs on the construction thread before browsers exist (no TID_UI
// requirement). `on_mpv_event` is invoked from the cef_consumer_thread and
// emits on the bus; `MessageBus::emit` self-hops to TID_UI internally, so no
// CefPostTask wrapping is needed here.
namespace player_router {

// Register all `player.*` handlers with `g_bus`. Idempotent — intended to be
// called once during WebBrowser construction.
void install();

// Translate an mpv property/lifecycle event into the matching `player.*`
// bus notification. Called from main.cpp's TID_UI event drain loop.
void on_mpv_event(const MpvEvent& ev);

}  // namespace player_router
