// SPDX-License-Identifier: GPL-3.0-or-later
//
// GICv3 for QEMU virt (gic-version=3, which QEMU's hvf accelerator requires).
// Selected at boot from the device tree by irq_gic.c, behind the same intc.h
// interface as the GICv2.
//
// The distributor runs with affinity routing (ARE) and routes every shared
// interrupt (SPI) to core 0. Each core owns a redistributor frame, found by
// matching its MPIDR, which holds its SGI/PPI state (the timer); the CPU
// interface is the ICC_* system registers. Everything is Group 1 at one
// priority, level-triggered, like the GICv2 setup.

#ifdef KORAOS_VIRT

#include "arch/percpu.h"
#include "gic3.h"
#include "intc.h"
#include "memory_access.h"
#include "peripherals/irq.h"
#include "platform/virt.h"

#define GICD_CTLR_ARE      BIT(4)
#define GICD_CTLR_GRP1     BIT(1)
#define GICD_CTLR_RWP      BIT(31)
#define GICD_IGROUPR(n)    (GICD_BASE + 0x080 + 4 * (n))
#define GICD_IROUTER(n)    (GICD_BASE + 0x6000 + 8 * (n))

// Redistributor frame: RD_base, then SGI_base 64 KB later.
#define GICR_TYPER         0x0008   // 64-bit: affinity [63:32], Last bit 4
#define GICR_WAKER         0x0014
#define GICR_SGI           0x10000
#define GICR_IGROUPR0      (GICR_SGI + 0x080)
#define GICR_ISENABLER0    (GICR_SGI + 0x100)
#define GICR_ICENABLER0    (GICR_SGI + 0x180)
#define GICR_ICPENDR0      (GICR_SGI + 0x280)
#define GICR_ICACTIVER0    (GICR_SGI + 0x380)
#define GICR_IPRIORITYR(n) (GICR_SGI + 0x400 + 4 * (n))
#define GICR_ICFGR1        (GICR_SGI + 0xC04)

#define GICR_TYPER_LAST    BIT(4)
#define GICR_TYPER_VLPIS   BIT(1)    // GICv4: two more 64 KB frames per core
#define GICR_WAKER_SLEEP   BIT(1)
#define GICR_WAKER_ASLEEP  BIT(2)

#define ICC_IAR_SPECIAL    1020      // 1020..1023: nothing (more) to handle

static void wait_distributor(void) {
    while (read32(GICD_CTLR) & GICD_CTLR_RWP) {
    }
}

// This core's redistributor: the frame whose GICR_TYPER affinity matches the
// core's MPIDR (Aff3.Aff2.Aff1.Aff0).
static uintptr_t find_redistributor(void) {
    const struct virt_platform *machine = virt_platform_get();
    uint64_t mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t affinity = (uint32_t)(((mpidr >> 32) & 0xff) << 24 | (mpidr & 0xffffff));
    uintptr_t frame = machine->gic_redist;
    uintptr_t end = machine->gic_redist + machine->gic_redist_size;
    while (frame < end) {
        uint64_t typer = read64(frame + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == affinity) {
            return frame;
        }
        if (typer & GICR_TYPER_LAST) {
            break;
        }
        frame += (typer & GICR_TYPER_VLPIS) ? 0x40000 : 0x20000;
    }
    return 0;
}

