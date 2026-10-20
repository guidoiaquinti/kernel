// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// Where a new task goes (pure logic, host-tested): `requested` if it names an
// online core, -1 if it names no such core; for requested == -1 the online
// core with the lowest load, the lowest-numbered one on a tie.
int sched_place(const unsigned *load, const bool *online, unsigned ncpu, int requested);

// A core's run queue in round-robin order (pure logic, host-tested): the
// first of `slots` slots after `from` that can_run(slot, ctx) accepts,
// wrapping around and trying `from` itself last; -1 if none can run.
int sched_round_robin(int slots, int from, bool (*can_run)(int slot, void *ctx), void *ctx);

// How many ticks from now msleep(ms) wakes at `hz` ticks per second: whole
// ticks rounded up, plus one because the current tick is already partly over.
// 0 (just yield) for 0 ms or an unknown rate.
uint64_t sched_sleep_ticks(uint64_t ms, unsigned hz);
