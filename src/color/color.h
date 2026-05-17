#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse a CSS color from <meta name="theme-color">: #RGB or #RRGGBB. Returns
// packed 0x00RRGGBB on success and 0 on malformed input (matches the legacy
// Color{} default).
uint32_t jfn_cef_parse_color(const char* s);

// Parse any form mpv emits or accepts (third_party/mpv/options/m_option.c
// :2079-2147). mpv's print_color emits #AARRGGBB — alpha is FIRST. Does NOT
// accept CSS #RGB. Returns packed 0x00RRGGBB on success and 0 on malformed
// input.
uint32_t jfn_mpv_parse_color(const char* s);

#ifdef __cplusplus
}
#endif
