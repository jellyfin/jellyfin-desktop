#pragma once

#include "platform.h"

namespace idle_inhibit {

void init();
void set(IdleInhibitLevel level);
void cleanup();

}  // namespace idle_inhibit
