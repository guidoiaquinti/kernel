// EL1 exception vector table and trap entry/exit for KoraOS.
//
// The save/restore layout below must stay in sync with struct trapframe in
// include/arch/trapframe.h (total size 272 bytes).

.global vectors

// Save full integer context onto the (kernel) stack as a struct trapframe.
.macro kernel_entry
    sub sp, sp, #272
    stp x0,  x1,  [sp, #16 * 0]
    stp x2,  x3,  [sp, #16 * 1]
    stp x4,  x5,  [sp, #16 * 2]
    stp x6,  x7,  [sp, #16 * 3]
    stp x8,  x9,  [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x19, [sp, #16 * 9]
    stp x20, x21, [sp, #16 * 10]
    stp x22, x23, [sp, #16 * 11]
    stp x24, x25, [sp, #16 * 12]
    stp x26, x27, [sp, #16 * 13]
    stp x28, x29, [sp, #16 * 14]
    str x30,      [sp, #240]
    mrs x21, sp_el0
    mrs x22, elr_el1
    mrs x23, spsr_el1
    stp x21, x22, [sp, #248]    // sp -> 248, elr -> 256
    str x23,      [sp, #264]    // spsr -> 264
.endm

// Restore context saved by kernel_entry and return to the interrupted context.
.macro kernel_exit
    ldr x23,      [sp, #264]
    ldp x21, x22, [sp, #248]
    msr spsr_el1, x23
    msr elr_el1,  x22
    msr sp_el0,   x21
    ldp x0,  x1,  [sp, #16 * 0]
    ldp x2,  x3,  [sp, #16 * 1]
    ldp x4,  x5,  [sp, #16 * 2]
    ldp x6,  x7,  [sp, #16 * 3]
    ldp x8,  x9,  [sp, #16 * 4]
    ldp x10, x11, [sp, #16 * 5]
    ldp x12, x13, [sp, #16 * 6]
    ldp x14, x15, [sp, #16 * 7]
    ldp x16, x17, [sp, #16 * 8]
    ldp x18, x19, [sp, #16 * 9]
    ldp x20, x21, [sp, #16 * 10]
    ldp x22, x23, [sp, #16 * 11]
    ldp x24, x25, [sp, #16 * 12]
    ldp x26, x27, [sp, #16 * 13]
    ldp x28, x29, [sp, #16 * 14]
    ldr x30,      [sp, #240]
    add sp, sp, #272
    eret
.endm

// One vector table slot: 0x80-aligned, just branch to the stub.
.macro ventry label
.align 7
    b \label
.endm

// Unexpected exception: report type/ESR/FAR and halt.
.macro handle_invalid type
    kernel_entry
    mov x0, #\type
    mrs x1, esr_el1
    mrs x2, far_el1
    mov x3, sp
    bl  handle_invalid_entry
    kernel_exit
.endm

.align 11
vectors:
    // Current EL with SP_EL0 (EL1t) - unused
    ventry el1t_sync
    ventry el1t_irq
    ventry el1t_fiq
    ventry el1t_error

    // Current EL with SP_EL1 (EL1h) - kernel-mode exceptions
    ventry el1h_sync
    ventry el1h_irq
    ventry el1h_fiq
    ventry el1h_error

    // Lower EL using AArch64 (EL0) - this is where user traps land
    ventry el0_sync
    ventry el0_irq
    ventry el0_fiq
    ventry el0_error

    // Lower EL using AArch32 - unused (KoraOS is AArch64 only)
    ventry el0_32_sync
    ventry el0_32_irq
    ventry el0_32_fiq
    ventry el0_32_error

// --- Real handlers ---

el1h_sync:
    kernel_entry
    mrs x0, esr_el1
    mrs x1, far_el1
    mov x2, sp
    bl  handle_sync_el1
    kernel_exit

// A trap from EL0 runs under the big kernel lock: taken here, released before
// returning (unless the task is forbid()den). Interrupts never take it.
// Every return to EL0 passes sched_preempt_check, which switches tasks if the
// tick or a wake-up asked for it. The kernel is not preemptible, so this is
// the only place a running task is switched out against its will.
el0_sync:
    kernel_entry
    bl  bkl_enter_from_el0
    mrs x0, esr_el1
    mrs x1, far_el1
    mov x2, sp
    bl  handle_sync_el0
    bl  bkl_exit_to_el0
    bl  sched_preempt_check
    kernel_exit

// IRQ from the kernel (EL1h) or from userland (EL0): save context, dispatch
// through the interrupt controller, restore. Interrupts stay masked (PSTATE.I
// set on entry) for the duration, so handlers do not nest.
el1h_irq:
    kernel_entry
    bl  handle_irq
    kernel_exit

el0_irq:
    kernel_entry
    bl  handle_irq
    bl  sched_preempt_check
    kernel_exit

// A new task's first return to EL0: cpu_switch() lands here with sp pointing
// at the trap frame thread_setup() prepared at the top of its kernel stack. It
// may have been killed before it ever ran: sched_preempt_check lets it die.
.global ret_to_user
ret_to_user:
    bl  schedule_tail
    bl  sched_preempt_check
    kernel_exit

// --- Invalid / unexpected entries (numbered by vector index) ---

el1t_sync:    handle_invalid 0
el1t_irq:     handle_invalid 1
el1t_fiq:     handle_invalid 2
el1t_error:   handle_invalid 3
el1h_fiq:     handle_invalid 6
el1h_error:   handle_invalid 7
el0_fiq:      handle_invalid 10
el0_error:    handle_invalid 11
el0_32_sync:  handle_invalid 12
el0_32_irq:   handle_invalid 13
el0_32_fiq:   handle_invalid 14
el0_32_error: handle_invalid 15
