#pragma once

#include <string>
#include <string_view>

// Token redaction for log output. Detects known query-param / JSON / header
// patterns that precede a Jellyfin access token and overwrites the token
// value with 'x' characters, preserving URL/JSON shape.
//
// Standalone module with no logging or third-party dependencies so it can be
// unit-tested in isolation.
namespace log_redact {

// Returns true if `msg` contains any redactable pattern with a non-empty
// token value following. Cheap pre-check so callers can skip the string copy
// on the common case of no secrets.
bool containsSecret(std::string_view msg);

// Overwrites token characters that follow known patterns with 'x' in place.
// Length-preserving — `msg.size()` is unchanged.
void censor(std::string& msg);

}  // namespace log_redact
