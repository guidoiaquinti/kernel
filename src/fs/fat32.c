#include "fs/fat32.h"
#include "fs/blkdev.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

// Each mounted view owns its geometry and scratch caches. Calls are serialized
// by the kernel; no task state or additional filesystem locks are required.
#define FAT32_CACHE_SECTORS 8u
#define FAT32_EOC 0x0fffffffu
#define FAT32_CLEAN 0x08000000u

struct fat32_inode {
    struct fat32_inode *next;
    uint32_t sector;
    uint16_t offset;
    uint8_t attributes;
    uint32_t first_cluster;
    uint32_t size;
    unsigned refs;
};

struct sector_cache {
    uint32_t lba;
    bool valid;
    bool dirty;
    uint8_t bytes[BLK_SECTOR_SIZE];
};

struct sector_undo {
    struct sector_undo *next;
    uint32_t lba;
    uint8_t bytes[BLK_SECTOR_SIZE];
};

struct namespace_undo {
    struct sector_undo *sectors;
    uint32_t free_count;
    uint32_t allocation_hint;
    bool free_count_known;
};

struct fat32_volume {
    blkdev_t *dev;
    uint32_t sec_per_clus;
    uint32_t fat_start;
    uint32_t fat_base;
    uint32_t fat_sectors;
    uint32_t fat_count;
    bool mirrored;
    uint32_t first_data_sector;
    uint32_t root_cluster;
    uint32_t cluster_count;
    uint32_t allocation_hint;
    uint32_t fsinfo_sector;
    uint32_t backup_fsinfo_sector;
    uint32_t free_count;
    bool free_count_known;
    bool dirty;
    bool dirty_persisted;
    bool fat_pending;
    uint32_t pending_cluster;
    uint32_t pending_value;
    uint32_t allocated_cluster;
    uint32_t allocated_previous;
    uint32_t reclaim_cluster;
    uint32_t reclaim_remaining;
    uint32_t reclaim_retained;
    uint32_t reclaim_next;
    bool reclaim_next_valid;
    bool reclaim_barrier;
    unsigned victim;
    struct fat32_inode *inodes;
    struct namespace_undo *undo;
    bool undo_active;
    bool undo_pending;
    struct sector_cache cache[FAT32_CACHE_SECTORS];
    char label[12];
    uint8_t data_buf[BLK_SECTOR_SIZE];
};

static int recover_pending(fat32_volume_t *vol);
static int rollback_namespace(fat32_volume_t *vol);
static int create_file(fat32_volume_t *vol, const char *path, uint32_t flags, fat32_file_t *out);

// --- small local helpers (freestanding: no libc) ---------------------------

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int io_error(int rc) {
    if (rc == BLK_ERR_RO) return FS_ERR_RO;
    if (rc == BLK_ERR_UNSUPPORTED) return FS_ERR_UNSUPPORTED;
    return rc == 0 ? 0 : FS_ERR_IO;
}

static int cache_get(fat32_volume_t *vol, uint32_t lba, struct sector_cache **out) {
    for (unsigned i = 0; i < FAT32_CACHE_SECTORS; i++) {
        if (vol->cache[i].valid && vol->cache[i].lba == lba) {
            *out = &vol->cache[i];
            return 0;
        }
    }
    struct sector_cache *slot = &vol->cache[vol->victim];
    for (unsigned i = 0; i < FAT32_CACHE_SECTORS; i++) {
        if (!vol->cache[i].valid) {
            slot = &vol->cache[i];
            break;
        }
    }
    if (slot->dirty) {
        int rc = io_error(blkdev_write(vol->dev, slot->lba, 1, slot->bytes));
        if (rc != 0) return rc; // retain the dirty victim on failure
        slot->dirty = false;
    }
    // A failed read must not destroy the previous cached sector.
    int rc = io_error(blkdev_read(vol->dev, lba, 1, vol->data_buf));
    if (rc != 0) return rc;
    memcpy(slot->bytes, vol->data_buf, BLK_SECTOR_SIZE);
    slot->lba = lba;
    slot->valid = true;
    vol->victim = ((unsigned)(slot - vol->cache) + 1) % FAT32_CACHE_SECTORS;
    *out = slot;
    return 0;
}

static int read_sector(fat32_volume_t *vol, uint32_t lba, uint8_t *buf) {
    struct sector_cache *slot;
    int rc = cache_get(vol, lba, &slot);
    if (rc == 0) memcpy(buf, slot->bytes, BLK_SECTOR_SIZE);
    return rc;
}

static int change_sector(fat32_volume_t *vol, uint32_t lba, struct sector_cache **out) {
    int rc = cache_get(vol, lba, out);
    if (rc != 0 || !vol->undo_active) return rc;
    for (struct sector_undo *s = vol->undo->sectors; s; s = s->next) {
        if (s->lba == lba) return 0;
    }
    struct sector_undo *snapshot = kmalloc(sizeof(*snapshot));
    if (snapshot == NULL) return FS_ERR_IO;
    snapshot->lba = lba;
    memcpy(snapshot->bytes, (*out)->bytes, BLK_SECTOR_SIZE);
    snapshot->next = vol->undo->sectors;
    vol->undo->sectors = snapshot;
    return 0;
}

static void wr16(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

static void wr32(uint8_t *p, uint32_t n) {
    wr16(p, n);
    wr16(p + 2, n >> 16);
}

static int cache_flush(fat32_volume_t *vol) {
    for (unsigned i = 0; i < FAT32_CACHE_SECTORS; i++) {
        struct sector_cache *slot = &vol->cache[i];
        if (slot->dirty) {
            int rc = io_error(blkdev_write(vol->dev, slot->lba, 1, slot->bytes));
            if (rc != 0) return rc;
            slot->dirty = false;
        }
    }
    return io_error(blkdev_flush(vol->dev));
}

// A data cluster number is valid if it names a real cluster and is not an
// end-of-chain / bad marker.
static bool cluster_valid(fat32_volume_t *vol, uint32_t c) {
    return c >= 2 && c < 0x0ffffff0u && c < vol->cluster_count + 2;
}

static uint32_t cluster_to_lba(fat32_volume_t *vol, uint32_t cluster) {
    return vol->first_data_sector + (cluster - 2) * vol->sec_per_clus;
}

// Follow one FAT link. Preserve I/O failure separately from EOC markers.
static int fat_next(fat32_volume_t *vol, uint32_t cluster, uint32_t *next) {
    if (vol->fat_pending && vol->pending_cluster == cluster) {
        *next = vol->pending_value & 0x0fffffffu;
        return 0;
    }
    uint32_t fat_off = cluster * 4;
    uint32_t sec = vol->fat_start + fat_off / BLK_SECTOR_SIZE;
    uint32_t off = fat_off % BLK_SECTOR_SIZE;
    struct sector_cache *slot;
    int rc = cache_get(vol, sec, &slot);
    if (rc != 0) return rc;
    *next = rd32(slot->bytes + off) & 0x0FFFFFFFu;
    return 0;
}

// Floyd's detector bounds corrupt cyclic chains promptly, including self loops.
// Its lookahead reads FAT entries only and preserves transport error codes.
static int chain_advance(fat32_volume_t *vol, uint32_t *cluster,
                         uint32_t *slow, uint32_t *fast) {
    int rc = fat_next(vol, *cluster, cluster);
    if (rc != 0) {
        return rc;
    }
    if (*cluster >= 0x0ffffff8u) {
        return 0;
    }
    if (!cluster_valid(vol, *cluster)) {
        return FS_ERR_CORRUPT;
    }
    if (cluster_valid(vol, *slow)) {
        rc = fat_next(vol, *slow, slow);
        if (rc != 0) {
            return rc;
        }
    }
    for (unsigned i = 0; i < 2 && cluster_valid(vol, *fast); i++) {
        rc = fat_next(vol, *fast, fast);
        if (rc != 0) {
            return rc;
        }
    }
    if (cluster_valid(vol, *fast) && *slow == *fast) {
        return FS_ERR_CORRUPT;
    }
    return 0;
}

// Read a byte range from a bounded chain, preserving corruption and I/O errors.
static long chain_read(fat32_volume_t *vol, uint32_t first_cluster, uint32_t pos,
                       void *buf, uint32_t len) {
    if (len == 0) {
        return 0;
    }
    uint32_t cluster_bytes = vol->sec_per_clus * BLK_SECTOR_SIZE;
    uint32_t cluster = first_cluster;
    uint32_t slow = cluster, fast = cluster;
    uint32_t traversed = 0;
    uint32_t skip = pos / cluster_bytes;
    if (skip >= vol->cluster_count) {
        return FS_ERR_CORRUPT;
    }
    while (skip--) {
        if (!cluster_valid(vol, cluster) || ++traversed > vol->cluster_count) {
            return FS_ERR_CORRUPT;
        }
        int rc = chain_advance(vol, &cluster, &slow, &fast);
        if (rc != 0) {
            return rc;
        }
        if (cluster >= 0x0ffffff8u) {
            return 0;
        }
    }
    uint32_t off = pos % cluster_bytes;
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < len) {
        if (!cluster_valid(vol, cluster) || ++traversed > vol->cluster_count) {
            return FS_ERR_CORRUPT;
        }
        uint32_t lba = cluster_to_lba(vol, cluster);
        while (off < cluster_bytes && done < len) {
            uint32_t si = off / BLK_SECTOR_SIZE;
            uint32_t so = off % BLK_SECTOR_SIZE;
            if (read_sector(vol, lba + si, vol->data_buf) != 0) {
                return FS_ERR_IO;
            }
            uint32_t chunk = BLK_SECTOR_SIZE - so;
            if (chunk > len - done) {
                chunk = len - done;
            }
            memcpy(out + done, vol->data_buf + so, chunk);
            done += chunk;
            off += chunk;
        }
        if (done == len) {
            break;
        }
        int rc = chain_advance(vol, &cluster, &slow, &fast);
        if (rc != 0) {
            return rc;
        }
        if (cluster >= 0x0ffffff8u) {
            break;
        }
        off = 0;
    }
    return (long)done;
}

