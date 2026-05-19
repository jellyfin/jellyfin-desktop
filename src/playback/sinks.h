#pragma once

// Umbrella header — individual sinks live in sinks/. Platform media-session
// sinks (mpris/macos/windows) are NOT in this umbrella; main.cpp picks one
// per-platform via #ifdef.
#include "sinks/queued_sink.h"
#include "sinks/browser_sink.h"
