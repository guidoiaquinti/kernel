// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task placement at creation. See proc/placement.h.

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
