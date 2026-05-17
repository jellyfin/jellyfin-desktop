#pragma once

#include "jfn_jellyfin.h"

#include <string>
#include <string_view>

// Header-only thin wrapper over the Rust URL helpers in
// src/jellyfin/src/lib.rs. Adapts the extern "C" surface to std::string-
// flavored call sites.

namespace jellyfin_api {

// Trim surrounding whitespace, lowercase Http:/Https: scheme prefix, and
// prepend http:// when no scheme is present.
inline std::string normalize_input(std::string_view user_input) {
    std::string in(user_input);
    char* raw = jfn_jellyfin_normalize_input(in.c_str());
    std::string out = raw ? std::string(raw) : std::string();
    jfn_jellyfin_free_string(raw);
    return out;
}

// Reduce a URL to its server base:
//   - If the URL contains "/web" (case-insensitive) in its path, truncate
//     at the last occurrence ("/jellyfin/web/index.html" → "/jellyfin").
//   - Otherwise, return the origin (everything up to the first '/' after
//     "://", or the whole string if there's no path).
inline std::string extract_base_url(std::string_view url) {
    std::string in(url);
    char* raw = jfn_jellyfin_extract_base_url(in.c_str());
    std::string out = raw ? std::string(raw) : std::string();
    jfn_jellyfin_free_string(raw);
    return out;
}

// True iff `body` parses as a JSON object containing a non-empty string
// "Id" field. Used to validate a /System/Info/Public response.
inline bool is_valid_public_info(std::string_view body) {
    return jfn_jellyfin_is_valid_public_info(body.data(), body.size());
}

}  // namespace jellyfin_api