// --- long filename (VFAT) decoding -----------------------------------------

// An LFN holds at most 20 entries of 13 UTF-16 code units.
#define FAT32_LFN_MAX_UNITS 260

// Byte offsets of the 13 UTF-16 code units within a 32-byte LFN entry.
static const uint8_t lfn_off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static uint8_t short_checksum(const uint8_t *ent) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + ent[i]);
    }
    return sum;
}

static bool lfn_complete(const uint16_t *units, uint32_t n) {
    bool terminated = false;
    for (uint32_t i = 0; i < n; i++) {
        if (terminated) {
            if (units[i] != 0xffffu) {
                return false;
            }
        } else if (units[i] == 0) {
            terminated = true;
        } else if (i >= 255 || units[i] == 0xffffu) {
            return false;
        }
    }
    return n > 0 && units[0] != 0;
}

// Decode `n` little-endian UTF-16 code units (possibly with surrogate pairs) to
// a NUL-terminated UTF-8 string in `out` (capacity `cap`, including the NUL).
// Stops at a 0x0000 unit (the LFN terminator) or when the buffer is full. Lone
// surrogates decode to U+FFFD.
static void utf16_to_utf8(const uint16_t *units, uint32_t n, char *out,
                          uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cp = units[i];
        if (cp == 0x0000) {
            break;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate
            uint32_t lo = (i + 1 < n) ? units[i + 1] : 0;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else {
                cp = 0xFFFD;  // unpaired high surrogate
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // unpaired low surrogate
        }

        uint32_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (o + need + 1 > cap) {
            break;  // no room for this char plus the NUL
        }
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
}

// Map one 8.3 short-name byte: lower-case ASCII, and replace any non-ASCII byte
// with '?'. Short names are stored in an OEM code page, not Unicode; decoding
// that is out of scope, and a raw >= 0x80 byte would be invalid UTF-8. Names
// that actually need non-ASCII characters carry an LFN, which we decode fully.
static char short_byte(uint8_t b) {
    if (b >= 0x80) {
        return '?';
    }
    return to_lower((char)b);
}

// Build a name from an 8.3 short-directory entry: "NAME    EXT" -> "name.ext",
// spaces trimmed, lower-cased.
static void name_from_83(const uint8_t *ent, char *out) {
    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (ent[i] == ' ') {
            break;
        }
        out[o++] = short_byte(ent[i]);
    }
    if (ent[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11; i++) {
            if (ent[i] == ' ') {
                break;
            }
            out[o++] = short_byte(ent[i]);
        }
    }
    out[o] = 0;
}

// --- public API -------------------------------------------------------------

static void copy_label(char *out, const uint8_t *label) {
    unsigned end = 11;
    while (end && label[end - 1] == ' ') {
        end--;
    }
    for (unsigned i = 0; i < end; i++) {
        out[i] = short_byte(label[i]);
    }
    out[end] = 0;
    if (strcmp(out, "no name") == 0) {
        out[0] = 0;
    }
}

// Formatters put the label at the start of the root directory. Mounting runs
// at boot on attached media, so walk the chain once and only this far; past it
// the BPB label stands.
#define FAT32_LABEL_CLUSTERS 100u

static int find_root_label(fat32_volume_t *vol) {
    uint32_t cluster = vol->root_cluster;
    uint32_t slow = cluster, fast = cluster;
    for (uint32_t scanned = 0; scanned < FAT32_LABEL_CLUSTERS; scanned++) {
        if (scanned > 0) {
            int rc = chain_advance(vol, &cluster, &slow, &fast);
            if (rc != 0) {
                return rc;
            }
            if (cluster >= 0x0ffffff8u) {
                return 0;
            }
        }
        uint32_t lba = cluster_to_lba(vol, cluster);
        for (uint32_t s = 0; s < vol->sec_per_clus; s++) {
            struct sector_cache *slot;
            if (cache_get(vol, lba + s, &slot) != 0) {
                return FS_ERR_IO;
            }
            for (uint32_t off = 0; off < BLK_SECTOR_SIZE; off += 32) {
                const uint8_t *ent = slot->bytes + off;
                if (ent[0] == 0) {
                    return 0;
                }
                if (ent[0] != 0xe5 && ent[11] == 0x08) {
                    copy_label(vol->label, ent);
                    return 0;
                }
            }
        }
    }
    return 0;
}

int fat32_mount(blkdev_t *dev, fat32_volume_t **out) {
    if (out == NULL) {
        return FS_ERR_INVAL;
    }
    *out = NULL;
    if (dev == NULL) {
        return FS_ERR_INVAL;
    }
    uint8_t sec[BLK_SECTOR_SIZE];
    if (blkdev_read(dev, 0, 1, sec) != 0) {
        return FS_ERR_IO;
    }
    uint32_t spc = sec[13], rsvd = rd16(sec + 14), nfats = sec[16];
    uint32_t fatsz = rd32(sec + 36), root = rd32(sec + 44);
    uint32_t total = rd16(sec + 19);
    if (total == 0) {
        total = rd32(sec + 32);
    }
    uint64_t data = (uint64_t)rsvd + (uint64_t)nfats * fatsz;
    uint32_t flags = rd16(sec + 40);
    uint32_t active = (flags & 0x80) ? flags & 0x0f : 0;
    if (sec[510] != 0x55 || sec[511] != 0xaa || rd16(sec + 11) != 512 ||
        !spc || (spc & (spc - 1)) || spc > 128 || !rsvd || !nfats ||
        !fatsz || rd16(sec + 17) || rd16(sec + 22) || rd16(sec + 42) ||
        active >= nfats || total > dev->sector_count || data >= total) {
        return FS_ERR_NOFS;
    }
    uint32_t clusters = (total - (uint32_t)data) / spc;
    if (!clusters || clusters > 0x0fffffeeu ||
        (uint64_t)fatsz * 128 < (uint64_t)clusters + 2 ||
        root < 2 || root >= clusters + 2) {
        return FS_ERR_NOFS;
    }
    fat32_volume_t *vol = kmalloc(sizeof(*vol));
    if (vol == NULL) {
        return FS_ERR_IO;
    }
    vol->dev = dev;
    vol->sec_per_clus = spc;
    vol->fat_start = rsvd + active * fatsz;
    vol->first_data_sector = (uint32_t)data;
    vol->root_cluster = root;
    vol->cluster_count = clusters;
    vol->fat_base = rsvd;
    vol->fat_sectors = fatsz;
    vol->fat_count = nfats;
    vol->mirrored = !(flags & 0x80);
    vol->allocation_hint = 2;
    vol->fsinfo_sector = rd16(sec + 48);
    vol->backup_fsinfo_sector = rd16(sec + 50) + vol->fsinfo_sector;
    if (vol->fsinfo_sector == 0 || vol->fsinfo_sector >= rsvd) {
        vol->fsinfo_sector = 0;
        vol->backup_fsinfo_sector = 0;
    } else if (rd16(sec + 50) == 0 || rd16(sec + 50) == 0xffffu ||
               vol->backup_fsinfo_sector >= rsvd) {
        vol->backup_fsinfo_sector = 0;
    }
    if (vol->fsinfo_sector) {
        uint8_t info[BLK_SECTOR_SIZE];
        if (read_sector(vol, vol->fsinfo_sector, info) != 0) {
            kfree(vol);
            return FS_ERR_IO;
        }
        if (rd32(info) == 0x41615252u && rd32(info + 484) == 0x61417272u &&
            rd32(info + 508) == 0xaa550000u && cluster_valid(vol, rd32(info + 492))) {
            vol->allocation_hint = rd32(info + 492);
        }
    }
    copy_label(vol->label, sec + 71);
    // Root-directory label is authoritative; the BPB copy can be stale.
    int rc = find_root_label(vol);
    if (rc != 0) {
        kfree(vol);
        return rc;
    }
    *out = vol;
    return 0;
}

int fat32_unmount(fat32_volume_t *volume) {
    if (volume == NULL) return FS_ERR_INVAL;
    if (volume->inodes != NULL) return FS_ERR_BUSY;
    if (volume->dirty) {
        int rc = fat32_sync_volume(volume);
        if (rc != 0) return rc;
    }
    kfree(volume);
    return 0;
}

const char *fat32_label(const fat32_volume_t *volume) {
    return volume->label;
}

bool fat32_is_read_only(const fat32_volume_t *volume) {
    return volume->dev->read_only;
}

