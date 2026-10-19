// SPDX-License-Identifier: GPL-3.0-or-later
//
// GIC-400 interrupt controller for the Raspberry Pi 4. See intc.h.
//
// The firmware's ARM stub (enable_gic=1) puts every interrupt in the
// non-secure group before the kernel starts, so the kernel, running
// non-secure at EL1, owns the distributor and CPU interface from here. All
// interrupts go to core 0 at one priority, level-triggered: the same setup as
// Circle's interruptgic.cpp.

#include "peripherals/irq.h"

#if RPI_VERSION == 4 || defined(KORAOS_VIRT)

#include "intc.h"
#include "memory_access.h"

#ifdef KORAOS_VIRT
#include "gic3.h"

// QEMU virt has a GICv2 or, with gic-version=3 (and under hvf), a GICv3; the
// device tree says which, and intc_name follows.
const char *intc_name = "GICv2 (virt)";

static bool gic3(void) {
    return virt_platform_get()->gic_version == 3;
}

void gic_virt_edge_triggers(void) {
    // QEMU virtio-mmio transports advertise rising-edge SPIs in their DTB.
    const struct virt_platform *machine = virt_platform_get();
    for (unsigned i = 0; i < machine->virtio_count; i++) {
        const struct virt_mmio_device *dev = &machine->virtio[i];
        if (dev->edge_triggered) {
            uintptr_t reg = GICD_ICFGR0 + 4 * (dev->irq / 16);
            write32(reg, read32(reg) | (2u << (2 * (dev->irq % 16))));
        }
    }
}
#else
const char *intc_name = "GIC-400";
#endif

void intc_init(void) {
#ifdef KORAOS_VIRT
    if (gic3()) {
        intc_name = "GICv3 (virt)";
        gic3_init();
        return;
    }
#endif
    write32(GICD_CTLR, 0);

    // Disable, un-pend and deactivate everything.
    for (unsigned n = 0; n < IRQ_COUNT / 32; n++) {
        write32(GICD_ICENABLER0 + 4 * n, 0xFFFFFFFF);
        write32(GICD_ICPENDR0 + 4 * n, 0xFFFFFFFF);
        write32(GICD_ICACTIVER0 + 4 * n, 0xFFFFFFFF);
    }

    // One priority for all, every SPI to core 0 (the SGI/PPI target bytes are
    // read-only and always name the local core).
    for (unsigned n = 0; n < IRQ_COUNT / 4; n++) {
        write32(GICD_IPRIORITYR0 + 4 * n, GIC_PRIORITY_DEFAULT * 0x01010101u);
        write32(GICD_ITARGETSR0 + 4 * n, GIC_TARGET_CORE0 * 0x01010101u);
    }

    // Level-triggered.
    for (unsigned n = 0; n < IRQ_COUNT / 16; n++) {
        write32(GICD_ICFGR0 + 4 * n, 0);
    }

#ifdef KORAOS_VIRT
    gic_virt_edge_triggers();
#endif

    write32(GICD_CTLR, GICD_CTLR_ENABLE);
    intc_init_cpu();
}

// The CPU interface, and the SGI/PPI enables in GICD_ISENABLER0, are banked:
// each core sets up its own.
void intc_init_cpu(void) {
#ifdef KORAOS_VIRT
    if (gic3()) {
        gic3_init_cpu();
        return;
    }
#endif
    write32(GICC_PMR, GICC_PMR_ALLOW_ALL);  // let every priority through
    write32(GICC_CTLR, GICC_CTLR_ENABLE);
    asm volatile("dsb sy" ::: "memory");
}

void intc_enable(unsigned irq) {
#ifdef KORAOS_VIRT
    if (gic3()) {
        gic3_enable(irq);
        return;
    }
#endif
    write32(GICD_ISENABLER0 + 4 * (irq / 32), 1u << (irq % 32));
    asm volatile("dsb sy" ::: "memory");
}

void intc_disable(unsigned irq) {
#ifdef KORAOS_VIRT
    if (gic3()) {
        gic3_disable(irq);
        return;
    }
#endif
    write32(GICD_ICENABLER0 + 4 * (irq / 32), 1u << (irq % 32));
    asm volatile("dsb sy" ::: "memory");
}

void intc_handle(void) {
#ifdef KORAOS_VIRT
    if (gic3()) {
        gic3_handle();
        return;
    }
#endif
    // Acknowledge until the CPU interface reports nothing pending. Reading IAR
    // activates the IRQ; writing the same value to EOIR completes it.
    for (;;) {
        uint32_t iar = read32(GICC_IAR);
        unsigned irq = iar & GICC_IAR_ID_MASK;
        if (irq >= IRQ_COUNT) {
            break;  // spurious (1023): nothing (more) pending
        }
        irq_dispatch(irq);
        write32(GICC_EOIR, iar);
    }
}

#endif  // RPI_VERSION == 4
