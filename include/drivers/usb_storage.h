// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fs/blkdev.h"

#ifdef __cplusplus
extern "C" {
#endif

// USB mass storage drives (src/circle/usb_storage.cpp): each drive Circle's
// bulk-only driver configured while the USB host controller enumerated at boot,
// as a block device. Transfers run in the calling task under the BKL, on any
// core; their completion interrupts arrive on core 0. The drives are read-only
// unless the kernel is built with KORAOS_STORAGE_WRITABLE
// (./build.sh --storage-writable).

// Collect the configured drives (umsd1, umsd2, ...); called by
// circle_usb_init() after enumeration.
void usb_storage_collect(void);

unsigned usb_storage_count(void);
blkdev_t *usb_storage_device(unsigned index);

#ifdef __cplusplus
}
#endif
