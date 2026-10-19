// SPDX-License-Identifier: GPL-3.0-or-later
//
// Legacy Broadcom interrupt controller for the Raspberry Pi 3 (and QEMU
// raspi3b): BCM2835 enable/pending registers for peripheral IRQs 0..63, and
// the BCM2836 per-core local controller for the generic timer. See intc.h.

#include "peripherals/irq.h"

#if RPI_VERSION != 4 && !defined(KORAOS_VIRT)

#include "arch/percpu.h"
#include "intc.h"
#include "memory_access.h"

const char intc_name[] = "BCM2835 legacy";

// The BCM2836 local controller has one timer-routing and one source register
// per core, 4 bytes apart; peripheral (GPU) IRQs go to core 0 only
// (LOCAL_GPU_ROUTING keeps its reset value).
static uintptr_t timer_irqcntl(void) {
    return CORE0_TIMER_IRQCNTL + 4 * this_cpu()->id;
}

static uintptr_t irq_source(void) {
    return CORE0_IRQ_SOURCE + 4 * this_cpu()->id;
}

void intc_init(void) {
    // Mask every peripheral IRQ to start from a known-quiet state.
    write32(DISABLE_IRQS_1, 0xFFFFFFFF);
    write32(DISABLE_IRQS_2, 0xFFFFFFFF);
    write32(DISABLE_BASIC_IRQS, 0xFFFFFFFF);
    intc_init_cpu();
}

void intc_init_cpu(void) {
    write32(timer_irqcntl(), 0);
    asm volatile("dsb sy" ::: "memory");
}

// Local sources 0..3 are the generic-timer events; their routing bits in the
// core's timer control register are in the same order as in its source one.
static void route_local_timer(unsigned irq, int on) {
    uint32_t bit = 1u << (irq - IRQ_LOCAL_BASE);
    uint32_t cntl = read32(timer_irqcntl());
    write32(timer_irqcntl(), on ? (cntl | bit) : (cntl & ~bit));
}

void intc_enable(unsigned irq) {
    if (irq < 32) {
        write32(ENABLE_IRQS_1, 1u << irq);
    } else if (irq < 64) {
        write32(ENABLE_IRQS_2, 1u << (irq - 32));
    } else if (irq < IRQ_LOCAL_BASE + 4) {
        route_local_timer(irq, 1);
    }
    asm volatile("dsb sy" ::: "memory");
}

void intc_disable(unsigned irq) {
    if (irq < 32) {
        write32(DISABLE_IRQS_1, 1u << irq);
    } else if (irq < 64) {
        write32(DISABLE_IRQS_2, 1u << (irq - 32));
    } else if (irq < IRQ_LOCAL_BASE + 4) {
        route_local_timer(irq, 0);
    }
    asm volatile("dsb sy" ::: "memory");
}

static void dispatch_pending(uint32_t pending, unsigned base) {
    while (pending != 0) {
        unsigned bit = (unsigned)__builtin_ctz(pending);
        irq_dispatch(base + bit);
        pending &= ~(1u << bit);
    }
}

void intc_handle(void) {
    uint32_t source = read32(irq_source());

    // Local per-core sources: generic-timer events (bits 0..3).
    dispatch_pending(source & 0xF, IRQ_LOCAL_BASE);

    // Peripheral (GPU) IRQs are signalled by one aggregate bit; the specific
    // lines come from the BCM2835 pending registers.
    if (source & CORE_IRQ_GPU) {
        dispatch_pending(read32(IRQ_PENDING_1), 0);
        dispatch_pending(read32(IRQ_PENDING_2), 32);
    }
}

#endif  // RPI_VERSION != 4
