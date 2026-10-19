// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// A periodic timer tick driven by the ARMv8 generic timer (CNTP_EL0), delivered
// through KoraOS's interrupt controller (arch/irq.h). It maintains a monotonic
// tick counter and drives the scheduler (sched_tick: time slices and msleep).

#ifdef __cplusplus
extern "C" {
#endif

// Start the periodic tick at `hz` interrupts per second. Requires irq_init()
// first and irq_enable() to actually fire.
void systick_init(unsigned hz);

// Secondary cores: start this core's own tick (core 0 called systick_init).
void systick_init_this_core(void);

// Ticks elapsed since systick_init(), and the tick rate (0 before init).
uint64_t systick_count(void);
unsigned systick_hz(void);

// Register a callback invoked from the tick ISR on every tick (or NULL to
// clear). Used by the Circle CTimer adapter to poll kernel timers. Runs in
// interrupt context with IRQs masked, after the scheduler's tick.
void systick_set_tick_hook(void (*hook)(void));

#ifdef __cplusplus
}
#endif