int fat32_readdir(fat32_file_t *dir, fat32_dirent_t *out) {
    if (dir == NULL || out == NULL || dir->volume == NULL) {
        return FS_ERR_INVAL;
    }
    if (!dir->is_dir) {
        return FS_ERR_NOTDIR;
    }
    if (dir->volume->undo_pending) {
        int rc = recover_pending(dir->volume);
        if (rc != 0) return rc;
    }
    uint8_t ent[32];
    uint16_t units[FAT32_LFN_MAX_UNITS] = {0};
    uint32_t nunits = 0;
    uint32_t expected = 0;
    uint8_t checksum = 0;
    bool have_lfn = false;

    for (;;) {
        if (dir->pos > 0xffffffffu - sizeof(ent)) return FS_ERR_CORRUPT;
        long n = chain_read(dir->volume, dir->first_cluster, dir->pos, ent, sizeof(ent));
        if (n < 0) {
            return (int)n;
        }
        if (n < (long)sizeof(ent)) {
            return 0;  // ran off the end of the directory's cluster chain
        }
        dir->pos += sizeof(ent);

        uint8_t first = ent[0];
        if (first == 0x00) {
            return 0;  // no more entries
        }
        if (first == 0xE5) {
            have_lfn = false;  // deleted
            nunits = 0;
            continue;
        }
        uint8_t attr = ent[11];
        if (attr == 0x0f) {
            unsigned ord = ent[0] & 0x1f;
            if (ent[0] & 0x40) {
                memset(units, 0, sizeof(units));
                expected = ord;
                nunits = ord * 13;
                checksum = ent[13];
                have_lfn = ord > 0 && ord <= 20;
            }
            if (!have_lfn || ord == 0 || ord > 20 || ord != expected || ent[13] != checksum ||
                (ent[0] & 0xa0) || ent[12] || rd16(ent + 26)) {
                have_lfn = false;
            } else {
                for (unsigned k = 0; k < 13; k++) {
                    units[(ord - 1) * 13 + k] = (uint16_t)rd16(ent + lfn_off[k]);
                }
                expected--;
            }
            continue;
        }
        if (attr & 0x08) {
            have_lfn = false;  // volume label
            nunits = 0;
            continue;
        }
        if (first == '.') {
            have_lfn = false;  // "." or ".."
            nunits = 0;
            continue;
        }

        if (have_lfn && expected == 0 && short_checksum(ent) == checksum &&
            lfn_complete(units, nunits)) {
            utf16_to_utf8(units, nunits, out->name, FAT32_NAME_MAX + 1);
        } else {
            name_from_83(ent, out->name);
        }
        uint32_t cluster = dir->first_cluster;
        uint32_t skip = (dir->pos - 32) / (dir->volume->sec_per_clus * BLK_SECTOR_SIZE);
        while (skip--) {
            int rc = fat_next(dir->volume, cluster, &cluster);
            if (rc != 0) return rc;
            if (!cluster_valid(dir->volume, cluster)) return FS_ERR_CORRUPT;
        }
        out->entry_sector = cluster_to_lba(dir->volume, cluster) +
                            ((dir->pos - 32) / BLK_SECTOR_SIZE) % dir->volume->sec_per_clus;
        out->entry_offset = (uint16_t)((dir->pos - 32) % BLK_SECTOR_SIZE);
        out->entry_index = (dir->pos - 32) / 32;
        out->parent_cluster = dir->first_cluster;
        out->lfn_slots = have_lfn && expected == 0 && short_checksum(ent) == checksum &&
                         lfn_complete(units, nunits) ? (uint8_t)(nunits / 13) : 0;
        name_from_83(ent, out->short_name);
        out->attributes = attr;
        out->first_cluster = (rd16(ent + 20) << 16) | rd16(ent + 26);
        out->size = rd32(ent + 28);
        out->is_dir = (attr & 0x10) != 0;
        return 1;
    }
}

// Case-insensitive compare of a NUL-terminated name against a length-delimited
// component.
static bool name_eq(const char *name, const char *comp, size_t comp_len) {
    size_t i = 0;
    for (; i < comp_len; i++) {
        if (name[i] == 0 || to_lower(name[i]) != to_lower(comp[i])) {
            return false;
        }
    }
    return name[i] == 0;
}

static int find_in_dir(fat32_volume_t *vol, uint32_t dir_cluster, const char *comp, size_t comp_len,
                       fat32_dirent_t *out) {
    fat32_file_t dir = {.volume = vol, .first_cluster = dir_cluster, .size = 0, .pos = 0,
                        .is_dir = true};
    int r;
    while ((r = fat32_readdir(&dir, out)) == 1) {
        if (name_eq(out->name, comp, comp_len) || name_eq(out->short_name, comp, comp_len)) return 0;
    }
    return r < 0 ? r : FS_ERR_NOTFOUND;
}

// Resolve an absolute path to its directory entry. "/" resolves to a synthetic
// root-directory entry.
struct lookup_scratch {
    fat32_dirent_t current;
    fat32_dirent_t found;
};

int fat32_lookup(fat32_volume_t *vol, const char *path, fat32_dirent_t *out) {
    if (vol == NULL) return FS_ERR_NOFS;
    if (vol->undo_pending) {
        int rc = recover_pending(vol);
        if (rc != 0) return rc;
    }
    if (path == NULL || out == NULL || path[0] != '/') return FS_ERR_INVAL;
    // UTF-8 directory entries are large. Keep both traversal entries off the
    // per-task kernel stack, including unoptimized Debug builds.
    struct lookup_scratch *scratch = kmalloc(sizeof(*scratch));
    if (scratch == NULL) return FS_ERR_IO;
    scratch->current.name[0] = '/';
    scratch->current.first_cluster = vol->root_cluster;
    scratch->current.is_dir = true;
    scratch->current.attributes = 0x10;
    scratch->current.entry_sector = 0xffffffffu;
    const char *p = path + 1;
    int rc = 0;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len > 0) {
            if (!scratch->current.is_dir) {
                rc = FS_ERR_NOTDIR;
                break;
            }
            rc = find_in_dir(vol, scratch->current.first_cluster, start, len, &scratch->found);
            if (rc != 0) break;
            scratch->current = scratch->found;
        }
        if (*p == '/') p++;
    }
    if (rc == 0) *out = scratch->current;
    kfree(scratch);
    return rc;
}

static int handle_open(fat32_volume_t *vol, const fat32_dirent_t *de,
                       uint32_t flags, fat32_file_t *out) {
    fat32_inode_t *inode = vol->inodes;
    while (inode && (inode->sector != de->entry_sector || inode->offset != de->entry_offset)) {
        inode = inode->next;
    }
    if (inode == NULL) {
        inode = kmalloc(sizeof(*inode));
        if (inode == NULL) return FS_ERR_IO;
        inode->sector = de->entry_sector;
        inode->offset = de->entry_offset;
        inode->attributes = de->attributes;
        inode->first_cluster = de->first_cluster;
        inode->size = de->size;
        inode->next = vol->inodes;
        vol->inodes = inode;
    }
    inode->refs++;
    *out = (fat32_file_t){.volume = vol, .inode = inode, .flags = flags,
                         .first_cluster = inode->first_cluster, .size = inode->size,
                         .is_dir = de->is_dir};
    return 0;
}

int fat32_open_flags(fat32_volume_t *vol, const char *path, uint32_t flags, fat32_file_t *out) {
    if (out == NULL || (flags & ~(3u | FAT32_O_CREAT | FAT32_O_EXCL | FAT32_O_TRUNC | FAT32_O_APPEND)) ||
        (flags & 3u) == 3u || ((flags & (FAT32_O_TRUNC | FAT32_O_APPEND)) &&
                              !(flags & 3u)) || ((flags & FAT32_O_EXCL) && !(flags & FAT32_O_CREAT))) return FS_ERR_INVAL;
    fat32_dirent_t de;
    int rc = fat32_lookup(vol, path, &de);
    if (rc == FS_ERR_NOTFOUND && (flags & FAT32_O_CREAT)) return create_file(vol, path, flags, out);
    if (rc != 0) return rc;
    if (flags & FAT32_O_EXCL) return FS_ERR_EXISTS;
    if (de.is_dir) return FS_ERR_ISDIR;
    if ((flags & 3u) && (vol->dev->read_only || (de.attributes & 1))) return FS_ERR_RO;
    if ((flags & 3u) && (vol->dev->write == NULL || vol->dev->flush == NULL)) {
        return FS_ERR_UNSUPPORTED;
    }
    rc = handle_open(vol, &de, flags, out);
    if (rc == 0 && (flags & FAT32_O_TRUNC)) {
        rc = fat32_truncate(out, 0);
        if (rc != 0) fat32_close(out);
    }
    return rc;
}

int fat32_open(fat32_volume_t *vol, const char *path, fat32_file_t *out) {
    return fat32_open_flags(vol, path, FAT32_O_RDONLY, out);
}

int fat32_opendir(fat32_volume_t *vol, const char *path, fat32_file_t *out) {
    if (out == NULL) return FS_ERR_INVAL;
    fat32_dirent_t de;
    int rc = fat32_lookup(vol, path, &de);
    if (rc != 0) return rc;
    if (!de.is_dir) return FS_ERR_NOTFILE;
    return handle_open(vol, &de, FAT32_O_RDONLY, out);
}

void fat32_close(fat32_file_t *file) {
    if (file == NULL || file->volume == NULL || file->inode == NULL) return;
    fat32_inode_t *inode = file->inode;
    if (--inode->refs == 0) {
        fat32_inode_t **link = &file->volume->inodes;
        while (*link && *link != inode) link = &(*link)->next;
        if (*link == inode) *link = inode->next;
        kfree(inode);
    }
    memset(file, 0, sizeof(*file));
}

uint32_t fat32_size(const fat32_file_t *file) {
    return file->inode ? file->inode->size : file->size;
}

static void refresh_handle(fat32_file_t *file) {
    if (file->inode) {
        file->size = file->inode->size;
        file->first_cluster = file->inode->first_cluster;
    }
}

