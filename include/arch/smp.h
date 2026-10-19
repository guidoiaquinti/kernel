// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// Secondary core bring-up (src/arch/smp.c, docs/smp.md). Core 0 releases the
// other cores, PSCI CPU_ON on QEMU virt and the firmware spin table on the
// Raspberry Pi; each one turns on its MMU and caches with the shared page
// tables, installs the vectors, sets up its own interrupt controller
// interface and timer, and idles in WFI. Device interrupts and Circle stay on
// core 0.

// Start every other core and wait (up to a second each) for it to come
// online. Call on core 0 once the MMU, interrupt controller and system tick
// are up. Returns the number of cores online, core 0 included.
unsigned smp_start_secondaries(void);

// Cores online, core 0 included.
unsigned smp_cores_online(void);
