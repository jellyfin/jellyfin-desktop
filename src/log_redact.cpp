#include "log_redact.h"

#include "log_redact/jfn_log_redact.h"

namespace log_redact {

bool containsSecret(std::string_view msg) {
    return jfn_log_redact_contains_secret(
        reinterpret_cast<const unsigned char*>(msg.data()), msg.size());
}

void censor(std::string& msg) {
    if (msg.empty()) return;
    jfn_log_redact_censor(
        reinterpret_cast<unsigned char*>(msg.data()), msg.size());
}

}  // namespace log_redact
