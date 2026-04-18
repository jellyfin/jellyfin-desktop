#pragma once
#ifdef __APPLE__
namespace macos_platform {
// Return the NSWindow contentView bounds in logical (points) units.
// Falls back to false if the window isn't up yet.
bool query_logical_content_size(int* w, int* h);
}
#endif
