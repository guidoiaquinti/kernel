#include "fs/namespace.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

static fs_volume_info_t volumes[BLKDEV_MAX_PARTITIONS];
static unsigned volume_count;
static fat32_volume_t *boot_volume;
static fs_assign_info_t *assigns[FS_MAX_ASSIGNS];
static unsigned assign_count;
static unsigned owned_locations;

struct directory_pin {
    struct directory_pin *next;
    fat32_file_t directory;
};

struct fs_directory_pins {
    unsigned refs;
    struct directory_pin *directories;
};

static void release_pins(fs_directory_pins_t *pins) {
    if (pins == NULL || --pins->refs != 0) return;
    while (pins->directories) {
        struct directory_pin *node = pins->directories;
        pins->directories = node->next;
        fat32_close(&node->directory);
        kfree(node);
    }
    kfree(pins);
}

void fs_cwd_release(fs_cwd_t *cwd) {
    if (cwd == NULL) return;
    if (cwd->pins) {
        owned_locations--;
        release_pins(cwd->pins);
    }
    cwd->pins = NULL;
    cwd->volume = NULL;
    cwd->path[0] = 0;
}

static int pin_directory(fs_directory_pins_t *pins, fat32_volume_t *volume, const char *path) {
    struct directory_pin *node = kmalloc(sizeof(*node));
    if (node == NULL) return FS_ERR_IO;
    int rc = fat32_opendir(volume, path, &node->directory);
    if (rc != 0) {
        kfree(node);
        return rc;
    }
    node->next = pins->directories;
    pins->directories = node;
    return 0;
}

static int pin_location(fs_cwd_t *location) {
    if (location == NULL || location->volume == NULL || location->path[0] != '/') {
        return FS_ERR_INVAL;
    }
    size_t len = 0;
    while (len < FS_PATH_MAX && location->path[len]) len++;
    if (len == FS_PATH_MAX) return FS_ERR_INVAL;
    fs_directory_pins_t *pins = kmalloc(sizeof(*pins));
    if (pins == NULL) return FS_ERR_IO;
    pins->refs = 1;
    char *prefix = kmalloc(len + 1);
    if (prefix == NULL) {
        release_pins(pins);
        return FS_ERR_IO;
    }
    memcpy(prefix, location->path, len + 1);
    char saved = prefix[1];
    prefix[1] = 0;
    int rc = pin_directory(pins, location->volume, prefix);
    prefix[1] = saved;
    // Pin every ancestor too. Renaming a parent would otherwise invalidate
    // the canonical paths retained by children and single-target assigns.
    for (size_t i = 1; rc == 0 && i <= len && len > 1; i++) {
        if (prefix[i] != '/' && i != len) continue;
        saved = prefix[i];
        prefix[i] = 0;
        rc = pin_directory(pins, location->volume, prefix);
        prefix[i] = saved;
    }
    kfree(prefix);
    if (rc != 0) {
        release_pins(pins);
        return rc;
    }
    location->pins = pins;
    owned_locations++;
    return 0;
}

int fs_cwd_copy(fs_cwd_t *out, const fs_cwd_t *source) {
    if (out == NULL || source == NULL || source->volume == NULL) return FS_ERR_INVAL;
    if (out == source) return 0;
    *out = *source;
    if (out->pins) {
        out->pins->refs++;
        owned_locations++;
        return 0;
    }
    return pin_location(out);
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool name_equal(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] == 0 || lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return a[n] == 0;
}

