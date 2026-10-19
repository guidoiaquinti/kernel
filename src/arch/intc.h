// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The platform interrupt controller behind arch/irq.c, private to src/arch.
// One implementation per board, selected by RPI_VERSION: irq_bcm.c (Pi 3,
// legacy BCM2835/BCM2836 controllers) and irq_gic.c (Pi 4, GIC-400). IRQ
// numbers are the KoraOS numbers from peripherals/irq.h.

// Controller name, for diagnostics.
extern const char intc_name[];

// Bring the controller up with every IRQ disabled and nothing pending (core 0).
void intc_init(void);

// Set up the calling core's side of the controller (secondary cores): the GIC
// CPU interface, or nothing routed to it on the BCM local controller.
void intc_init_cpu(void);

// Enable / disable one IRQ at the controller. Peripheral (shared) IRQs are
// routed to core 0; per-core sources (the generic timer: a GIC PPI, or a BCM
// local timer event) are enabled for the calling core.
void intc_enable(unsigned irq);
void intc_disable(unsigned irq);

// Called from handle_irq(): acknowledge every pending IRQ and pass each one to
// irq_dispatch().
void intc_handle(void);

// Provided by arch/irq.c: run the registered handler for one IRQ.
void irq_dispatch(unsigned irq);
