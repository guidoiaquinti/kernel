// SPDX-License-Identifier: GPL-3.0-or-later
//
// libk mutex: a test-and-set lock on the compiler's atomics (load-acquire and
// store-exclusive loops on the Cortex-A53, which has no LSE atomics). A waiter
// spins briefly, which is enough when the holder runs on another core, then
// yield()s its core, in case the holder was preempted on this very core and
// can only finish once it gets the CPU back.

#include "koraos.h"

#define MUTEX_SPINS 100  // spin-wait rounds before giving the core away

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

int mutex_trylock(mutex_t *m) {
    return __atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE) == 0;
}

void mutex_lock(mutex_t *m) {
    unsigned spins = 0;
    while (!mutex_trylock(m)) {
        // Wait with plain loads until it looks free, then try again.
        while (__atomic_load_n(&m->locked, __ATOMIC_RELAXED)) {
            if (++spins < MUTEX_SPINS) {
                cpu_relax();
            } else {
                spins = 0;
                yield();
            }
        }
    }
}

void mutex_unlock(mutex_t *m) {
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
}
