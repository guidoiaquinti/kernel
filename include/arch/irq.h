// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// KoraOS interrupt handling: the CPU-side IRQ mask plus a small dispatcher over
// the platform interrupt controller (the legacy BCM controller on the Pi 3, the
// GIC-400 on the Pi 4; see peripherals/irq.h). This is KoraOS's own layer; the
// vendored Circle USB stack is bridged onto it rather than the other way
// around. IRQ numbers are the per-board KoraOS numbers from peripherals/irq.h
// (IRQ_TIMER_CNTPNS, IRQ_USB, ...), which match Circle's numbering.

typedef void (*irq_handler_t)(void *ctx);

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the interrupt controller and clear the handler table. Does not unmask
// CPU interrupts; call irq_enable() once handlers are ready.
void irq_init(void);

// Register a handler for an IRQ and enable (route to core 0) that IRQ in the
// controller. The device itself must still be told to raise it. Handlers run
// with CPU interrupts masked.
void irq_connect(unsigned irq, irq_handler_t handler, void *ctx);

// Disable an IRQ in the controller and unregister its handler.
void irq_disconnect(unsigned irq);

// Secondary cores: set up this core's side of the interrupt controller, and
// enable a per-core IRQ (the generic timer) whose handler core 0 connected.
void irq_init_this_core(void);
void irq_enable_this_core(unsigned irq);

// Unmask / mask IRQs at the CPU (PSTATE.I / DAIF).
void irq_enable(void);
void irq_disable(void);

// Mask IRQs at the CPU and return the previous mask state, for short critical
// sections over data an interrupt handler may also touch. Nests: pass the
// returned value to irq_restore() to put the mask back as it was.
uint64_t irq_save(void);
void irq_restore(uint64_t flags);

// Dispatch entry point called from the EL1/EL0 IRQ vectors. Not called directly.
void handle_irq(void);

// Diagnostics (the debug console's `irqs`): the number of IRQ lines, how often
// each IRQ was taken since irq_init(), a short name for a connected IRQ ("" if
// it has no handler), and the interrupt controller's name.
unsigned irq_lines(void);
unsigned long irq_hits(unsigned irq);
const char *irq_name(unsigned irq);
const char *irq_controller(void);

#ifdef __cplusplus
}
#endif
