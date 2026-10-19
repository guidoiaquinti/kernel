// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-CPU data. See arch/percpu.h.

#include "arch/percpu.h"

struct cpu cpus[MAX_CPUS];

void percpu_attach(unsigned id) {
    asm volatile("msr tpidr_el1, %0" ::"r"(&cpus[id]) : "memory");
}

void percpu_init(unsigned id) {
    percpu_attach(id);
    cpus[id].id = id;
}