// Return a volume-local anchor, not merely a volume: assigns can name a
// subdirectory. Devices precede assigns, which precede unique volume labels.
static int prefix_location(const char *name, size_t len,
                            fat32_volume_t **volume, const char **path) {
    for (size_t i = 0; i < len; i++) {
        if (name[i] == ':' || name[i] == '/' || (uint8_t)name[i] < 0x20) {
            return FS_ERR_NOTFOUND;
        }
    }
    for (unsigned i = 0; i < volume_count; i++) {
        if (name_equal(volumes[i].device, name, len)) {
            *volume = volumes[i].volume;
            *path = "/";
            return 0;
        }
    }
    for (unsigned i = 0; i < assign_count; i++) {
        if (name_equal(assigns[i]->name, name, len)) {
            *volume = assigns[i]->target.volume;
            *path = assigns[i]->target.path;
            return 0;
        }
    }
    if (name_equal("sys", name, len) || name_equal("c", name, len)) {
        return FS_ERR_NOTFOUND; // reserved defaults never fall through to labels
    }
    fat32_volume_t *match = NULL;
    for (unsigned i = 0; i < volume_count; i++) {
        if (len && name_equal(volumes[i].label, name, len)) {
            if (match != NULL) {
                return FS_ERR_NOTFOUND; // duplicate labels are ambiguous
            }
            match = volumes[i].volume;
        }
    }
    if (match == NULL) {
        return FS_ERR_NOTFOUND;
    }
    *volume = match;
    *path = "/";
    return 0;
}

int fs_namespace_reset(void) {
    // Each assign owns one location. Additional owners include tasks and boot
    // loader temporaries, even when they share an assign's pin bundle.
    if (owned_locations != assign_count) return FS_ERR_BUSY;
    for (unsigned i = 0; i < volume_count; i++) {
        unsigned assign_handles = 0;
        for (unsigned a = 0; a < assign_count; a++) {
            fs_directory_pins_t *pins = assigns[a]->target.pins;
            for (struct directory_pin *p = pins ? pins->directories : NULL; p; p = p->next) {
                if (p->directory.volume == volumes[i].volume) assign_handles++;
            }
        }
        if (fat32_handle_count(volumes[i].volume) != assign_handles) return FS_ERR_BUSY;
    }
    // Keep every pointer and namespace name intact if durable sync fails.
    for (unsigned i = 0; i < volume_count; i++) {
        int rc = fat32_sync_volume(volumes[i].volume);
        if (rc != 0) return rc;
    }
    for (unsigned i = 0; i < assign_count; i++) {
        fs_cwd_release(&assigns[i]->target);
        kfree(assigns[i]);
    }
    assign_count = 0;
    unsigned retained = 0;
    int error = 0;
    for (unsigned i = 0; i < volume_count; i++) {
        int rc = fat32_unmount(volumes[i].volume);
        if (rc != 0) {
            volumes[retained++] = volumes[i];
            if (error == 0) error = rc;
        } else if (volumes[i].volume == boot_volume) {
            boot_volume = NULL;
        }
    }
    volume_count = retained;
    return error;
}

// Mount every registered partition. The boot volume is `boot`'s, or with
// `probe` the first volume that has a file at that path.
static int mount_all(blkdev_t *boot, const char *probe) {
    int reset_rc = fs_namespace_reset();
    if (reset_rc != 0) return reset_rc;
    int boot_error = probe != NULL ? FS_ERR_NOTFOUND : FS_ERR_NOFS;
    for (unsigned i = 0; i < blkdev_partition_count(); i++) {
        blkdev_t *dev = blkdev_partition_io(i);
        fat32_volume_t *volume;
        int rc = fat32_mount(dev, &volume);
        if (dev == boot) {
            boot_error = rc;
        }
        if (rc != 0) {
            continue;
        }
        fat32_stat_t st;
        if (probe != NULL && boot == NULL && fat32_stat(volume, probe, &st) == 0 && !st.is_dir) {
            boot = dev;
        }
        fs_volume_info_t *info = &volumes[volume_count++];
        info->volume = volume;
        info->label = fat32_label(volume);
        info->boot = dev == boot;
        info->read_only = fat32_is_read_only(volume);
        info->device[0] = 'd';
        info->device[1] = 'f';
        unsigned n = 2;
        if (i >= 10) {
            info->device[n++] = (char)('0' + i / 10);
        }
        info->device[n++] = (char)('0' + i % 10);
        info->device[n] = 0;
        if (dev == boot) {
            boot_volume = volume;
        }
    }
    if (boot_volume == NULL) {
        return boot_error;
    }
    fs_assign_info_t *sys = kmalloc(sizeof(*sys));
    if (sys == NULL) {
        fs_namespace_reset();
        return FS_ERR_IO;
    }
    memcpy(sys->name, "sys", 4);
    sys->immutable = true;
    int rc = fs_boot_cwd(&sys->target);
    if (rc != 0) {
        kfree(sys);
        fs_namespace_reset();
        return rc;
    }
    assigns[assign_count++] = sys;
    rc = fs_assign_set(&sys->target, "c", "sys:bin");
    if (rc != 0) {
        fs_namespace_reset();
    }
    return rc;
}

