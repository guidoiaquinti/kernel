// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// Where a new task goes (pure logic, host-tested): `requested` if it names an
// online core, -1 if it names no such core; for requested == -1 the online
// core with the lowest load, the lowest-numbered one on a tie.
int sched_place(const unsigned *load, const bool *online, unsigned ncpu, int requested);
