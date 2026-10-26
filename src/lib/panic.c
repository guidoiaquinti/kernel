// SPDX-License-Identifier: GPL-3.0-or-later
//
// Kernel panic and the locked printf. See lib/panic.h.

#include "lib/panic.h"

#include <stdarg.h>

#include "arch/irq.h"
#include "arch/percpu.h"
#include "arch/spinlock.h"
#include "lib/printf.h"

static struct spinlock printf_lock = SPINLOCK_INIT("printf");
static bool printf_lock_ready;  // set by printf_lock_init

// A panicking core prints without the lock: whoever holds it may never let go.
static unsigned long printf_lock_take(void) {
    uint64_t flags = irq_save();
    if (!this_cpu()->panicking) {
        spin_lock(&printf_lock);
    }
    return flags;
}

static void printf_lock_drop(unsigned long flags) {
    if (spin_is_held_here(&printf_lock)) {
        spin_unlock(&printf_lock);
    }
    irq_restore(flags);
}

void printf_lock_init(void) {
    tfp_set_output_lock(printf_lock_take, printf_lock_drop);
    printf_lock_ready = true;
}

unsigned long serial_out_begin(void) {
    return printf_lock_ready ? printf_lock_take() : irq_save();
}

void serial_out_end(unsigned long flags) {
    printf_lock_drop(flags);  // releases the lock only if this core holds it
}

void panic_begin(void) {
    irq_disable();
    this_cpu()->panicking = true;
}

void panic(const char *fmt, ...) {
    panic_begin();
    tfp_printf("\n*** kernel panic on cpu %u: ", this_cpu()->id);
    va_list va;
    va_start(va, fmt);
    tfp_vprintf((char *)fmt, va);
    va_end(va);
    tfp_printf(" ***\n");
    for (;;) {
        asm volatile("wfi");
    }
}
