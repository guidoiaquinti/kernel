// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB mass storage drives as KoraOS block devices, on Circle's bulk-only
// driver (CUSBBulkOnlyMassStorageDevice). Circle's partition manager also
// publishes the drive's partitions as Circle devices; KoraOS ignores those and
// probes the whole drive itself (src/fs/blkdev.c), like any other disk.
//
// Callers hold the BKL, so one task at a time uses a drive (Seek then Read is
// not atomic). Circle is built multi-core, so that task may run on any core,
// but every USB interrupt goes to core 0: a transfer opens IRQs on its own core
// while it waits, since syscalls run with them masked and a caller on core 0
// would otherwise wait for its own completion interrupt forever.

#include "drivers/usb_storage.h"
#include "drivers/readahead.h"

#include <circle/devicenameservice.h>
#include <circle/synchronize.h>
#include <circle/usb/usbmassdevice.h>
#include <circle/util.h>

#include "arch/irq.h"

extern "C" void tfp_printf(const char *fmt, ...);

namespace {

constexpr unsigned kMaxDrives = 4;
// Sectors per SCSI command. Circle hands the data stage to the controller in
// one piece, and the DWC2's packet count field is 10 bits: 64 KB on a
// full-speed drive (64-byte packets) would be 1024 packets, one more than it
// holds. 32 KB fits at any speed; it is also the largest bounce buffer Circle
// stages an unaligned transfer through.
constexpr uint32_t kChunkSectors = 64;
// The FAT32 layer reads one sector at a time, and every SCSI command costs a
// few USB frames (CBW, data, CSW), so reads go through a read-ahead window:
// one aligned 16 KB block per drive, refilled by a single command. A write
// that overlaps the window drops it.
constexpr uint32_t kWindowSectors = 32;

struct Drive {
    blkdev_t dev;
    CUSBBulkOnlyMassStorageDevice *umsd;
    readahead_t ra;
    // Whole cache lines, so Circle transfers into it without a bounce copy.
    alignas(DATA_CACHE_LINE_LENGTH_MAX) uint8_t window[kWindowSectors * BLK_SECTOR_SIZE];
};

Drive g_drives[kMaxDrives];
unsigned g_count;

int transfer(Drive *d, uint32_t lba, uint32_t count, void *buf, bool write) {
    CUSBBulkOnlyMassStorageDevice *umsd = d->umsd;
    uint8_t *p = static_cast<uint8_t *>(buf);
    int rc = 0;
    uint64_t flags = irq_save();
    irq_enable();
    while (count > 0) {
        uint32_t n = count < kChunkSectors ? count : kChunkSectors;
        int bytes = (int)(n * BLK_SECTOR_SIZE);
        umsd->Seek((u64)lba * BLK_SECTOR_SIZE);
        if ((write ? umsd->Write(p, bytes) : umsd->Read(p, bytes)) != bytes) {
            rc = BLK_ERR_IO;
            break;
        }
        lba += n;
        count -= n;
        p += bytes;
    }
    irq_restore(flags);
    return rc;
}

int drive_read(blkdev_t *dev, uint32_t lba, uint32_t count, void *buf) {
    Drive *d = static_cast<Drive *>(dev->ctx);
    if (count >= kWindowSectors) {
        return transfer(d, lba, count, buf, false);  // large enough on its own
    }
    uint8_t *p = static_cast<uint8_t *>(buf);
    for (; count > 0; lba++, count--, p += BLK_SECTOR_SIZE) {
        if (!readahead_holds(&d->ra, lba)) {
            d->ra.valid = false;
            d->ra.lba = readahead_window(lba, kWindowSectors, dev->sector_count, &d->ra.count);
            if (transfer(d, d->ra.lba, d->ra.count, d->window, false) != 0) {
                return BLK_ERR_IO;
            }
            d->ra.valid = true;
        }
        memcpy(p, d->window + (lba - d->ra.lba) * BLK_SECTOR_SIZE, BLK_SECTOR_SIZE);
    }
    return 0;
}

#ifdef KORAOS_STORAGE_WRITABLE
int drive_write(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf) {
    Drive *d = static_cast<Drive *>(dev->ctx);
    if (readahead_overlaps(&d->ra, lba, count)) {
        d->ra.valid = false;
    }
    return transfer(d, lba, count, const_cast<void *>(buf), true);
}

// Circle writes with FUA (force unit access): a completed write is already on
// the medium, so there is no drive cache to flush.
int drive_flush(blkdev_t *dev) {
    (void)dev;
    return 0;
}
#endif

}  // namespace

void usb_storage_collect(void) {
    for (unsigned number = 1; g_count < kMaxDrives; number++) {
        auto *umsd = static_cast<CUSBBulkOnlyMassStorageDevice *>(
            CDeviceNameService::Get()->GetDevice("umsd", number, TRUE));
        if (umsd == 0) {
            break;
        }
        Drive &d = g_drives[g_count++];
        d.umsd = umsd;
        d.ra.valid = false;
        d.dev = blkdev_t{};
        d.dev.read = drive_read;
        d.dev.sector_count = umsd->GetCapacity();
        d.dev.ctx = &d;
#ifdef KORAOS_STORAGE_WRITABLE
        d.dev.write = drive_write;
        d.dev.flush = drive_flush;
        d.dev.read_only = false;
#else
        d.dev.read_only = true;
#endif
        tfp_printf("[usb] drive umsd%u: %u MB, %s\n", number,
                   (unsigned)(d.dev.sector_count / 2048), d.dev.read_only ? "read-only" : "writable");
    }
}

unsigned usb_storage_count(void) {
    return g_count;
}

blkdev_t *usb_storage_device(unsigned index) {
    return index < g_count ? &g_drives[index].dev : 0;
}
