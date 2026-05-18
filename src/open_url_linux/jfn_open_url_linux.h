#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Spawn xdg-open <url> detached. Caller must ensure the URL is non-empty and
// doesn't start with '-'.
void jfn_open_url(const char* url);

#ifdef __cplusplus
}
#endif