long fat32_seek(fat32_file_t *file, long offset, int whence) {
    if (file == NULL || file->volume == NULL || file->is_dir) return FS_ERR_INVAL;
    refresh_handle(file);
    int64_t base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = file->pos; break;
    case 2: base = file->size; break;
    default: return FS_ERR_INVAL;
    }
    if (offset < -base || offset > (int64_t)file->size - base) return FS_ERR_INVAL;
    file->pos = (uint32_t)(base + offset);
    return file->pos;
}

unsigned fat32_handle_count(const fat32_volume_t *vol) {
    unsigned count = 0;
    for (const fat32_inode_t *inode = vol->inodes; inode; inode = inode->next) count += inode->refs;
    return count;
}

bool fat32_entry_busy(fat32_volume_t *vol, uint32_t sector, uint16_t offset) {
    for (fat32_inode_t *inode = vol->inodes; inode; inode = inode->next) {
        if (inode->sector == sector && inode->offset == offset) return true;
    }
    return false;
}

bool fat32_directory_busy(fat32_volume_t *vol, uint32_t first_cluster) {
    for (fat32_inode_t *inode = vol->inodes; inode; inode = inode->next) {
        if ((inode->attributes & 0x10) && inode->first_cluster == first_cluster) return true;
    }
    return false;
}

long fat32_read(fat32_file_t *f, void *buf, uint32_t len) {
    if (f == NULL || f->volume == NULL || (len && buf == NULL)) {
        return FS_ERR_INVAL;
    }
    if (f->is_dir) {
        return FS_ERR_ISDIR;
    }
    if ((f->flags & 3u) == FAT32_O_WRONLY) return FS_ERR_INVAL;
    if (f->volume->undo_pending) {
        int rc = recover_pending(f->volume);
        if (rc != 0) return rc;
    }
    refresh_handle(f);
    if (f->pos >= f->size) {
        return 0;
    }
    uint32_t remaining = f->size - f->pos;
    if (len > remaining) {
        len = remaining;
    }
    long n = chain_read(f->volume, f->first_cluster, f->pos, buf, len);
    if (n < 0) {
        return n;
    }
    if ((uint32_t)n != len) {
        return FS_ERR_CORRUPT;
    }
    f->pos += (uint32_t)n;
    return n;
}

int fat32_stat(fat32_volume_t *vol, const char *path, fat32_stat_t *out) {
    if (out == NULL) {
        return FS_ERR_INVAL;
    }
    fat32_dirent_t de;
    int r = fat32_lookup(vol, path, &de);
    if (r != 0) {
        return r;
    }
    out->size = de.size;
    out->is_dir = de.is_dir;
    return 0;
}

// FAT entry writes preserve the four reserved high bits. ExtFlags selects the
// active FAT when mirroring is disabled, otherwise every copy is updated.
static int fat_store_copies(fat32_volume_t *vol, uint32_t cluster, uint32_t value) {
    uint32_t first = vol->mirrored ? vol->fat_base : vol->fat_start;
    uint32_t copies = vol->mirrored ? vol->fat_count : 1;
    for (uint32_t i = 0; i < copies; i++) {
        struct sector_cache *slot;
        int rc = change_sector(vol, first + i * vol->fat_sectors + cluster / 128, &slot);
        if (rc != 0) return rc;
        uint8_t *entry = slot->bytes + (cluster % 128) * 4;
        wr32(entry, (rd32(entry) & 0xf0000000u) | (value & 0x0fffffffu));
        slot->dirty = true;
    }
    return 0;
}

// Retain the intended mirrored value if cache eviction or a copy read fails.
// Retrying later updates every copy, including copies evicted before failure.
static int finish_fat_update(fat32_volume_t *vol) {
    if (!vol->fat_pending) return 0;
    int rc = fat_store_copies(vol, vol->pending_cluster, vol->pending_value);
    if (rc == 0) vol->fat_pending = false;
    return rc;
}

static int fat_set(fat32_volume_t *vol, uint32_t cluster, uint32_t value) {
    if (vol->fat_pending && vol->pending_cluster == cluster) {
        vol->pending_value = value; // a retry/rollback can supersede this intent
        return finish_fat_update(vol);
    }
    int rc = finish_fat_update(vol);
    if (rc != 0) return rc;
    vol->pending_cluster = cluster;
    vol->pending_value = value;
    vol->fat_pending = true;
    return finish_fat_update(vol);
}

// An interrupted extension may have an allocated but unpublished cluster; an
// interrupted shrink may have a detached tail. Keep both recoverable until a
// later operation or sync completes, without relying on a task or open handle.
static int recover_pending(fat32_volume_t *vol) {
    if (vol->undo_pending) return rollback_namespace(vol);
    if (vol->undo_active) return 0;
    int rc = finish_fat_update(vol);
    if (rc != 0) return rc;
    if (vol->allocated_cluster) {
        if (vol->allocated_previous) {
            rc = fat_set(vol, vol->allocated_previous, FAT32_EOC);
            if (rc != 0) return rc;
        }
        rc = fat_set(vol, vol->allocated_cluster, 0);
        if (rc != 0) return rc;
        if (vol->allocated_cluster < vol->allocation_hint) {
            vol->allocation_hint = vol->allocated_cluster;
        }
        vol->allocated_cluster = 0;
        vol->free_count_known = false;
    }
    if (vol->reclaim_barrier) {
        if (vol->reclaim_retained) {
            rc = fat_set(vol, vol->reclaim_retained, FAT32_EOC);
            if (rc != 0) return rc;
        }
        rc = cache_flush(vol);
        if (rc != 0) return rc;
        vol->reclaim_barrier = false;
    }
    while (vol->reclaim_remaining) {
        if (!vol->reclaim_next_valid) {
            rc = fat_next(vol, vol->reclaim_cluster, &vol->reclaim_next);
            if (rc != 0) return rc;
            vol->reclaim_next_valid = true;
        }
        rc = fat_set(vol, vol->reclaim_cluster, 0);
        if (rc != 0) return rc;
        if (vol->free_count_known) vol->free_count++;
        if (vol->reclaim_cluster < vol->allocation_hint) {
            vol->allocation_hint = vol->reclaim_cluster;
        }
        vol->reclaim_cluster = vol->reclaim_next;
        vol->reclaim_remaining--;
        vol->reclaim_next_valid = false;
    }
    return 0;
}

static int begin_mutation(fat32_volume_t *vol) {
    if (vol->dev->read_only) return FS_ERR_RO;
    if (vol->dev->write == NULL || vol->dev->flush == NULL) return FS_ERR_UNSUPPORTED;
    int recover_rc = recover_pending(vol);
    if (recover_rc != 0) return recover_rc;
    if (vol->dirty_persisted) return 0;
    // Detect transports with an unimplemented flush before touching media.
    int rc = io_error(blkdev_flush(vol->dev));
    if (rc != 0) return rc;
    uint32_t status;
    rc = fat_next(vol, 1, &status);
    if (rc != 0) return rc;
    vol->dirty = true;
    rc = fat_set(vol, 1, status & ~FAT32_CLEAN);
    if (rc != 0) return rc;
    rc = cache_flush(vol);
    if (rc == 0) vol->dirty_persisted = true;
    return rc;
}

static int update_fsinfo(fat32_volume_t *vol) {
    uint32_t sectors[2] = {vol->fsinfo_sector, vol->backup_fsinfo_sector};
    for (unsigned i = 0; i < 2; i++) {
        if (sectors[i] == 0) continue;
        struct sector_cache *slot;
        int rc = change_sector(vol, sectors[i], &slot);
        if (rc != 0) return rc;
        uint8_t *info = slot->bytes;
        if (rd32(info) != 0x41615252u || rd32(info + 484) != 0x61417272u ||
            rd32(info + 508) != 0xaa550000u) continue;
        wr32(info + 488, vol->free_count_known ? vol->free_count : 0xffffffffu);
        wr32(info + 492, vol->allocation_hint);
        slot->dirty = true;
    }
    return 0;
}

int fat32_sync_volume(fat32_volume_t *vol) {
    if (vol == NULL) return FS_ERR_INVAL;
    if (!vol->dirty) return 0;
    int rc = begin_mutation(vol);
    if (rc != 0) return rc;
    rc = update_fsinfo(vol);
    if (rc != 0) return rc;
    rc = cache_flush(vol);
    if (rc != 0) return rc;
    uint32_t status;
    rc = fat_next(vol, 1, &status);
    if (rc != 0) return rc;
    rc = fat_set(vol, 1, status | FAT32_CLEAN);
    if (rc == 0) rc = cache_flush(vol);
    if (rc != 0) {
        // Never leave a cached clean bit behind after a failed final flush.
        // Best-effort persistence also covers a flush failure after its write.
        vol->dirty_persisted = false;
        (void)fat_set(vol, 1, status & ~FAT32_CLEAN);
        (void)cache_flush(vol);
        return rc;
    }
    vol->dirty = false;
    vol->dirty_persisted = false;
    return 0;
}