void gic3_init(void) {
    write32(GICD_CTLR, 0);
    wait_distributor();

    // SPIs only (32 and up; SGIs/PPIs live in each redistributor): disabled,
    // idle, Group 1, one priority, level-triggered, routed to core 0.
    uint64_t mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint64_t core0 = mpidr & 0xff00ffffffull;
    for (unsigned n = 1; n < IRQ_COUNT / 32; n++) {
        write32(GICD_ICENABLER0 + 4 * n, 0xFFFFFFFF);
        write32(GICD_ICPENDR0 + 4 * n, 0xFFFFFFFF);
        write32(GICD_ICACTIVER0 + 4 * n, 0xFFFFFFFF);
        write32(GICD_IGROUPR(n), 0xFFFFFFFF);
    }
    for (unsigned n = 32 / 4; n < IRQ_COUNT / 4; n++) {
        write32(GICD_IPRIORITYR0 + 4 * n, GIC_PRIORITY_DEFAULT * 0x01010101u);
    }
    for (unsigned n = 32 / 16; n < IRQ_COUNT / 16; n++) {
        write32(GICD_ICFGR0 + 4 * n, 0);
    }
    for (unsigned irq = 32; irq < IRQ_COUNT; irq++) {
        write64(GICD_IROUTER(irq), core0);
    }
    gic_virt_edge_triggers();
    wait_distributor();

    write32(GICD_CTLR, GICD_CTLR_ARE | GICD_CTLR_GRP1);
    wait_distributor();
    gic3_init_cpu();
}

void gic3_init_cpu(void) {
    uintptr_t rd = find_redistributor();
    this_cpu()->gic_redist = rd;
    if (rd == 0) {
        return;  // no frame for this core: it gets no interrupts
    }

    // Wake the redistributor up.
    write32(rd + GICR_WAKER, read32(rd + GICR_WAKER) & ~GICR_WAKER_SLEEP);
    while (read32(rd + GICR_WAKER) & GICR_WAKER_ASLEEP) {
    }

    // This core's SGIs and PPIs: disabled, idle, Group 1, one priority,
    // level-triggered PPIs.
    write32(rd + GICR_ICENABLER0, 0xFFFFFFFF);
    write32(rd + GICR_ICPENDR0, 0xFFFFFFFF);
    write32(rd + GICR_ICACTIVER0, 0xFFFFFFFF);
    write32(rd + GICR_IGROUPR0, 0xFFFFFFFF);
    for (unsigned n = 0; n < 32 / 4; n++) {
        write32(rd + GICR_IPRIORITYR(n), GIC_PRIORITY_DEFAULT * 0x01010101u);
    }
    write32(rd + GICR_ICFGR1, 0);

    // The CPU interface: system registers on, every priority through, Group 1
    // enabled. (Booting through EL2, boot.S has let EL1 use them.)
    uint64_t sre;
    asm volatile("mrs %0, icc_sre_el1" : "=r"(sre));
    asm volatile("msr icc_sre_el1, %0\n\tisb" ::"r"(sre | 1));
    asm volatile("msr icc_pmr_el1, %0" ::"r"((uint64_t)GICC_PMR_ALLOW_ALL));
    asm volatile("msr icc_bpr1_el1, xzr");
    asm volatile("msr icc_ctlr_el1, xzr");  // EOI also deactivates
    asm volatile("msr icc_igrpen1_el1, %0\n\tisb" ::"r"(1ull));
}

void gic3_enable(unsigned irq) {
    if (irq < 32) {
        uintptr_t rd = this_cpu()->gic_redist;
        if (rd != 0) {
            write32(rd + GICR_ISENABLER0, 1u << irq);
        }
    } else {
        write32(GICD_ISENABLER0 + 4 * (irq / 32), 1u << (irq % 32));
    }
    asm volatile("dsb sy" ::: "memory");
}

void gic3_disable(unsigned irq) {
    if (irq < 32) {
        uintptr_t rd = this_cpu()->gic_redist;
        if (rd != 0) {
            write32(rd + GICR_ICENABLER0, 1u << irq);
        }
    } else {
        write32(GICD_ICENABLER0 + 4 * (irq / 32), 1u << (irq % 32));
        wait_distributor();
    }
    asm volatile("dsb sy" ::: "memory");
}

void gic3_handle(void) {
    for (;;) {
        uint64_t iar;
        asm volatile("mrs %0, icc_iar1_el1" : "=r"(iar));
        unsigned irq = (unsigned)(iar & 0xffffff);
        if (irq >= ICC_IAR_SPECIAL) {
            break;
        }
        if (irq < IRQ_COUNT) {
            irq_dispatch(irq);
        }
        asm volatile("msr icc_eoir1_el1, %0\n\tisb" ::"r"(iar));
    }
}

#endif  // KORAOS_VIRT
