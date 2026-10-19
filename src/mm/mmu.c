#include "mm/mmu.h"
#include "arch/cache.h"
#include "arch/percpu.h"
#include "arch/sysregs.h"
#include "common.h"
#include "mm.h"
#include "peripherals/base.h"
#ifdef KORAOS_VIRT
#include "platform/virt.h"
extern char kernel_start[];
#endif

// End of the executable (code) region, 2 MB aligned by the linker script.
extern char text_end[];

// Start of MMIO in the low 4 GB. The Pi 4 (BCM2711, "low peripheral" mode) has
// peripherals from 0xFC000000 up, below PBASE: the PCIe host bridge is at
// 0xFD500000. (PBASE itself may be lower, as in the QEMU Pi 4 variant.)
#if defined(KORAOS_VIRT)
#define DEVICE_BASE 0
#elif RPI_VERSION == 4
#define DEVICE_BASE (PBASE < 0xFC000000UL ? PBASE : 0xFC000000UL)
// PCIe outbound window where the VL805 xHCI controller's registers appear
// (Circle's MEM_PCIE_RANGE_START, 64 MB), above the low 4 GB: mapped as one
// 1 GB Device block at level 1.
#define PCIE_WINDOW_BASE 0x600000000UL
#else
#define DEVICE_BASE PBASE
#endif

// Coherent (Normal non-cacheable) pool for bus-master buffers such as the
// VideoCore mailbox. 2 MB aligned and a whole number of 2 MB MMU blocks, which
// build_identity_map() maps non-cacheable. The Pi 4 needs 4 MB: Circle's xHCI
// driver takes coherent slots 256..1023 (circle/memory.h COHERENT_SLOT_XHCI_*)
// for its rings, contexts and scratchpad.
#if RPI_VERSION == 4
#define COHERENT_POOL_SIZE 0x400000
#else
#define COHERENT_POOL_SIZE 0x200000
#endif
static uint8_t coherent_pool[COHERENT_POOL_SIZE]
    __attribute__((aligned(SECTION_SIZE)));

void *coherent_page(unsigned slot) {
    uint64_t offset = (uint64_t)slot * PAGE_SIZE;
    if (offset + PAGE_SIZE > COHERENT_POOL_SIZE) {
        return NULL;
    }
    return coherent_pool + offset;
}

// Translation tables for a 48-bit VA space covering the low 4 GB with 2 MB
// blocks: one L0 (512 GB/entry), one L1 (1 GB/entry, 4 entries used) and four
// L2 tables (2 MB/entry). Statically reserved in BSS, 4 KB aligned.
#define ENTRIES_PER_TABLE 512
#define GIB_COVERED 4

