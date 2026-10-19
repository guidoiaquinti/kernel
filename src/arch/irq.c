// SPDX-License-Identifier: GPL-3.0-or-later
//
// KoraOS interrupt dispatch, independent of the interrupt controller: the
// handler table, per-IRQ counters, and the CPU-side mask. The controller
// itself is in irq_bcm.c (Pi 3) or irq_gic.c (Pi 4). See arch/irq.h.

#include "arch/irq.h"

#include "arch/spinlock.h"
#include "intc.h"
#include "peripherals/irq.h"

// irq_lock (IRQ-safe) keeps a handler/context pair consistent while it is
// (dis)connected and read for dispatch, which may happen on any core (per-core
// timer interrupts). Handlers run without it, so one may disconnect itself.
static struct spinlock irq_lock = SPINLOCK_INIT("irq table");

static struct {
    irq_handler_t handler;
    void *ctx;
    unsigned long hits;
} irq_table[IRQ_COUNT];

void irq_init(void) {
    for (unsigned i = 0; i < IRQ_COUNT; i++) {
        irq_table[i].handler = NULL;
        irq_table[i].ctx = NULL;
        irq_table[i].hits = 0;
    }
    intc_init();
}

void irq_connect(unsigned irq, irq_handler_t handler, void *ctx) {
    if (irq >= IRQ_COUNT) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&irq_lock);
    irq_table[irq].handler = handler;
    irq_table[irq].ctx = ctx;
    spin_unlock_irqrestore(&irq_lock, flags);
    intc_enable(irq);
}

void irq_init_this_core(void) {
    intc_init_cpu();
}

void irq_enable_this_core(unsigned irq) {
    if (irq < IRQ_COUNT) {
        intc_enable(irq);
    }
}

void irq_disconnect(unsigned irq) {
    if (irq >= IRQ_COUNT) {
        return;
    }
    intc_disable(irq);
    uint64_t flags = spin_lock_irqsave(&irq_lock);
    irq_table[irq].handler = NULL;
    irq_table[irq].ctx = NULL;
    spin_unlock_irqrestore(&irq_lock, flags);
}

void irq_dispatch(unsigned irq) {
    if (irq >= IRQ_COUNT) {
        return;
    }
    spin_lock(&irq_lock);  // in an interrupt: IRQs already masked
    irq_table[irq].hits++;
    irq_handler_t handler = irq_table[irq].handler;
    void *ctx = irq_table[irq].ctx;
    spin_unlock(&irq_lock);
    if (handler != NULL) {
        handler(ctx);
    }
}

void handle_irq(void) {
    intc_handle();
}

unsigned irq_lines(void) {
    return IRQ_COUNT;
}

unsigned long irq_hits(unsigned irq) {
    return irq < IRQ_COUNT ? irq_table[irq].hits : 0;
}

const char *irq_name(unsigned irq) {
    if (irq >= IRQ_COUNT || irq_table[irq].handler == NULL) {
        return "";
    }
#ifdef KORAOS_VIRT
    if (irq == IRQ_UART0) {
        return "uart (PL011)";
    }
#endif
    switch (irq) {
    case IRQ_TIMER_CNTPNS:
        return "timer (systick)";
#ifndef KORAOS_VIRT
    case IRQ_USB:
        return "usb (DWC2)";
    case IRQ_AUX:
        return "uart (mini-UART)";
    case IRQ_UART0:
        return "uart (PL011)";
#endif
#if defined(IRQ_PCIE_INTA) && !defined(KORAOS_VIRT)
    case IRQ_PCIE_INTA:
        return "usb (xHCI via PCIe)";
#endif
    default:
        return "connected";
    }
}

const char *irq_controller(void) {
    return intc_name;
}

void irq_enable(void) {
    asm volatile("msr daifclr, #2" ::: "memory");  // clear PSTATE.I
}

void irq_disable(void) {
    asm volatile("msr daifset, #2" ::: "memory");  // set PSTATE.I
}

uint64_t irq_save(void) {
    uint64_t daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    asm volatile("msr daifset, #2" ::: "memory");
    return daif;
}

void irq_restore(uint64_t flags) {
    asm volatile("msr daif, %0" ::"r"(flags) : "memory");
}
