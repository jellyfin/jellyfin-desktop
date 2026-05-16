#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool jfn_log_redact_contains_secret(const unsigned char* msg, size_t len);
void jfn_log_redact_censor(unsigned char* msg, size_t len);

#ifdef __cplusplus
}
#endif