static uint64_t l0_table[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t l1_table[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t l2_tables[GIB_COVERED][ENTRIES_PER_TABLE]
    __attribute__((aligned(PAGE_SIZE)));

// The first 2 MB block is split into 4 KB pages so that page 0, which belongs
// to the firmware (the ARM stub, the secondary cores' spin table, and at 0xF8
// the device-tree pointer), can be made writable on its own while the rest of
// the block, the start of the kernel image at 0x80000, stays code. Page 0 is
// read-only like the code by default, so NULL-pointer writes still fault.
#ifndef KORAOS_VIRT
static uint64_t l3_low_table[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));

static void build_low_pages(void) {
    for (uint64_t p = 0; p < ENTRIES_PER_TABLE; p++) {
        l3_low_table[p] = (p << PAGE_SHIFT) | MMU_PAGE_FLAGS(MMU_CODE_BLOCK_FLAGS);
    }
    l2_tables[0][0] = (uint64_t)l3_low_table | PD_TABLE;
}

#endif

void mmu_set_firmware_page_writable(int writable) {
#ifdef KORAOS_VIRT
    (void)writable;
#else
    // Only the access permissions change, which needs no break-before-make.
    l3_low_table[0] = 0 | MMU_PAGE_FLAGS(writable ? MMU_NORMAL_BLOCK_FLAGS
                                                  : MMU_CODE_BLOCK_FLAGS);
    asm volatile("dsb ishst" ::: "memory");
    asm volatile("tlbi vaae1is, %0" ::"r"(0UL) : "memory");
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");
#endif
}

static void build_identity_map(void) {
    uint64_t code_end = (uint64_t)text_end;
#ifdef KORAOS_VIRT
    const struct virt_platform *machine = virt_platform_get();
    uint64_t ram_end = machine->ram_base + machine->ram_size;
    uint64_t code_start = (uintptr_t)kernel_start & ~(uint64_t)(SECTION_SIZE - 1);
#endif

    l0_table[0] = (uint64_t)l1_table | PD_TABLE;

    for (uint64_t g = 0; g < GIB_COVERED; g++) {
        l1_table[g] = (uint64_t)l2_tables[g] | PD_TABLE;

        for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
            // Physical base of this 2 MB block.
            uint64_t addr = (g << 30) | (i << 21);

            uint64_t flags;
#ifdef KORAOS_VIRT
            if (addr < machine->ram_base || addr >= ram_end) {
                flags = MMU_DEVICE_BLOCK_FLAGS;
            } else if (addr >= code_start && addr < code_end) {
#else
            if (addr >= DEVICE_BASE) {
                flags = MMU_DEVICE_BLOCK_FLAGS;       // peripherals / MMIO
            } else if (addr < code_end) {
#endif
                flags = MMU_CODE_BLOCK_FLAGS;         // kernel + user text
            } else if (addr >= (uint64_t)coherent_pool &&
                       addr < (uint64_t)coherent_pool + COHERENT_POOL_SIZE) {
                flags = MMU_COHERENT_BLOCK_FLAGS;     // Normal non-cacheable pool
            } else {
                flags = MMU_NORMAL_BLOCK_FLAGS;       // general RAM, EL0+EL1 RW
            }

            l2_tables[g][i] = addr | flags;
        }
    }

#ifndef KORAOS_VIRT
    build_low_pages();
#endif

#ifdef PCIE_WINDOW_BASE
    l1_table[PCIE_WINDOW_BASE >> 30] = PCIE_WINDOW_BASE | MMU_DEVICE_BLOCK_FLAGS;
#endif
}

void mmu_init(void) {
    // Built with this core's caches still off, so the tables are already in
    // memory for the other cores' table walks (cacheable and coherent once
    // their MMUs are on; later changes use broadcast TLB maintenance).
    build_identity_map();
    mmu_enable_this_core();
}

void mmu_enable_this_core(void) {
    // Physical address size supported by the CPU (PARange), clamped to 48-bit.
    uint64_t mmfr0;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t parange = mmfr0 & 0xF;
    if (parange > 5) {
        parange = 5;
    }

    uint64_t tcr = TCR_VALUE | (parange << 32);

    asm volatile("msr mair_el1, %0" ::"r"((uint64_t)MAIR_VALUE));
    asm volatile("msr tcr_el1, %0" ::"r"(tcr));
    asm volatile("msr ttbr0_el1, %0" ::"r"((uint64_t)l0_table));
    asm volatile("isb");

    // Make sure the tables are visible and the TLB is clean before enabling.
    asm volatile("dsb ish");
    asm volatile("tlbi vmalle1");
    asm volatile("dsb ish");
    asm volatile("isb");

    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_MMU_ENABLED | SCTLR_D_CACHE | SCTLR_I_CACHE;
    asm volatile("msr sctlr_el1, %0" ::"r"(sctlr));
    asm volatile("isb");

    // Normal cacheable memory from here on: spinlocks (exclusives) work now.
    this_cpu()->caches_on = true;
}

void mmu_map_coherent(uintptr_t base, size_t size) {
    if (size == 0) {
        return;
    }
    uint64_t start = (uint64_t)base & ~(uint64_t)(SECTION_SIZE - 1);
    uint64_t end = ((uint64_t)base + size + SECTION_SIZE - 1) &
                   ~(uint64_t)(SECTION_SIZE - 1);
    if (end > ((uint64_t)GIB_COVERED << 30)) {
        end = (uint64_t)GIB_COVERED << 30;
    }

    // Nothing (an IRQ handler included) may touch these blocks while their
    // entries are briefly invalid below. This is a local mask, not a lock:
    // callers run under the BKL (boot, or a task), so no other core remaps at
    // the same time, and the TLB maintenance is broadcast (inner shareable) so
    // other cores drop the old entries too.
    uint64_t daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    asm volatile("msr daifset, #2" ::: "memory");

    // Write back and drop anything cached while the range was mapped
    // cacheable, so no dirty line is evicted over it later.
    dcache_clean_invalidate((const void *)start, end - start);

    for (uint64_t addr = start; addr < end; addr += SECTION_SIZE) {
#ifdef KORAOS_VIRT
        const struct virt_platform *machine = virt_platform_get();
        if (addr < machine->ram_base || addr >= machine->ram_base + machine->ram_size) {
            continue; // MMIO must remain Device memory.
        }
#else
        if (addr >= DEVICE_BASE) {
            break;  // already Device memory
        }
#endif
        uint64_t *entry = &l2_tables[addr >> 30][(addr >> 21) & (ENTRIES_PER_TABLE - 1)];
        // Break-before-make: a live mapping's cacheability may only change via
        // an invalid entry and a TLB flush.
        *entry = 0;
        asm volatile("dsb ishst" ::: "memory");
        asm volatile("tlbi vaae1is, %0" ::"r"(addr >> PAGE_SHIFT) : "memory");
        asm volatile("dsb ish" ::: "memory");
        *entry = addr | MMU_COHERENT_BLOCK_FLAGS;
    }
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");

    // Lines speculatively filled through the old mapping before the switch.
    dcache_clean_invalidate((const void *)start, end - start);

    asm volatile("msr daif, %0" ::"r"(daif) : "memory");
}
