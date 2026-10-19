// SPDX-License-Identifier: GPL-3.0-or-later
//
// Periodic system tick on the generic timer. See arch/systick.h.

#include "arch/systick.h"

#include "arch/irq.h"
#include "arch/percpu.h"
#include "lib/timer.h"
#include "peripherals/irq.h"
#include "proc/task.h"

// Every core runs its own tick on its own generic timer (deadline and count in
// its per-CPU data); core 0's is the system tick: the global count, the
// scheduler and the Circle timer hook.
static volatile uint64_t g_ticks;
static unsigned g_hz;
static uint64_t g_interval;  // timer ticks between interrupts
static void (*g_tick_hook)(void);

static inline uint64_t read_cntpct(void) {
    uint64_t v;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_cval(uint64_t v) {
    asm volatile("msr cntp_cval_el0, %0" ::"r"(v));
}

static inline void write_cntp_ctl(uint64_t v) {
    asm volatile("msr cntp_ctl_el0, %0" ::"r"(v));
}

static void systick_isr(void *ctx) {
    (void)ctx;
    struct cpu *c = this_cpu();
    c->ticks++;
    // Advance the absolute deadline by one interval so periods do not drift with
    // interrupt latency (re-arming a relative TVAL would lose each overshoot).
    c->tick_deadline += g_interval;
    write_cntp_cval(c->tick_deadline);
    if (c->id != 0) {
        return;  // secondary cores have no work yet (Milestone 7 PR 5)
    }
    g_ticks++;
    sched_tick(g_ticks);
    if (g_tick_hook != NULL) {
        g_tick_hook();
    }
}

// Arm this core's timer and let EL0 read the virtual counter
// (CNTKCTL_EL1.EL0VCTEN), so programs can time themselves without a syscall;
// CNTFRQ_EL0 is always readable.
static void start_this_core(void) {
    uint64_t kctl;
    asm volatile("mrs %0, cntkctl_el1" : "=r"(kctl));
    asm volatile("msr cntkctl_el1, %0" ::"r"(kctl | (1u << 1)));

    // Arm the first absolute deadline and enable the timer (bit0=ENABLE,
    // bit1=IMASK=0).
    struct cpu *c = this_cpu();
    c->tick_deadline = read_cntpct() + g_interval;
    write_cntp_cval(c->tick_deadline);
    write_cntp_ctl(1);
    asm volatile("isb");
}

void systick_init_this_core(void) {
    if (g_interval == 0) {
        return;
    }
    start_this_core();
    irq_enable_this_core(IRQ_TIMER_CNTPNS);
}

void systick_set_tick_hook(void (*hook)(void)) {
    g_tick_hook = hook;
}

void systick_init(unsigned hz) {
    if (hz == 0) {
        return;
    }
    g_hz = hz;
    g_interval = timer_freq_hz() / hz;
    g_ticks = 0;

    // Enabling the IRQ also routes the timer event to this core (the local
    // controller on the Pi 3, a per-core PPI in the GIC on the Pi 4).
    irq_connect(IRQ_TIMER_CNTPNS, systick_isr, NULL);

    start_this_core();
}

uint64_t systick_count(void) {
    return g_ticks;
}

unsigned systick_hz(void) {
    return g_hz;
}
