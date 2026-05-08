#pragma once

// Shutdown signal plumbing. g_shutting_down / g_shutdown_event /
// initiate_shutdown are forward-declared in common.h for callers that don't
// need this header's other helpers.

void signal_handler(int);

template <typename Browser>
inline void try_close_browser(Browser* b) {
    if (b && b->browser()) b->browser()->GetHost()->CloseBrowser(true);
}