int fs_mount_registered(blkdev_t *boot) {
    return mount_all(boot, NULL);
}

int fs_mount_registered_search(const char *probe) {
    return mount_all(NULL, probe);
}

unsigned fs_volume_count(void) {
    return volume_count;
}

const fs_volume_info_t *fs_volume_get(unsigned index) {
    return index < volume_count ? &volumes[index] : NULL;
}

int fs_boot_cwd(fs_cwd_t *out) {
    if (out == NULL || boot_volume == NULL) {
        return FS_ERR_NOFS;
    }
    out->volume = boot_volume;
    out->pins = NULL;
    out->path[0] = '/';
    out->path[1] = 0;
    return pin_location(out);
}

struct resolve_scratch {
    fs_cwd_t location;
    fat32_dirent_t entry;
};

int fs_resolve(const fs_cwd_t *cwd, const char *path, fs_cwd_t *out,
               fat32_dirent_t *entry) {
    if (cwd == NULL || cwd->volume == NULL || path == NULL || out == NULL || out == cwd) {
        return FS_ERR_INVAL;
    }
    size_t path_len = 0;
    while (path_len < FS_QUALIFIED_PATH_MAX && path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len == FS_QUALIFIED_PATH_MAX) {
        return FS_ERR_INVAL;
    }
    struct resolve_scratch *s = kmalloc(sizeof(*s));
    if (s == NULL) {
        return FS_ERR_IO;
    }
    s->location = *cwd;
    s->location.pins = NULL; // resolution returns an unowned canonical snapshot
    const char *p = path;
    const char *colon = NULL;
    for (size_t i = 0; i < path_len; i++) {
        if (path[i] == ':') {
            colon = path + i;
            break;
        }
        if (path[i] == '/') {
            break;
        }
    }
    int rc = 0;
    if (colon != NULL) {
        const char *anchor;
        rc = prefix_location(path, (size_t)(colon - path), &s->location.volume, &anchor);
        if (rc != 0) {
            goto done;
        }
        memcpy(s->location.path, anchor, (size_t)strlen(anchor) + 1);
        p = colon + 1;
    } else if (*p == '/') {
        s->location.path[0] = '/';
        s->location.path[1] = 0;
    }
    rc = fat32_lookup(s->location.volume, s->location.path, &s->entry);
    if (rc != 0) {
        goto done;
    }
    while (*p) {
        if (!s->entry.is_dir) {
            rc = FS_ERR_NOTDIR;
            goto done;
        }
        if (*p == '/') {
            p++;
            continue;
        }
        const char *start = p;
        while (*p && *p != '/') {
            if (*p == ':') {
                rc = FS_ERR_INVAL;
                goto done;
            }
            p++;
        }
        size_t len = (size_t)(p - start);
        size_t used = (size_t)strlen(s->location.path);
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (used > 1 && s->location.path[used - 1] != '/') {
                used--;
            }
            if (used > 1) {
                used--;
            }
            s->location.path[used] = 0;
        } else {
            size_t base = used;
            if (used > 1) {
                s->location.path[used++] = '/';
            }
            if (len > FAT32_NAME_MAX || used + len >= FS_PATH_MAX) {
                rc = FS_ERR_INVAL;
                goto done;
            }
            memcpy(s->location.path + used, start, len);
            s->location.path[used + len] = 0;
            rc = fat32_lookup(s->location.volume, s->location.path, &s->entry);
            if (rc != 0) {
                goto done;
            }
            // Preserve the directory's actual spelling in canonical cwd.
            size_t actual = (size_t)strlen(s->entry.name);
            used = base;
            if (base > 1) {
                s->location.path[used++] = '/';
            }
            if (used + actual >= FS_PATH_MAX) {
                rc = FS_ERR_INVAL;
                goto done;
            }
            memcpy(s->location.path + used, s->entry.name, actual + 1);
            continue;
        }
        rc = fat32_lookup(s->location.volume, s->location.path, &s->entry);
        if (rc != 0) {
            goto done;
        }
    }
    *out = s->location;
    if (entry != NULL) {
        *entry = s->entry;
    }
