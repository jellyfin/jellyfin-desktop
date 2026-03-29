#pragma once

#include <functional>
#include <string>

// Try to signal an already-running instance to raise its window.
// Returns true if an existing instance was found and signaled (caller should exit).
bool trySignalExisting();

// Start listening for signals from future instances.
// onRaise is called from the listener thread when a "raise" message arrives.
// The string parameter carries the Wayland activation token (empty if unavailable).
void startListener(const std::function<void(const std::string&)>& onRaise);

// Stop the listener and clean up the socket/pipe.
void stopListener();
