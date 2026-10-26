// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task placement at creation and the run-queue order. See proc/placement.h.

#include "proc/placement.h"

int sched_place(const unsigned *load, const bool *online, unsigned ncpu, int requested) {
    if (requested >= 0) {
        return (unsigned)requested < ncpu && online[requested] ? requested : -1;
    }
    int best = -1;
    for (unsigned i = 0; i < ncpu; i++) {
        if (online[i] && (best < 0 || load[i] < load[best])) {
            best = (int)i;
        }
    }
    return best;
}

int sched_round_robin(int slots, int from, bool (*can_run)(int slot, void *ctx), void *ctx) {
    if (slots <= 0) {
        return -1;
    }
    if (from < 0 || from >= slots) {
        from = slots - 1;  // so that slot 0 comes first
    }
    for (int i = 1; i <= slots; i++) {
        int slot = (from + i) % slots;
        if (can_run(slot, ctx)) {
            return slot;
        }
    }
    return -1;
}

uint64_t sched_wake_us(uint64_t now_us, uint64_t ms) {
    uint64_t limit = ~(uint64_t)0;
    if (ms > (limit - now_us) / 1000) {
        return limit;
    }
    return now_us + ms * 1000;
}