done:
    kfree(s);
    return rc;
}

int fs_chdir(fs_cwd_t *cwd, const char *path) {
    struct resolve_scratch *s = kmalloc(sizeof(*s));
    if (s == NULL) {
        return FS_ERR_IO;
    }
    int rc = fs_resolve(cwd, path, &s->location, &s->entry);
    if (rc == 0 && !s->entry.is_dir) {
        rc = FS_ERR_NOTDIR;
    }
    if (rc == 0) rc = pin_location(&s->location);
    if (rc == 0) {
        fs_cwd_release(cwd);
        *cwd = s->location;
    }
    kfree(s);
    return rc;
}

int fs_getcwd(const fs_cwd_t *cwd, char *buf, size_t size) {
    if (cwd == NULL || cwd->volume == NULL || buf == NULL) {
        return FS_ERR_INVAL;
    }
    const fs_volume_info_t *info = NULL;
    for (unsigned i = 0; i < volume_count; i++) {
        if (volumes[i].volume == cwd->volume) {
            info = &volumes[i];
            break;
        }
    }
    if (info == NULL) {
        return FS_ERR_NOFS;
    }
    const char *name = info->device;
    fat32_volume_t *label_volume;
    const char *label_path;
    if (*info->label && prefix_location(info->label, (size_t)strlen(info->label),
                                      &label_volume, &label_path) == 0 &&
        label_volume == cwd->volume && strcmp(label_path, "/") == 0) {
        name = info->label;
    }
    size_t n = (size_t)strlen(name);
    size_t p = (size_t)strlen(cwd->path + 1);
    if (size < n + p + 2) {
        return FS_ERR_INVAL;
    }
    memcpy(buf, name, n);
    buf[n] = ':';
    memcpy(buf + n + 1, cwd->path + 1, p + 1);
    return 0;
}

unsigned fs_assign_count(void) {
    return assign_count;
}

const fs_assign_info_t *fs_assign_get(unsigned index) {
    return index < assign_count ? assigns[index] : NULL;
}

static int assign_name(const char *name, char *out) {
    if (name == NULL) {
        return FS_ERR_INVAL;
    }
    unsigned n = 0;
    while (n <= FS_PREFIX_MAX + 1 && name[n]) {
        n++;
    }
    if (n && name[n - 1] == ':') {
        n--;
    }
    if (n == 0 || n > FS_PREFIX_MAX) {
        return FS_ERR_INVAL;
    }
    for (unsigned i = 0; i < n; i++) {
        if ((uint8_t)name[i] <= 0x20 || (uint8_t)name[i] == 0x7f ||
            name[i] == ':' || name[i] == '/') {
            return FS_ERR_INVAL;
        }
        out[i] = lower(name[i]);
    }
    out[n] = 0;
    if (n > 2 && out[0] == 'd' && out[1] == 'f') {
        unsigned i = 2;
        while (i < n && out[i] >= '0' && out[i] <= '9') {
            i++;
        }
        if (i == n) {
            return FS_ERR_INVAL;
        }
    }
    return 0;
}

