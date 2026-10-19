#include "arch/sysregs.h"

.global _start

.section .text

.equ CORE_ID_MASK,     0xFF
.equ SECONDARY_STACK_SIZE, 16384     // matches smp.c

_start:
    // Preserve the QEMU/Linux boot ABI device tree pointer across EL changes.
    mov x20, x0
    // Park secondary cores
    mrs x1, MPIDR_EL1
    and x1, x1, #CORE_ID_MASK
    cbz x1, primary_core
    b CORE_PARK

primary_core:
    // Drop to EL1 if we booted in a higher exception level, then continue
    // at el1_entry. KoraOS runs its kernel in EL1.
    adr x2, el1_entry
    b switch_to_el1

el1_entry:
    // Set up stack pointer (SP_EL1)
    ldr x0, =stack_top
    mov sp, x0

    // Clear BSS
    ldr x10, =__bss_start
    ldr x11, =__bss_end
clear_bss:
    cmp x10, x11
    b.hs bss_cleared
    str xzr, [x10], #8
    b clear_bss

bss_cleared:
    // Trap FP/SIMD at EL0 and EL1. The kernel, Circle included, is integer-only;
    // the registers belong to EL0 tasks and are handed out lazily on first use
    // (src/arch/fpsimd.c), so any kernel FP instruction now faults loudly.
    mrs x0, cpacr_el1
    bic x0, x0, #(3 << 20)      // FPEN = 0b00: trap FP/SIMD at EL0 and EL1
    msr cpacr_el1, x0
    isb

    // Jump to kernel main with the original boot argument.
    mov x0, x20
    bl kernel_main
    // If kernel_main returns, hang
    b hang

// Secondary cores start here, released by smp.c (PSCI CPU_ON on virt, the
// firmware spin table on the Pis), at EL2 (or EL3, or EL1 under PSCI without
// EL2) with the MMU off. Each gets its own stack and calls secondary_main(id),
// which turns its MMU and caches on before touching anything shared.
.global secondary_entry
secondary_entry:
    mrs x19, MPIDR_EL1
    and x19, x19, #CORE_ID_MASK     // survives the eret below
    adr x2, secondary_el1
    b switch_to_el1

secondary_el1:
    ldr x0, =secondary_stacks       // stack top: secondary_stacks[id + 1]
    mov x1, #SECONDARY_STACK_SIZE
    madd x0, x19, x1, x0
    add x0, x0, x1
    mov sp, x0

    mrs x0, cpacr_el1               // trap FP/SIMD, as on core 0
    bic x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    mov x0, x19
    bl secondary_main
    b hang

// switch_to_el1: configure the appropriate higher EL and eret down to EL1,
// landing at x2. If already in EL1, branch there directly. Uses only
// scratch registers (no stack), since SP is not set up until afterwards.
switch_to_el1:
    mrs x0, CurrentEL
    lsr x0, x0, #2          // x0 = current EL (1, 2 or 3)

    cmp x0, #3
    bne check_el2

    // --- From EL3: route directly down to EL1 ---
    ldr x1, =SCR_VALUE
    msr scr_el3, x1
    ldr x1, =HCR_VALUE          // EL1 is AArch64
    msr hcr_el2, x1
    ldr x1, =SCTLR_VALUE_MMU_DISABLED
    msr sctlr_el1, x1
    ldr x1, =SPSR_VALUE_EL1     // return to EL1h, all interrupts masked
    msr spsr_el3, x1
    msr elr_el3, x2
    eret

check_el2:
    cmp x0, #2
    bne already_el1

    // --- From EL2: drop to EL1 ---
    ldr x1, =HCR_VALUE          // EL1 is AArch64
    msr hcr_el2, x1
    ldr x1, =SCTLR_VALUE_MMU_DISABLED
    msr sctlr_el1, x1
    // Let EL1 use the physical/virtual timers without trapping.
    mrs x1, cnthctl_el2
    orr x1, x1, #3
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    ldr x1, =SPSR_VALUE_EL1     // return to EL1h, all interrupts masked
    msr spsr_el2, x1
    msr elr_el2, x2
    eret

already_el1:
    br x2

CORE_PARK:
    mov x9, x1
SECONDARY_WAIT:
    wfi
    b SECONDARY_WAIT

hang:
    wfi
    b hang
