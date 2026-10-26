// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for task placement and the run-queue order (src/proc/placement.c).
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

// A toy task table for sched_round_robin: which core each slot's task runs
// on and whether it is runnable.
struct queue {
    unsigned core[8];
    bool runnable[8];
    unsigned want;  // the core picking
};

static bool can_run(int slot, void *ctx) {
    const struct queue *q = ctx;
    return q->runnable[slot] && q->core[slot] == q->want;
}

static void round_robin(void) {
    struct queue q = {
        .core = {0, 1, 0, 0, 1, 0, 2, 0},
        .runnable = {true, true, true, false, true, true, true, false},
        .want = 0,
    };
    // Core 0's runnable slots are 0, 2 and 5: each turn passes to the next.
    CHECK(sched_round_robin(8, 0, can_run, &q) == 2, "0 -> 2");
    CHECK(sched_round_robin(8, 2, can_run, &q) == 5, "2 -> 5, skipping a blocked task");
    CHECK(sched_round_robin(8, 5, can_run, &q) == 0, "5 wraps around to 0");
    CHECK(sched_round_robin(8, 3, can_run, &q) == 5, "from a task that just blocked");
    CHECK(sched_round_robin(8, -1, can_run, &q) == 0, "no previous slot: start at 0");
    // Other cores' tasks are never picked.
    q.want = 2;
    CHECK(sched_round_robin(8, 6, can_run, &q) == 6, "the only task keeps running");
    q.want = 1;
    CHECK(sched_round_robin(8, 1, can_run, &q) == 4, "core 1: 1 -> 4");
    CHECK(sched_round_robin(8, 4, can_run, &q) == 1, "core 1: 4 -> 1");
    q.want = 3;
    CHECK(sched_round_robin(8, 0, can_run, &q) == -1, "nothing runnable: idle");
    // A full round visits every runnable task of the core exactly once.
    q.want = 0;
    unsigned seen[8] = {0};
    int slot = 0;
    for (int turn = 0; turn < 9; turn++) {
        slot = sched_round_robin(8, slot, can_run, &q);
        seen[slot]++;
    }
    CHECK(seen[0] == 3 && seen[2] == 3 && seen[5] == 3, "fair turns %u/%u/%u", seen[0],
          seen[2], seen[5]);
}

static void wake_deadline(void) {
    CHECK(sched_wake_us(1000, 300) == 301000, "300 ms after 1 ms");
    CHECK(sched_wake_us(0, 1) == 1000, "1 ms");
    uint64_t far = sched_wake_us(5000000, ~(uint64_t)0);
    CHECK(far == ~(uint64_t)0, "an absurd sleep saturates instead of wrapping into the past");
    CHECK(sched_wake_us(~(uint64_t)0 - 500, 1) == ~(uint64_t)0, "near the end of time");
}

TEST_MAIN(least_loaded, spreading, requested, round_robin, wake_deadline)