// Validate the entire chain before any operation can detach/free clusters.
// A zero first cluster is legal only for a zero-length regular file.
static int validate_chain(fat32_volume_t *vol, uint32_t first, uint32_t size,
                          uint32_t *count, uint32_t *last) {
    *count = 0;
    *last = 0;
    if (first == 0) return size == 0 ? 0 : FS_ERR_CORRUPT;
    uint32_t cluster = first, slow = first, fast = first;
    while (cluster_valid(vol, cluster)) {
        if (++*count > vol->cluster_count) return FS_ERR_CORRUPT;
        *last = cluster;
        int rc = chain_advance(vol, &cluster, &slow, &fast);
        if (rc != 0) return rc;
        if (cluster >= 0x0ffffff8u) {
            uint64_t capacity = (uint64_t)*count * vol->sec_per_clus * BLK_SECTOR_SIZE;
            return size <= capacity ? 0 : FS_ERR_CORRUPT;
        }
    }
    return FS_ERR_CORRUPT;
}

static int cluster_at(fat32_volume_t *vol, uint32_t first, uint32_t index, uint32_t *out) {
    uint32_t cluster = first;
    if (index >= vol->cluster_count) return FS_ERR_CORRUPT;
    while (index--) {
        if (!cluster_valid(vol, cluster)) return FS_ERR_CORRUPT;
        int rc = fat_next(vol, cluster, &cluster);
        if (rc != 0) return rc;
    }
    if (!cluster_valid(vol, cluster)) return FS_ERR_CORRUPT;
    *out = cluster;
    return 0;
}

static int allocate_cluster(fat32_volume_t *vol, uint32_t *out) {
    // Count actual FAT entries once. Neither FSInfo count nor hint is proof
    // that any cluster is free, even if it carries valid signatures.
    if (!vol->free_count_known) {
        uint32_t free_count = 0;
        for (uint32_t c = 2; c < vol->cluster_count + 2; c++) {
            uint32_t value;
            int rc = fat_next(vol, c, &value);
            if (rc != 0) return rc;
            if (value == 0) free_count++;
        }
        vol->free_count = free_count;
        vol->free_count_known = true;
    }
    if (vol->free_count == 0) return FS_ERR_NOSPC;
    uint32_t c = vol->allocation_hint;
    for (uint32_t scanned = 0; scanned < vol->cluster_count; scanned++) {
        uint32_t value;
        int rc = fat_next(vol, c, &value);
        if (rc != 0) return rc;
        if (value == 0) {
            // Zero every sector, then persist it before publishing a FAT link.
            for (uint32_t s = 0; s < vol->sec_per_clus; s++) {
                struct sector_cache *slot;
                rc = change_sector(vol, cluster_to_lba(vol, c) + s, &slot);
                if (rc != 0) return rc;
                memset(slot->bytes, 0, BLK_SECTOR_SIZE);
                slot->dirty = true;
            }
            rc = cache_flush(vol);
            if (rc != 0) return rc;
            vol->allocated_cluster = c;
            rc = fat_set(vol, c, FAT32_EOC);
            if (rc != 0) return rc;
            vol->free_count--;
            vol->allocation_hint = c + 1 < vol->cluster_count + 2 ? c + 1 : 2;
            *out = c;
            return 0;
        }
        c = c + 1 < vol->cluster_count + 2 ? c + 1 : 2;
    }
    // Concurrent filesystem execution is serialized. Reaching here means the
    // advisory count became unreliable after an I/O error or external change.
    vol->free_count_known = false;
    return FS_ERR_NOSPC;
}

static int inode_store(fat32_volume_t *vol, fat32_inode_t *inode,
                       uint32_t first, uint32_t size) {
    struct sector_cache *slot;
    int rc = change_sector(vol, inode->sector, &slot);
    if (rc != 0) return rc;
    uint8_t *entry = slot->bytes + inode->offset;
    wr16(entry + 20, first >> 16);
    wr16(entry + 26, first);
    wr32(entry + 28, size);
    entry[11] |= 0x20; // archive bit, preserving read-only/directory attributes
    slot->dirty = true;
    inode->first_cluster = first;
    inode->size = size;
    inode->attributes = entry[11];
    return 0;
}

static int mutable_file(fat32_file_t *file) {
    if (file == NULL || file->volume == NULL || file->inode == NULL) return FS_ERR_INVAL;
    if (file->is_dir) return FS_ERR_ISDIR;
    if (!(file->flags & 3u)) return FS_ERR_RO;
    if (file->inode->attributes & 1) return FS_ERR_RO;
    int rc = recover_pending(file->volume);
    if (rc != 0) return rc;
    refresh_handle(file);
    return 0;
}

long fat32_write(fat32_file_t *file, const void *buf, uint32_t len) {
    int rc = mutable_file(file);
    if (rc != 0) return rc;
    if (len && buf == NULL) return FS_ERR_INVAL;
    if (len == 0) return 0;
    fat32_volume_t *vol = file->volume;
    if (file->flags & FAT32_O_APPEND) file->pos = file->size;
    // A second handle can shrink the file beneath this independent cursor.
    if (file->pos > file->size) return FS_ERR_INVAL;
    if (len > 0xffffffffu - file->pos) return FS_ERR_INVAL;
    uint32_t count, last;
    rc = validate_chain(vol, file->first_cluster, file->size, &count, &last);
    if (rc != 0) return rc;
    rc = begin_mutation(vol);
    if (rc != 0) return rc;
    uint32_t cluster_bytes = vol->sec_per_clus * BLK_SECTOR_SIZE;
    uint32_t done = 0, first = file->first_cluster, cluster = 0;
    uint32_t index = file->pos / cluster_bytes;
    if (index < count) {
        rc = cluster_at(vol, first, index, &cluster);
        if (rc != 0) return rc;
    }
    const uint8_t *source = buf;
    while (done < len) {
        if (index >= count) {
            vol->allocated_previous = last;
            rc = allocate_cluster(vol, &cluster);
            if (rc != 0) break;
            if (last) {
                rc = fat_set(vol, last, cluster);
                if (rc != 0) break;
            } else {
                first = cluster;
            }
            last = cluster;
            count++;
        }
        uint32_t offset = file->pos % cluster_bytes;
        while (offset < cluster_bytes && done < len) {
            struct sector_cache *slot;
            rc = cache_get(vol, cluster_to_lba(vol, cluster) + offset / BLK_SECTOR_SIZE, &slot);
            if (rc != 0) break;
            uint32_t sector_offset = offset % BLK_SECTOR_SIZE;
            uint32_t chunk = BLK_SECTOR_SIZE - sector_offset;
            if (chunk > len - done) chunk = len - done;
            // Load metadata's sector before changing data/cursor. It may cause
            // an eviction, so reacquire the data slot afterwards.
            struct sector_cache *metadata;
            rc = cache_get(vol, file->inode->sector, &metadata);
            if (rc != 0) break;
            rc = cache_get(vol, cluster_to_lba(vol, cluster) + offset / BLK_SECTOR_SIZE, &slot);
            if (rc != 0) break;
            memcpy(slot->bytes + sector_offset, source + done, chunk);
            slot->dirty = true;
            uint32_t next_pos = file->pos + chunk;
            uint32_t size = next_pos > file->inode->size ? next_pos : file->inode->size;
            rc = inode_store(vol, file->inode, first, size);
            if (rc != 0) break;
            vol->allocated_cluster = 0; // now reachable through coherent metadata
            file->pos = next_pos;
            done += chunk;
            offset += chunk;
        }
        if (rc != 0) break;
        index++;
        if (done < len && index < count) {
            rc = fat_next(vol, cluster, &cluster);
            if (rc != 0) break;
            if (!cluster_valid(vol, cluster)) { rc = FS_ERR_CORRUPT; break; }
        }
    }
    refresh_handle(file);
    // Invalidate an advisory count if an incomplete transport operation could
    // have changed a FAT copy. Sync propagates subsequent transport failures.
    if (rc != 0 && rc != FS_ERR_NOSPC) vol->free_count_known = false;
    return done ? (long)done : rc;
}

int fat32_truncate(fat32_file_t *file, uint32_t size) {
    int rc = mutable_file(file);
    if (rc != 0) return rc;
    if (size > file->size) return FS_ERR_INVAL; // holes/growth are deferred
    if (size == file->size) return 0;
    fat32_volume_t *vol = file->volume;
    uint32_t count, last;
    rc = validate_chain(vol, file->first_cluster, file->size, &count, &last);
    if (rc != 0) return rc;
    rc = begin_mutation(vol);
    if (rc != 0) return rc;
    uint32_t cluster_bytes = vol->sec_per_clus * BLK_SECTOR_SIZE;
    uint32_t keep = size ? (size - 1) / cluster_bytes + 1 : 0;
    uint32_t tail = file->first_cluster, retained = 0;
    if (keep) {
        rc = cluster_at(vol, tail, keep - 1, &retained);
        if (rc != 0) return rc;
        rc = fat_next(vol, retained, &tail);
        if (rc != 0) return rc;
    }
    // Remove references to the tail before freeing any of its FAT entries.
    rc = inode_store(vol, file->inode, keep ? file->first_cluster : 0, size);
    if (rc != 0) return rc;
    refresh_handle(file);
    vol->reclaim_retained = retained;
    vol->reclaim_cluster = tail;
    vol->reclaim_remaining = count - keep;
    vol->reclaim_barrier = true;
    vol->reclaim_next_valid = false;
    return recover_pending(vol);
}

// Namespace operations retain an undo record until they complete. This is an
// in-memory recovery protocol for reported I/O errors, not a power-loss journal.
static void discard_namespace_undo(fat32_volume_t *vol) {
    while (vol->undo->sectors) {
        struct sector_undo *node = vol->undo->sectors;
        vol->undo->sectors = node->next;
        kfree(node);
    }
    kfree(vol->undo);
    vol->undo = NULL;
    vol->undo_active = false;
    vol->undo_pending = false;
}