int fs_assign_set(const fs_cwd_t *cwd, const char *name, const char *target) {
    char normalized[FS_PREFIX_MAX + 1];
    int rc = assign_name(name, normalized);
    if (rc != 0 || strcmp(normalized, "sys") == 0) {
        return FS_ERR_INVAL;
    }
    unsigned index = 0;
    while (index < assign_count && strcmp(assigns[index]->name, normalized) != 0) {
        index++;
    }
    if (target == NULL) {
        if (index == assign_count) {
            return FS_ERR_NOTFOUND;
        }
        fs_cwd_release(&assigns[index]->target);
        kfree(assigns[index]);
        for (unsigned i = index + 1; i < assign_count; i++) {
            assigns[i - 1] = assigns[i];
        }
        assign_count--;
        return 0;
    }
    if (index == assign_count && assign_count == FS_MAX_ASSIGNS) {
        return FS_ERR_INVAL;
    }
    fs_assign_info_t *replacement = kmalloc(sizeof(*replacement));
    if (replacement == NULL) {
        return FS_ERR_IO;
    }
    memcpy(replacement->name, normalized, (size_t)strlen(normalized) + 1);
    replacement->immutable = false;
    fat32_dirent_t entry;
    rc = fs_resolve(cwd, target, &replacement->target, &entry);
    if (rc == 0 && !entry.is_dir) {
        rc = FS_ERR_NOTDIR;
    }
    if (rc == 0) rc = pin_location(&replacement->target);
    if (rc != 0) {
        kfree(replacement);
        return rc;
    }
    if (index < assign_count) {
        fs_cwd_release(&assigns[index]->target);
        kfree(assigns[index]);
    } else {
        assign_count++;
    }
    assigns[index] = replacement;
    return 0;
}

static int open_resolved(const fs_cwd_t *cwd, const char *path,
                         fat32_file_t *out, int kind, fat32_stat_t *stat) {
    if ((kind != 2 && out == NULL) || (kind == 2 && stat == NULL)) {
        return FS_ERR_INVAL;
    }
    struct resolve_scratch *s = kmalloc(sizeof(*s));
    if (s == NULL) {
        return FS_ERR_IO;
    }
    int rc = fs_resolve(cwd, path, &s->location, &s->entry);
    if (rc == 0) {
        if (kind == 0) {
            rc = fat32_open(s->location.volume, s->location.path, out);
        } else if (kind == 1) {
            rc = fat32_opendir(s->location.volume, s->location.path, out);
        } else {
            stat->size = s->entry.size;
            stat->is_dir = s->entry.is_dir;
        }
    }
    kfree(s);
    return rc;
}

int fs_open(const fs_cwd_t *cwd, const char *path, fat32_file_t *out) {
    return open_resolved(cwd, path, out, 0, NULL);
}

int fs_opendir(const fs_cwd_t *cwd, const char *path, fat32_file_t *out) {
    return open_resolved(cwd, path, out, 1, NULL);
}

int fs_stat(const fs_cwd_t *cwd, const char *path, fat32_stat_t *out) {
    return open_resolved(cwd, path, NULL, 2, out);
}

int fs_program_open(const fs_cwd_t *cwd, const char *name, fat32_file_t *out) {
    if (name == NULL || *name == 0) {
        return FS_ERR_INVAL;
    }
    bool bare = true;
    size_t len = 0;
    while (len < FS_QUALIFIED_PATH_MAX && name[len]) {
        if (name[len] == '/' || name[len] == ':') {
            bare = false;
        }
        len++;
    }
    if (len == FS_QUALIFIED_PATH_MAX) {
        return FS_ERR_INVAL;
    }
    if (!bare) {
        return fs_open(cwd, name, out);
    }
    if (len + 3 > FS_QUALIFIED_PATH_MAX) {
        return FS_ERR_INVAL;
    }
    char *path = kmalloc(len + 3);
    if (path == NULL) {
        return FS_ERR_IO;
    }
    memcpy(path, "c:", 2);
    memcpy(path + 2, name, len + 1);
    int rc = fs_open(cwd, path, out);
    kfree(path);
    return rc;
}

