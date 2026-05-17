#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Must match the numeric values of platform.h IdleInhibitLevel
// (None=0, System=1, Display=2).
#define JFN_IDLE_INHIBIT_LEVEL_NONE     0u
#define JFN_IDLE_INHIBIT_LEVEL_SYSTEM   1u
#define JFN_IDLE_INHIBIT_LEVEL_DISPLAY  2u

// Set the current inhibit level. NONE releases any held inhibitor.
// Internally connects to the system bus on first non-NONE call and holds the
// fd returned by org.freedesktop.login1.Manager.Inhibit.
void jfn_idle_inhibit_set(uint32_t level);

// Release the inhibitor and drop the bus connection.
void jfn_idle_inhibit_cleanup(void);

#ifdef __cplusplus
}
#endif