static int rollback_namespace(fat32_volume_t *vol) {
    vol->undo_active = false;
    vol->undo_pending = true;
    // The snapshots supersede any incomplete namespace allocation/FAT intent.
    vol->fat_pending = false;
    vol->allocated_cluster = 0;
    vol->reclaim_remaining = 0;
    vol->reclaim_barrier = false;
    vol->reclaim_next_valid = false;
    for (struct sector_undo *node = vol->undo->sectors; node; node = node->next) {
        struct sector_cache *slot;
        int rc = cache_get(vol, node->lba, &slot);
        if (rc != 0) return rc;
        memcpy(slot->bytes, node->bytes, BLK_SECTOR_SIZE);
        slot->dirty = true;
    }
    int rc = cache_flush(vol);
    if (rc != 0) return rc;
    vol->free_count = vol->undo->free_count;
    vol->free_count_known = vol->undo->free_count_known;
    vol->allocation_hint = vol->undo->allocation_hint;
    discard_namespace_undo(vol);
    return 0;
}

static int namespace_begin(fat32_volume_t *vol) {
    if (vol == NULL) return FS_ERR_NOFS;
    int rc = begin_mutation(vol);
    if (rc != 0) return rc;
    struct namespace_undo *undo = kmalloc(sizeof(*undo));
    if (undo == NULL) return FS_ERR_IO;
    undo->free_count = vol->free_count;
    undo->free_count_known = vol->free_count_known;
    undo->allocation_hint = vol->allocation_hint;
    vol->undo = undo;
    vol->undo_active = true;
    return 0;
}

static int namespace_finish(fat32_volume_t *vol, int rc) {
    if (rc != 0) {
        (void)rollback_namespace(vol);
        return rc;
    }
    vol->allocated_cluster = 0;
    discard_namespace_undo(vol);
    return 0;
}

static int namespace_writable(fat32_volume_t *vol) {
    if (vol == NULL) return FS_ERR_NOFS;
    if (vol->dev->read_only) return FS_ERR_RO;
    if (vol->dev->write == NULL || vol->dev->flush == NULL) return FS_ERR_UNSUPPORTED;
    return recover_pending(vol);
}

struct filename {
    uint16_t units[255];
    unsigned count;
    char text[FAT32_NAME_MAX + 1];
    uint8_t alias[11];
};

