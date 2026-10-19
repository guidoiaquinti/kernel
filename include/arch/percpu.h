// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// Per-CPU data. Each core's TPIDR_EL1 points at its own struct cpu, so
// this_cpu() is a single register read and needs no lock: only the owning
// core touches its own entry (other cores may read it for diagnostics).

#define MAX_CPUS 4

struct task;

// Each entry fills whole cache lines: a secondary core writes its own entry
// while other cores use theirs, so no two may share a line.
struct cpu {
    unsigned id;
    struct task *curr;        // the task running on this core
    struct task *idle;        // its boot thread (task 0 on core 0)
    volatile bool need_resched;  // switch tasks on the next return to EL0
    struct task *fp_owner;    // whose state is in this core's FP/SIMD registers
    int locks_held;           // spinlocks this core holds (debug checks)
    bool panicking;           // panic(): bypass locks so the message gets out
    bool caches_on;           // MMU and data cache enabled: spinlocks usable
    volatile bool online;     // secondary core: brought up and idling
    uint64_t ticks;           // this core's timer interrupts
    uint64_t tick_deadline;   // its next timer compare value
} __attribute__((aligned(64)));

extern struct cpu cpus[MAX_CPUS];

// Point this core's TPIDR_EL1 at cpus[id] and record its id. Call first thing
// on core 0. A secondary core calls percpu_attach() first (a register write
// only: its MMU is still off, see smp.c) and percpu_init() once caches are on.
void percpu_init(unsigned id);
void percpu_attach(unsigned id);

#ifdef KORAOS_HOST_TEST
struct cpu *this_cpu(void);  // provided by the host test
#else
static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    asm volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}
#endif
