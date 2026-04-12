#pragma once

namespace input {

struct KeyEvent;

// Returns true if the event was consumed by a hotkey action.
// Dispatch must not forward the event to the browser when this returns true.
//
// Bindings and gates live in hotkeys.cpp. Platform files never match keys
// for hotkey purposes.
bool hotkey_try_consume(const KeyEvent& e);

}  // namespace input