// Reject overlong encodings, lone continuation bytes, surrogate code points,
// and values beyond Unicode. VFAT's 0xffff padding cannot be a name unit.
static int decode_filename(const char *text, struct filename *name) {
    size_t len = 0;
    while (len <= FAT32_NAME_MAX && text[len]) len++;
    if (len == 0 || len > FAT32_NAME_MAX || text[len - 1] == '.' || text[len - 1] == ' ' ||
        strcmp(text, ".") == 0 || strcmp(text, "..") == 0) return FS_ERR_BADNAME;
    name->count = 0;
    for (size_t i = 0; i < len;) {
        uint32_t first = (uint8_t)text[i++], cp;
        unsigned more;
        if (first < 0x80) { cp = first; more = 0; }
        else if (first >= 0xc2 && first <= 0xdf) { cp = first & 0x1f; more = 1; }
        else if (first >= 0xe0 && first <= 0xef) { cp = first & 0x0f; more = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { cp = first & 7; more = 3; }
        else return FS_ERR_BADNAME;
        if (more > len - i) return FS_ERR_BADNAME;
        for (unsigned k = 0; k < more; k++) {
            uint32_t byte = (uint8_t)text[i++];
            if ((byte & 0xc0) != 0x80) return FS_ERR_BADNAME;
            cp = (cp << 6) | (byte & 0x3f);
        }
        if ((more == 1 && cp < 0x80) || (more == 2 && cp < 0x800) ||
            (more == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp == 0xffff) return FS_ERR_BADNAME;
        if (cp < 0x20 || cp == 0x7f || cp == '"' || cp == '*' || cp == '/' ||
            cp == ':' || cp == '<' || cp == '>' || cp == '?' || cp == '\\' || cp == '|') {
            return FS_ERR_BADNAME;
        }
        if (cp >= 0x10000) {
            if (name->count + 2 > 255) return FS_ERR_BADNAME;
            cp -= 0x10000;
            name->units[name->count++] = (uint16_t)(0xd800 | (cp >> 10));
            name->units[name->count++] = (uint16_t)(0xdc00 | (cp & 0x3ff));
        } else {
            if (name->count == 255) return FS_ERR_BADNAME;
            name->units[name->count++] = (uint16_t)cp;
        }
    }
    memcpy(name->text, text, len + 1);
    return 0;
}

struct parent_name {
    fat32_dirent_t parent;
    struct filename name;
    char path[4096];
};

static int parent_and_name(fat32_volume_t *vol, const char *path, struct parent_name *out) {
    if (path == NULL || path[0] != '/') return FS_ERR_INVAL;
    size_t len = 0, split = 0;
    while (len < sizeof(out->path) && path[len]) {
        if (path[len] == '/') split = len;
        len++;
    }
    if (len == sizeof(out->path)) return FS_ERR_INVAL;
    int rc = decode_filename(path + split + 1, &out->name);
    if (rc != 0) return rc;
    size_t parent_len = split ? split : 1;
    memcpy(out->path, path, parent_len);
    out->path[parent_len] = 0;
    rc = fat32_lookup(vol, out->path, &out->parent);
    if (rc == 0 && !out->parent.is_dir) rc = FS_ERR_NOTDIR;
    return rc;
}

static int directory_geometry(fat32_volume_t *vol, uint32_t first,
                               uint32_t *count, uint32_t *last, uint32_t *slots) {
    int rc = validate_chain(vol, first, 0, count, last);
    if (rc != 0) return rc;
    uint64_t capacity = (uint64_t)*count * vol->sec_per_clus * (BLK_SECTOR_SIZE / 32);
    if (*count == 0 || capacity > 0xffffffffu / 32) return FS_ERR_CORRUPT;
    *slots = (uint32_t)capacity;
    return 0;
}

static int directory_slot(fat32_volume_t *vol, uint32_t first, uint32_t index,
                          uint32_t *sector, uint16_t *offset) {
    uint32_t slots_per_cluster = vol->sec_per_clus * (BLK_SECTOR_SIZE / 32), cluster;
    int rc = cluster_at(vol, first, index / slots_per_cluster, &cluster);
    if (rc != 0) return rc;
    *sector = cluster_to_lba(vol, cluster) + (index % slots_per_cluster) / 16;
    *offset = (uint16_t)((index % 16) * 32);
    return 0;
}

static int entry_read(fat32_volume_t *vol, uint32_t first, uint32_t index, uint8_t *entry) {
    uint32_t sector;
    uint16_t offset;
    int rc = directory_slot(vol, first, index, &sector, &offset);
    if (rc != 0) return rc;
    struct sector_cache *slot;
    rc = cache_get(vol, sector, &slot);
    if (rc == 0) memcpy(entry, slot->bytes + offset, 32);
    return rc;
}

static int entry_write(fat32_volume_t *vol, uint32_t first, uint32_t index, const uint8_t *entry) {
    uint32_t sector;
    uint16_t offset;
    int rc = directory_slot(vol, first, index, &sector, &offset);
    if (rc != 0) return rc;
    struct sector_cache *slot;
    rc = change_sector(vol, sector, &slot);
    if (rc != 0) return rc;
    memcpy(slot->bytes + offset, entry, 32);
    slot->dirty = true;
    return 0;
}

static bool short_character(uint8_t c) {
    if (c <= 0x20 || c >= 0x7f) return false;
    const char *forbidden = "\"*+,./:;<=>?[\\]|";
    while (*forbidden) if (c == (uint8_t)*forbidden++) return false;
    return true;
}

static uint8_t upper_byte(uint8_t c) {
    return c >= 'a' && c <= 'z' ? (uint8_t)(c - 32) : c;
}

static int alias_exists(fat32_volume_t *vol, uint32_t parent, const uint8_t *alias,
                        const fat32_dirent_t *exclude, bool *exists) {
    uint32_t count, last, slots;
    int rc = directory_geometry(vol, parent, &count, &last, &slots);
    if (rc != 0) return rc;
    *exists = false;
    for (uint32_t i = 0; i < slots; i++) {
        uint8_t entry[32];
        rc = entry_read(vol, parent, i, entry);
        if (rc != 0) return rc;
        if (entry[0] == 0) break;
        if (entry[0] == 0xe5 || entry[11] == 0x0f) continue;
        if (exclude && exclude->parent_cluster == parent && exclude->entry_index == i) continue;
        unsigned k = 0;
        while (k < 11 && upper_byte(entry[k]) == alias[k]) k++;
        if (k == 11) { *exists = true; return 0; }
    }
    // An alias must not shadow another entry's long name either. Both names
    // participate in lookup, including LFNs that look exactly like an 8.3 name.
    char alias_name[13];
    name_from_83(alias, alias_name);
    fat32_dirent_t *entry = kmalloc(sizeof(*entry));
    if (entry == NULL) return FS_ERR_IO;
    fat32_file_t dir = {.volume = vol, .first_cluster = parent, .is_dir = true};
    while ((rc = fat32_readdir(&dir, entry)) == 1) {
        if (exclude && exclude->parent_cluster == parent && exclude->entry_index == entry->entry_index) continue;
        if (name_eq(entry->name, alias_name, (size_t)strlen(alias_name))) {
            *exists = true;
            break;
        }
    }
    kfree(entry);
    return rc < 0 ? rc : 0;
}

static int make_alias(fat32_volume_t *vol, uint32_t parent, struct filename *name,
                      const fat32_dirent_t *exclude) {
    size_t len = (size_t)strlen(name->text), split = len;
    for (size_t i = 1; i < len; i++) if (name->text[i] == '.') split = i;
    size_t base_len = split, ext_len = split < len ? len - split - 1 : 0;
    bool direct = base_len > 0 && base_len <= 8 && ext_len <= 3;
    memset(name->alias, ' ', 11);
    for (size_t i = 0; i < base_len; i++) {
        uint8_t c = (uint8_t)name->text[i];
        if (!short_character(c)) direct = false;
        if (i < 8) name->alias[i] = upper_byte(c);
    }
    for (size_t i = 0; i < ext_len; i++) {
        uint8_t c = (uint8_t)name->text[split + 1 + i];
        if (!short_character(c)) direct = false;
        if (i < 3) name->alias[8 + i] = upper_byte(c);
    }
    bool exists;
    int rc = 0;
    if (direct) {
        rc = alias_exists(vol, parent, name->alias, exclude, &exists);
        if (rc != 0 || !exists) return rc;
    }
    char base[8], ext[3];
    unsigned base_count = 0, ext_count = 0;
    for (size_t i = 0; i < base_len && base_count < sizeof(base); i++) {
        uint8_t c = (uint8_t)name->text[i];
        if (c == ' ' || c == '.') continue;
        if ((c & 0xc0) == 0x80) continue;
        base[base_count++] = (char)(short_character(c) ? upper_byte(c) : '_');
    }
    if (base_count == 0) base[base_count++] = '_';
    for (size_t i = 0; i < ext_len && ext_count < sizeof(ext); i++) {
        uint8_t c = (uint8_t)name->text[split + 1 + i];
        if (c == ' ' || c == '.') continue;
        if ((c & 0xc0) == 0x80) continue;
        ext[ext_count++] = (char)(short_character(c) ? upper_byte(c) : '_');
    }
    for (uint32_t number = 1; number < 1000000; number++) {
        char digits[6];
        unsigned n = 0;
        for (uint32_t value = number; value; value /= 10) digits[n++] = (char)('0' + value % 10);
        unsigned prefix = 7 - n;
        if (prefix > base_count) prefix = base_count;
        memset(name->alias, ' ', 11);
        memcpy(name->alias, base, prefix);
        name->alias[prefix] = '~';
        for (unsigned i = 0; i < n; i++) name->alias[prefix + 1 + i] = (uint8_t)digits[n - 1 - i];
        memcpy(name->alias + 8, ext, ext_count);
        rc = alias_exists(vol, parent, name->alias, exclude, &exists);
        if (rc != 0 || !exists) return rc;
    }
    return FS_ERR_NOSPC;
}

static int namespace_allocate(fat32_volume_t *vol, uint32_t *out) {
    vol->allocated_previous = 0;
    int rc = allocate_cluster(vol, out);
    if (rc == 0) vol->allocated_cluster = 0; // the namespace undo owns this reservation
    return rc;
}

static int extend_directory(fat32_volume_t *vol, uint32_t *last, uint32_t *slots) {
    uint32_t added = vol->sec_per_clus * (BLK_SECTOR_SIZE / 32);
    if (*slots > 0xffffffffu / 32 - added) return FS_ERR_NOSPC;
    uint32_t cluster;
    int rc = namespace_allocate(vol, &cluster);
    if (rc != 0) return rc;
    rc = fat_set(vol, *last, cluster);
    if (rc != 0) return rc;
    *last = cluster;
    *slots += added;
    return 0;
}

static int reserve_entries(fat32_volume_t *vol, uint32_t parent, unsigned needed, uint32_t *start) {
    uint32_t count, last, slots;
    int rc = directory_geometry(vol, parent, &count, &last, &slots);
    if (rc != 0) return rc;
    uint32_t run = 0;
    bool end_seen = false, run_end = false;
    for (uint32_t index = 0;; index++) {
        if (index == slots) {
            rc = extend_directory(vol, &last, &slots);
            if (rc != 0) return rc;
            end_seen = true;
        }
        uint8_t entry[32];
        rc = entry_read(vol, parent, index, entry);
        if (rc != 0) return rc;
        if (entry[0] == 0) end_seen = true;
        if (end_seen || entry[0] == 0xe5) {
            if (run == 0) { *start = index; run_end = false; }
            run++;
            if (end_seen) run_end = true;
        } else run = 0;
        if (run == needed) {
            if (run_end) {
                if (index + 1 == slots) {
                    rc = extend_directory(vol, &last, &slots);
                    if (rc != 0) return rc;
                }
                uint8_t terminator[32] = {0};
                rc = entry_write(vol, parent, index + 1, terminator);
            }
            return rc;
        }
    }
}

static unsigned name_entries(const struct filename *name) {
    return (name->count + 12) / 13 + 1;
}

static int write_named_entry(fat32_volume_t *vol, uint32_t parent, uint32_t start,
                             const struct filename *name, const uint8_t *metadata) {
    unsigned lfns = name_entries(name) - 1;
    uint8_t checksum = short_checksum(name->alias);
    for (unsigned i = 0; i < lfns; i++) {
        unsigned ord = lfns - i;
        uint8_t entry[32] = {0};
        entry[0] = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        entry[11] = 0x0f;
        entry[13] = checksum;
        for (unsigned j = 0; j < 13; j++) {
            unsigned index = (ord - 1) * 13 + j;
            uint16_t unit = index < name->count ? name->units[index] :
                            index == name->count ? 0 : 0xffffu;
            wr16(entry + lfn_off[j], unit);
        }
        int rc = entry_write(vol, parent, start + i, entry);
        if (rc != 0) return rc;
    }
    uint8_t entry[32];
    memcpy(entry, metadata, 32);
    memcpy(entry, name->alias, 11);
    entry[12] = 0; // all spelling is preserved in the LFN
    return entry_write(vol, parent, start + lfns, entry);
}

static int delete_named_entry(fat32_volume_t *vol, const fat32_dirent_t *entry) {
    if (entry->entry_index < entry->lfn_slots) return FS_ERR_CORRUPT;
    for (uint32_t i = entry->entry_index - entry->lfn_slots; i <= entry->entry_index; i++) {
        uint8_t raw[32];
        int rc = entry_read(vol, entry->parent_cluster, i, raw);
        if (rc != 0) return rc;
        raw[0] = 0xe5;
        rc = entry_write(vol, entry->parent_cluster, i, raw);
        if (rc != 0) return rc;
    }
    return 0;
}

static int namespace_free_chain(fat32_volume_t *vol, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t next;
        int rc = fat_next(vol, first, &next);
        if (rc != 0) return rc;
        rc = fat_set(vol, first, 0);
        if (rc != 0) return rc;
        if (vol->free_count_known) vol->free_count++;
        if (first < vol->allocation_hint) vol->allocation_hint = first;
        first = next;
    }
    return 0;
}

static void initial_metadata(uint8_t *entry, uint8_t attributes, uint32_t cluster) {
    memset(entry, 0, 32);
    entry[11] = attributes;
    wr16(entry + 16, 0x21); // 1980-01-01 until a wall clock is available
    wr16(entry + 18, 0x21);
    wr16(entry + 24, 0x21);
    wr16(entry + 20, cluster >> 16);
    wr16(entry + 26, cluster);
}

struct create_scratch {
    struct parent_name target;
    fat32_dirent_t entry;
};

static int create_file(fat32_volume_t *vol, const char *path, uint32_t flags, fat32_file_t *out) {
    int rc = namespace_writable(vol);
    if (rc != 0) return rc;
    struct create_scratch *s = kmalloc(sizeof(*s));
    if (s == NULL) return FS_ERR_IO;
    rc = parent_and_name(vol, path, &s->target);
    if (rc == 0 && (s->target.parent.attributes & 1)) rc = FS_ERR_RO;
    if (rc == 0) rc = make_alias(vol, s->target.parent.first_cluster, &s->target.name, NULL);
    if (rc != 0) { kfree(s); return rc; }
    rc = namespace_begin(vol);
    if (rc != 0) { kfree(s); return rc; }
    uint32_t start;
    rc = reserve_entries(vol, s->target.parent.first_cluster, name_entries(&s->target.name), &start);
    if (rc == 0) {
        uint8_t metadata[32];
        initial_metadata(metadata, 0x20, 0);
        rc = write_named_entry(vol, s->target.parent.first_cluster, start, &s->target.name, metadata);
    }
    if (rc == 0) rc = fat32_lookup(vol, path, &s->entry);
    if (rc == 0) rc = handle_open(vol, &s->entry, flags, out);
    rc = namespace_finish(vol, rc);
    kfree(s);
    return rc;
}

int fat32_mkdir(fat32_volume_t *vol, const char *path) {
    int rc = namespace_writable(vol);
    if (rc != 0) return rc;
    struct create_scratch *s = kmalloc(sizeof(*s));
    if (s == NULL) return FS_ERR_IO;
    rc = parent_and_name(vol, path, &s->target);
    if (rc == 0 && (s->target.parent.attributes & 1)) rc = FS_ERR_RO;
    if (rc == 0) {
        rc = fat32_lookup(vol, path, &s->entry);
        if (rc == 0) rc = FS_ERR_EXISTS;
        else if (rc == FS_ERR_NOTFOUND) rc = 0;
    }
    if (rc == 0) rc = make_alias(vol, s->target.parent.first_cluster, &s->target.name, NULL);
    if (rc != 0) { kfree(s); return rc; }
    rc = namespace_begin(vol);
    if (rc != 0) { kfree(s); return rc; }
    uint32_t cluster, start;
    rc = namespace_allocate(vol, &cluster);
    if (rc == 0) {
        uint8_t dot[32];
        initial_metadata(dot, 0x10, cluster);
        memset(dot, ' ', 11);
        dot[0] = '.';
        rc = entry_write(vol, cluster, 0, dot);
        if (rc == 0) {
            dot[1] = '.';
            uint32_t parent = s->target.parent.first_cluster;
            if (parent == vol->root_cluster) parent = 0;
            wr16(dot + 20, parent >> 16);
            wr16(dot + 26, parent);
            rc = entry_write(vol, cluster, 1, dot);
        }
    }
    if (rc == 0) rc = reserve_entries(vol, s->target.parent.first_cluster, name_entries(&s->target.name), &start);
    if (rc == 0) {
        uint8_t metadata[32];
        initial_metadata(metadata, 0x10, cluster);
        rc = write_named_entry(vol, s->target.parent.first_cluster, start, &s->target.name, metadata);
    }
    rc = namespace_finish(vol, rc);
    kfree(s);
    return rc;
}

static int empty_directory(fat32_volume_t *vol, uint32_t first) {
    uint32_t count, last, slots;
    int rc = directory_geometry(vol, first, &count, &last, &slots);
    if (rc != 0) return rc;
    for (uint32_t i = 0; i < slots; i++) {
        uint8_t raw[32];
        rc = entry_read(vol, first, i, raw);
        if (rc != 0) return rc;
        if (raw[0] == 0) return 0;
        if (raw[0] == 0xe5 || raw[11] == 0x0f || (raw[11] & 8)) continue;
        if (raw[0] == '.' && raw[1] == ' ') continue;
        if (raw[0] == '.' && raw[1] == '.' && raw[2] == ' ') continue;
        return FS_ERR_NOTEMPTY;
    }
    return 0;
}

static int remove_entry(fat32_volume_t *vol, const char *path, bool directory) {
    int rc = namespace_writable(vol);
    if (rc != 0) return rc;
    struct create_scratch *s = kmalloc(sizeof(*s));
    if (s == NULL) return FS_ERR_IO;
    rc = parent_and_name(vol, path, &s->target);
    if (rc == 0) rc = fat32_lookup(vol, path, &s->entry);
    if (rc == 0 && s->entry.is_dir != directory) rc = directory ? FS_ERR_NOTDIR : FS_ERR_ISDIR;
    if (rc == 0 && ((s->entry.attributes | s->target.parent.attributes) & 1)) rc = FS_ERR_RO;
    if (rc == 0 && fat32_entry_busy(vol, s->entry.entry_sector, s->entry.entry_offset)) rc = FS_ERR_BUSY;
    if (rc == 0 && directory) rc = empty_directory(vol, s->entry.first_cluster);
    uint32_t count = 0, last;
    if (rc == 0) rc = validate_chain(vol, s->entry.first_cluster, directory ? 0 : s->entry.size, &count, &last);
    if (rc != 0) { kfree(s); return rc; }
    rc = namespace_begin(vol);
    if (rc != 0) { kfree(s); return rc; }
    rc = delete_named_entry(vol, &s->entry);
    if (rc == 0) rc = namespace_free_chain(vol, s->entry.first_cluster, count);
    rc = namespace_finish(vol, rc);
    kfree(s);
    return rc;
}

int fat32_unlink(fat32_volume_t *vol, const char *path) {
    return remove_entry(vol, path, false);
}

int fat32_rmdir(fat32_volume_t *vol, const char *path) {
    return remove_entry(vol, path, true);
}

static int directory_parent(fat32_volume_t *vol, uint32_t cluster, uint32_t *parent) {
    if (cluster == vol->root_cluster) { *parent = cluster; return 0; }
    uint8_t entry[32];
    int rc = entry_read(vol, cluster, 1, entry);
    if (rc != 0) return rc;
    if (entry[0] != '.' || entry[1] != '.' || entry[2] != ' ' || !(entry[11] & 0x10)) {
        return FS_ERR_CORRUPT;
    }
    *parent = (rd16(entry + 20) << 16) | rd16(entry + 26);
    if (*parent == 0) *parent = vol->root_cluster;
    return cluster_valid(vol, *parent) ? 0 : FS_ERR_CORRUPT;
}

static int check_directory_move(fat32_volume_t *vol, uint32_t source, uint32_t target_parent) {
    uint32_t slow = target_parent, fast = target_parent;
    for (uint32_t i = 0; i < vol->cluster_count; i++) {
        if (target_parent == source) return FS_ERR_INVAL;
        if (target_parent == vol->root_cluster) return 0;
        int rc = directory_parent(vol, target_parent, &target_parent);
        if (rc != 0) return rc;
        if (slow != vol->root_cluster) {
            rc = directory_parent(vol, slow, &slow);
            if (rc != 0) return rc;
        }
        for (unsigned step = 0; step < 2 && fast != vol->root_cluster; step++) {
            rc = directory_parent(vol, fast, &fast);
            if (rc != 0) return rc;
        }
        if (fast != vol->root_cluster && slow == fast) return FS_ERR_CORRUPT;
    }
    return FS_ERR_CORRUPT;
}

struct rename_scratch {
    struct parent_name source_parent;
    struct parent_name target;
    fat32_dirent_t source;
    fat32_dirent_t destination;
};

int fat32_rename(fat32_volume_t *vol, const char *source, const char *destination) {
    int rc = namespace_writable(vol);
    if (rc != 0) return rc;
    struct rename_scratch *s = kmalloc(sizeof(*s));
    if (s == NULL) return FS_ERR_IO;
    rc = parent_and_name(vol, source, &s->source_parent);
    if (rc == 0) rc = fat32_lookup(vol, source, &s->source);
    if (rc == 0) rc = parent_and_name(vol, destination, &s->target);
    if (rc == 0 && ((s->source.attributes | s->source_parent.parent.attributes |
                    s->target.parent.attributes) & 1)) rc = FS_ERR_RO;
    if (rc == 0 && fat32_entry_busy(vol, s->source.entry_sector, s->source.entry_offset)) rc = FS_ERR_BUSY;
    bool same = false;
    if (rc == 0) {
        rc = fat32_lookup(vol, destination, &s->destination);
        if (rc == 0) {
            same = s->source.entry_sector == s->destination.entry_sector &&
                   s->source.entry_offset == s->destination.entry_offset;
            if (!same || !name_eq(s->source.name, s->target.name.text,
                                  (size_t)strlen(s->target.name.text))) rc = FS_ERR_EXISTS;
        } else if (rc == FS_ERR_NOTFOUND) rc = 0;
    }
    uint32_t chain_count, chain_last;
    if (rc == 0 && s->source.is_dir && !cluster_valid(vol, s->source.first_cluster)) rc = FS_ERR_CORRUPT;
    if (rc == 0) rc = validate_chain(vol, s->source.first_cluster,
                                     s->source.is_dir ? 0 : s->source.size, &chain_count, &chain_last);
    if (rc == 0 && s->source.is_dir) {
        rc = check_directory_move(vol, s->source.first_cluster, s->target.parent.first_cluster);
    }
    if (rc == 0 && same) {
        uint8_t original[32];
        rc = entry_read(vol, s->source.parent_cluster, s->source.entry_index, original);
        if (rc == 0) memcpy(s->target.name.alias, original, 11);
    } else if (rc == 0) {
        rc = make_alias(vol, s->target.parent.first_cluster, &s->target.name, &s->source);
    }
    if (rc != 0) { kfree(s); return rc; }
    rc = namespace_begin(vol);
    if (rc != 0) { kfree(s); return rc; }
    uint8_t metadata[32];
    rc = entry_read(vol, s->source.parent_cluster, s->source.entry_index, metadata);
    uint32_t start = 0;
    unsigned needed = name_entries(&s->target.name);
    bool in_place = same && s->source.parent_cluster == s->target.parent.first_cluster &&
                    needed <= (unsigned)s->source.lfn_slots + 1;
    if (rc == 0 && in_place) {
        rc = delete_named_entry(vol, &s->source);
        start = s->source.entry_index + 1 - needed;
    } else if (rc == 0) {
        rc = reserve_entries(vol, s->target.parent.first_cluster, needed, &start);
    }
    if (rc == 0) rc = write_named_entry(vol, s->target.parent.first_cluster, start, &s->target.name, metadata);
    if (rc == 0 && s->source.is_dir && s->source.parent_cluster != s->target.parent.first_cluster) {
        uint8_t dotdot[32];
        rc = entry_read(vol, s->source.first_cluster, 1, dotdot);
        if (rc == 0) {
            if (dotdot[0] != '.' || dotdot[1] != '.' || !(dotdot[11] & 0x10)) rc = FS_ERR_CORRUPT;
            else {
                uint32_t parent = s->target.parent.first_cluster;
                if (parent == vol->root_cluster) parent = 0;
                wr16(dotdot + 20, parent >> 16);
                wr16(dotdot + 26, parent);
                rc = entry_write(vol, s->source.first_cluster, 1, dotdot);
            }
        }
    }
    if (rc == 0 && !in_place) rc = delete_named_entry(vol, &s->source);
    rc = namespace_finish(vol, rc);
    kfree(s);
    return rc;
}
