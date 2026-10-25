// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fs/blkdev.h"

// The SD card slot, through the SoC's SDHCI controller (src/drivers/sdhci.c):
// the Arasan "EMMC" controller on the Pi 3, with the card's pins switched to it
// from the firmware's SDHOST controller. Polled, without DMA or interrupts:
// every transfer runs in the calling task under the BKL, on any core.
//
// Returns the card as a block device, or NULL with *present false if no card
// answers, or NULL with *present true if a card answered but could not be
// brought up. The device is read-only unless the kernel is built with
// KORAOS_SD_WRITABLE (./build.sh --sd-writable).
blkdev_t *sdhci_init(bool *present);
