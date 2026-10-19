// SPDX-License-Identifier: GPL-3.0-or-later
//
// Secondary core bring-up. See arch/smp.h and docs/smp.md.
//
// A released core starts at secondary_entry (src/boot.S) with its MMU and data
// cache off, so its memory accesses bypass the caches while core 0's are
// cached. Until its own MMU is on it therefore writes nothing shared (only its
// stack, which core 0 has written back and invalidated before releasing it,
// and system registers), and it never takes a spinlock (exclusives need
// cacheable memory; see src/arch/spinlock.c).

#include "arch/smp.h"

#include "arch/cache.h"
#include "arch/exception.h"
#include "arch/irq.h"
#include "arch/percpu.h"
#include "arch/systick.h"
#include "lib/printf.h"
#include "lib/timer.h"
#include "mm/mmu.h"
#include "proc/task.h"
#ifdef KORAOS_VIRT
#include "platform/virt.h"
#endif

#define SECONDARY_STACK_SIZE 16384  // matches src/boot.S

// Boot stacks for cores 1..MAX_CPUS-1 (slot 0 unused: core 0 has the boot
// stack); secondary_entry puts core id's stack top at [id + 1].
uint8_t secondary_stacks[MAX_CPUS][SECONDARY_STACK_SIZE] __attribute__((aligned(64)));

extern char secondary_entry[];

void secondary_main(unsigned id);

void secondary_main(unsigned id) {
    // Caches off: registers and the private stack only.
    percpu_attach(id);
    mmu_enable_this_core();

    // Caches on: shared data and spinlocks are usable from here.
    percpu_init(id);
    exception_init();
    task_init_idle();
    irq_init_this_core();
    systick_init_this_core();

    __atomic_store_n(&this_cpu()->online, true, __ATOMIC_RELEASE);
    irq_enable();
    for (;;) {
        asm volatile("wfi");
    }
}

#ifdef KORAOS_VIRT

#define PSCI_CPU_ON_64 0xC4000003u

// PSCI CPU_ON(target MPIDR, entry point, context) through the conduit the
// device tree names. Returns the PSCI status (0 = success).
static long psci_cpu_on(enum virt_psci_method method, uint64_t mpidr, uintptr_t entry) {
    register uint64_t x0 asm("x0") = PSCI_CPU_ON_64;
    register uint64_t x1 asm("x1") = mpidr;
    register uint64_t x2 asm("x2") = entry;
    register uint64_t x3 asm("x3") = 0;
    if (method == VIRT_PSCI_HVC) {
        asm volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    } else {
        asm volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    }
    return (long)x0;
}

#else

// The Raspberry Pi firmware's ARM stub parks cores 1..3 in WFE, each polling
// a 64-bit release address in the spin table at 0xd8 + 8 * core (core 0's
// slot is unused); a non-zero value is the entry point.
#define SPIN_TABLE_BASE 0xd8

static void release_from_spin_table(unsigned core) {
    volatile uint64_t *slot = (volatile uint64_t *)(uintptr_t)(SPIN_TABLE_BASE + 8 * core);
    mmu_set_firmware_page_writable(1);
    *slot = (uintptr_t)secondary_entry;
    // The waiting core reads the slot with its caches off.
    dcache_clean((const void *)slot, sizeof(*slot));
    mmu_set_firmware_page_writable(0);
    asm volatile("dsb sy\n\tsev" ::: "memory");
}

#endif

static bool wait_online(unsigned core) {
    uint64_t deadline = timer_us() + 1000000;
    while (!__atomic_load_n(&cpus[core].online, __ATOMIC_ACQUIRE)) {
        if (timer_us() > deadline) {
            return false;
        }
    }
    return true;
}

static void start_core(unsigned core, uint64_t mpidr) {
    // Nothing of core 0's may linger in the cache over what the new core
    // writes with its caches still off: its stack and its per-CPU entry.
    dcache_clean_invalidate(secondary_stacks[core], SECONDARY_STACK_SIZE);
    dcache_clean_invalidate(&cpus[core], sizeof(cpus[core]));

#ifdef KORAOS_VIRT
    const struct virt_platform *machine = virt_platform_get();
    long status = psci_cpu_on(machine->psci_method, mpidr, (uintptr_t)secondary_entry);
    if (status != 0) {
        printf("smp: cpu %u: PSCI CPU_ON failed (%ld)\n", core, status);
        return;
    }
#else
    (void)mpidr;
    release_from_spin_table(core);
#endif
    if (wait_online(core)) {
        printf("smp: cpu %u online\n", core);
    } else {
        printf("smp: cpu %u did not come online\n", core);
    }
}

unsigned smp_start_secondaries(void) {
#ifdef KORAOS_VIRT
    const struct virt_platform *machine = virt_platform_get();
    if (machine->psci_method == VIRT_PSCI_NONE) {
        printf("smp: no PSCI, staying on one core\n");
        return 1;
    }
    uint64_t self;
    asm volatile("mrs %0, mpidr_el1" : "=r"(self));
    for (unsigned i = 0; i < machine->cpu_count && i < VIRT_MAX_CPUS; i++) {
        uint64_t mpidr = machine->cpu_mpidr[i];
        unsigned core = (unsigned)(mpidr & 0xff);
        if ((mpidr & 0xff00ffffffull) == (self & 0xff00ffffffull) || core >= MAX_CPUS ||
            (mpidr & 0xff00ffff00ull) != 0) {
            continue;  // ourselves, or beyond the cores KoraOS manages
        }
        start_core(core, mpidr);
    }
#else
    for (unsigned core = 1; core < MAX_CPUS; core++) {
        start_core(core, core);
    }
#endif
    unsigned online = smp_cores_online();
    printf("smp: %u cores online\n", online);
    return online;
}

unsigned smp_cores_online(void) {
    unsigned n = 1;
    for (unsigned core = 1; core < MAX_CPUS; core++) {
        n += __atomic_load_n(&cpus[core].online, __ATOMIC_ACQUIRE) ? 1 : 0;
    }
    return n;
}
