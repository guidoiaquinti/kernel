// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fs/blkdev.h"

// The embedded ramdisk (src/fs/ramdisk.c), in kernels built with
// ./build.sh --ramdisk SIZE (KORAOS_RAMDISK): lays the volume out in free RAM
// past the page pool and returns it as a read-write block device, or NULL
// (with a log line) if it does not fit or the image is damaged.
blkdev_t *ramdisk_init(void);
