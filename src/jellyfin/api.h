#pragma once

#include <string>
#include <string_view>

namespace jellyfin_api {

// Trim surrounding whitespace, lowercase Http:/Https: scheme prefix, and
// prepend http:// when no scheme is present.
std::string normalize_input(std::string_view user_input);

// Reduce a URL to its server base:
//   - If the URL contains "/web" (case-insensitive) in its path, truncate
//     at the last occurrence ("/jellyfin/web/index.html" → "/jellyfin").
//   - Otherwise, return the origin (everything up to the first '/' after
//     "://", or the whole string if there's no path).
std::string extract_base_url(std::string_view url);

// True iff `body` parses as a JSON object containing a non-empty string
// "Id" field. Used to validate a /System/Info/Public response.
bool is_valid_public_info(std::string_view body);

}  // namespace jellyfin_api
