// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for task placement (src/proc/placement.c).
#include "proc/placement.h"
#include "test.h"

static void least_loaded(void) {
    unsigned load[4] = {3, 1, 2, 1};
    bool online[4] = {true, true, true, true};
    CHECK(sched_place(load, online, 4, -1) == 1, "lowest load, lowest id on a tie");
    load[1] = 5;
    CHECK(sched_place(load, online, 4, -1) == 3, "next lowest");
    online[3] = false;
    CHECK(sched_place(load, online, 4, -1) == 2, "offline cores are skipped");
    unsigned empty[4] = {0, 0, 0, 0};
    bool one[4] = {true, false, false, false};
    CHECK(sched_place(empty, one, 4, -1) == 0, "single core");
}

static void spreading(void) {
    // Placing tasks one after another fills the cores evenly.
    unsigned load[4] = {0, 0, 0, 0};
    bool online[4] = {true, true, true, true};
    unsigned placed[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 12; i++) {
        int core = sched_place(load, online, 4, -1);
        CHECK(core >= 0 && core < 4, "placement %u", i);
        load[core]++;
        placed[core]++;
    }
    CHECK(placed[0] == 3 && placed[1] == 3 && placed[2] == 3 && placed[3] == 3,
          "12 tasks spread 3 per core");
}

static void requested(void) {
    unsigned load[4] = {9, 9, 9, 0};
    bool online[4] = {true, true, false, true};
    CHECK(sched_place(load, online, 4, 1) == 1, "pinned to a busy core");
    CHECK(sched_place(load, online, 4, 2) == -1, "offline core refused");
    CHECK(sched_place(load, online, 4, 4) == -1, "nonexistent core refused");
    CHECK(sched_place(load, online, 4, 14) == -1, "far out of range refused");
}

TEST_MAIN(least_loaded, spreading, requested)
