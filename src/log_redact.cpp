#include "log_redact.h"

#include <array>
#include <string>
#include <string_view>

namespace log_redact {

namespace {

// A redaction rule: when `needle` is found in a log line, the run of
// characters following it (up to but excluding the first character in
// `terminators`, or end-of-string) is overwritten with 'x'.
struct PatternRule {
    std::string_view needle;
    std::string_view terminators;
};

// Terminators for URL-param / header forms. Tokens end at the next URL
// delimiter or whitespace.
constexpr std::string_view kUrlTerminators = "&\"' \t\r\n;<>";
// JSON form: the token ends at the closing quote.
constexpr std::string_view kJsonTerminators = "\"";

constexpr std::array<PatternRule, 6> kRules = {{
    {"api_key=",                kUrlTerminators},
    {"X-MediaBrowser-Token%3D", kUrlTerminators},
    {"X-MediaBrowser-Token=",   kUrlTerminators},
    {"ApiKey=",                 kUrlTerminators},
    {"AccessToken=",            kUrlTerminators},
    {"AccessToken\":\"",        kJsonTerminators},
}};

std::size_t findTokenEnd(const std::string& msg, std::size_t from,
                         std::string_view terminators) {
    auto end = msg.find_first_of(terminators, from);
    return end == std::string::npos ? msg.size() : end;
}

void elideTokens(std::string& msg, const PatternRule& rule) {
    std::size_t start = 0;
    while ((start = msg.find(rule.needle, start)) != std::string::npos) {
        std::size_t token_start = start + rule.needle.size();
        std::size_t token_end = findTokenEnd(msg, token_start, rule.terminators);
        for (std::size_t i = token_start; i < token_end; ++i) {
            msg[i] = 'x';
        }
        // Advance past the (now-redacted) token; if it was empty, advance past
        // the needle so we don't loop on the same position.
        start = token_end > token_start ? token_end : token_start;
    }
}

}  // namespace

bool containsSecret(std::string_view msg) {
    for (const auto& rule : kRules) {
        auto pos = msg.find(rule.needle);
        if (pos == std::string_view::npos) continue;
        std::size_t token_start = pos + rule.needle.size();
        if (token_start < msg.size() &&
            rule.terminators.find(msg[token_start]) == std::string_view::npos) {
            return true;
        }
    }
    return false;
}

void censor(std::string& msg) {
    for (const auto& rule : kRules) {
        elideTokens(msg, rule);
    }
}

}  // namespace log_redact