// Resolve an existing parent while retaining the caller's exact new leaf
// spelling. A qualified bare leaf such as extras:new starts at that root.
static int resolve_new_leaf(const fs_cwd_t *cwd, const char *path, fs_cwd_t *out) {
    if (cwd == NULL || path == NULL || out == NULL) return FS_ERR_INVAL;
    size_t len = 0, separator = 0;
    bool found = false, slash_seen = false;
    while (len < FS_QUALIFIED_PATH_MAX && path[len]) {
        if (path[len] == '/') { separator = len; found = true; slash_seen = true; }
        else if (path[len] == ':' && !slash_seen) { separator = len; found = true; }
        len++;
    }
    if (len == 0 || len == FS_QUALIFIED_PATH_MAX) return FS_ERR_INVAL;
    const char *leaf = found ? path + separator + 1 : path;
    size_t leaf_len = len - (size_t)(leaf - path);
    if (leaf_len == 0 || leaf_len > FAT32_NAME_MAX || strcmp(leaf, ".") == 0 ||
        strcmp(leaf, "..") == 0) return FS_ERR_BADNAME;
    char *parent = kmalloc(len + 2);
    if (parent == NULL) return FS_ERR_IO;
    if (!found) { parent[0] = '.'; parent[1] = 0; }
    else {
        size_t parent_len = path[separator] == ':' || separator == 0 ? separator + 1 : separator;
        memcpy(parent, path, parent_len);
        parent[parent_len] = 0;
    }
    fat32_dirent_t *entry = kmalloc(sizeof(*entry));
    if (entry == NULL) { kfree(parent); return FS_ERR_IO; }
    int rc = fs_resolve(cwd, parent, out, entry);
    if (rc == 0 && !entry->is_dir) rc = FS_ERR_NOTDIR;
    if (rc == 0) {
        size_t used = (size_t)strlen(out->path);
        if (used > 1) out->path[used++] = '/';
        if (used + leaf_len >= FS_PATH_MAX) rc = FS_ERR_INVAL;
        else memcpy(out->path + used, leaf, leaf_len + 1);
    }
    kfree(entry);
    kfree(parent);
    return rc;
}

int fs_open_flags(const fs_cwd_t *cwd, const char *path, uint32_t flags, fat32_file_t *out) {
    fs_cwd_t *resolved = kmalloc(sizeof(*resolved));
    if (resolved == NULL) return FS_ERR_IO;
    int rc = flags & FAT32_O_CREAT ? resolve_new_leaf(cwd, path, resolved) :
                                    fs_resolve(cwd, path, resolved, NULL);
    if (rc == 0) rc = fat32_open_flags(resolved->volume, resolved->path, flags, out);
    kfree(resolved);
    return rc;
}

static int mutate_resolved(const fs_cwd_t *cwd, const char *path, unsigned operation) {
    fs_cwd_t *resolved = kmalloc(sizeof(*resolved));
    if (resolved == NULL) return FS_ERR_IO;
    int rc = operation == 1 ? resolve_new_leaf(cwd, path, resolved) :
                             fs_resolve(cwd, path, resolved, NULL);
    if (rc == 0) {
        if (operation == 0) rc = fat32_unlink(resolved->volume, resolved->path);
        else if (operation == 1) rc = fat32_mkdir(resolved->volume, resolved->path);
        else rc = fat32_rmdir(resolved->volume, resolved->path);
    }
    kfree(resolved);
    return rc;
}

int fs_unlink(const fs_cwd_t *cwd, const char *path) { return mutate_resolved(cwd, path, 0); }
int fs_mkdir(const fs_cwd_t *cwd, const char *path) { return mutate_resolved(cwd, path, 1); }
int fs_rmdir(const fs_cwd_t *cwd, const char *path) { return mutate_resolved(cwd, path, 2); }

int fs_rename(const fs_cwd_t *cwd, const char *source, const char *destination) {
    struct rename_locations {
        fs_cwd_t source;
        fs_cwd_t destination;
    } *s = kmalloc(sizeof(*s));
    if (s == NULL) return FS_ERR_IO;
    int rc = fs_resolve(cwd, source, &s->source, NULL);
    if (rc == 0) rc = resolve_new_leaf(cwd, destination, &s->destination);
    if (rc == 0 && s->source.volume != s->destination.volume) rc = FS_ERR_XDEV;
    if (rc == 0) rc = fat32_rename(s->source.volume, s->source.path, s->destination.path);
    kfree(s);
    return rc;
}

int fs_sync_all(void) {
    int error = 0;
    for (unsigned i = 0; i < volume_count; i++) {
        int rc = fat32_sync_volume(volumes[i].volume);
        if (rc != 0 && error == 0) error = rc;
    }
    return error;
}
