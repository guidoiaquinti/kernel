// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/virt.h"

#define FDT_MAGIC 0xd00dfeedu
#define FDT_MAX_SIZE (2 * 1024 * 1024)
#define FDT_MAX_DEPTH 16
#define VIRT_RAM_BASE 0x40000000UL
#define VIRT_ADDRESS_LIMIT 0x100000000UL

static struct virt_platform platform;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool string_is(const uint8_t *p, size_t len, const char *s) {
    size_t i = 0;
    while (s[i] && i < len && p[i] == (uint8_t)s[i]) {
        i++;
    }
    return !s[i] && i < len && !p[i];
}

static bool compatible(const uint8_t *p, size_t len, const char *s) {
    for (size_t i = 0; i < len;) {
        if (string_is(p + i, len - i, s)) {
            return true;
        }
        while (i < len && p[i]) {
            i++;
        }
        i++;
    }
    return false;
}

static bool span(size_t off, size_t len, size_t bound) {
    return off <= bound && len <= bound - off;
}

static bool address_cells(const uint8_t *p, unsigned n, uint64_t *out) {
    if (n != 1 && n != 2) {
        return false;
    }
    *out = be32(p);
    if (n == 2) {
        *out = (*out << 32) | be32(p + 4);
    }
    return true;
}

struct node {
    unsigned address_cells, size_cells;
    unsigned parent_address_cells, parent_size_cells;
    const uint8_t *reg;
    size_t reg_len;
    const uint8_t *interrupts;
    size_t interrupts_len;
    bool memory, uart, gic, fw_cfg, virtio, disabled, translated_bus;
    bool reserved_children, reserved;
    bool cpu, psci;
    enum virt_psci_method method;
};

static bool reg_region(const struct node *n, unsigned index,
                       uintptr_t *base, size_t *size) {
    unsigned cells = n->parent_address_cells + n->parent_size_cells;
    size_t stride = cells * 4;
    if (!stride || !span((size_t)index * stride, stride, n->reg_len)) {
        return false;
    }
    const uint8_t *p = n->reg + index * stride;
    uint64_t addr, bytes;
    if (!address_cells(p, n->parent_address_cells, &addr) ||
        !address_cells(p + n->parent_address_cells * 4, n->parent_size_cells, &bytes) ||
        !bytes || addr >= VIRT_ADDRESS_LIMIT || bytes > VIRT_ADDRESS_LIMIT - addr) {
        return false;
    }
    *base = addr;
    *size = bytes;
    return true;
}

static bool gic_irq(const struct node *n, unsigned *irq, bool *edge) {
    if (n->interrupts_len < 12) {
        return false;
    }
    unsigned type = be32(n->interrupts);
    unsigned number = be32(n->interrupts + 4);
    unsigned flags = be32(n->interrupts + 8) & 15;
    // GICv2 supports active-high level and rising-edge interrupt lines.
    if ((type != 0 && type != 1) || (flags != 4 && flags != 1) ||
        (type == 1 && number >= 16) || number >= 224) {
        return false;
    }
    *irq = number + (type == 0 ? 32 : 16);
    *edge = flags == 1;
    return true;
}

// A /cpus/cpu@N node: its reg is the core's MPIDR affinity, in the parent's
// #address-cells (1 or 2) with no size.
static bool finish_cpu(const struct node *n, struct virt_platform *out) {
    unsigned cells = n->parent_address_cells;
    if ((cells != 1 && cells != 2) || n->reg_len < cells * 4u) {
        return false;
    }
    uint64_t mpidr = be32(n->reg);
    if (cells == 2) {
        mpidr = mpidr << 32 | be32(n->reg + 4);
    }
    if (out->cpu_count < VIRT_MAX_CPUS) {
        out->cpu_mpidr[out->cpu_count] = mpidr;
    }
    out->cpu_count++;
    return true;
}

static bool finish_node(const struct node *n, unsigned depth, struct virt_platform *out) {
    if (n->disabled) {
        return true;
    }
    if (n->cpu) {
        return depth == 3 ? finish_cpu(n, out) : false;
    }
    if (n->psci) {
        out->psci_method = n->method;
        return true;
    }
    if (!(n->memory || n->uart || n->gic || n->fw_cfg || n->virtio || n->reserved)) {
        return true;
    }
    if (n->translated_bus || (!n->reserved && depth != 2)) {
        return false; // QEMU virt devices live directly on the root bus.
    }
    uintptr_t base;
    size_t size;
    if (!reg_region(n, 0, &base, &size)) {
        return false;
    }
    if (!n->memory && !n->reserved && (base >= VIRT_RAM_BASE || (base & 3))) {
        return false; // Supported virt MMIO is aligned and below the RAM window.
    }
    if (n->reserved) {
        unsigned stride = (n->parent_address_cells + n->parent_size_cells) * 4;
        if (!stride || n->reg_len % stride) {
            return false;
        }
        for (unsigned i = 0; i < n->reg_len / stride; i++) {
            if (out->reserved_count == VIRT_MAX_RESERVED ||
                !reg_region(n, i, &base, &size)) {
                return false;
            }
            out->reserved[out->reserved_count++] = (struct virt_reserved_region){base, size};
        }
    } else if (n->memory) {
        if (out->ram_size || base != VIRT_RAM_BASE || (base | size) & 0x1fffff) {
            return false;
        }
        out->ram_base = base;
        out->ram_size = size;
    } else if (n->gic) {
        uintptr_t cpu;
        size_t cpu_size;
        if (out->gic_dist || size < 0x1000 ||
            !reg_region(n, 1, &cpu, &cpu_size) || cpu_size < 0x1000 ||
            cpu >= VIRT_RAM_BASE || (cpu & 0xfff) || (base & 0xfff)) {
            return false;
        }
        out->gic_dist = base;
        out->gic_cpu = cpu;
    } else if (n->fw_cfg) {
        if (out->fw_cfg_base || size < 0x18) {
            return false;
        }
        out->fw_cfg_base = base;
    } else {
        unsigned irq;
        bool edge;
        if (!gic_irq(n, &irq, &edge) || size < 0x200) {
            return false;
        }
        if (n->uart) {
            if (out->uart_base || edge) {
                return false;
            }
            out->uart_base = base;
            out->uart_irq = irq;
        } else {
            if (out->virtio_count == VIRT_MAX_VIRTIO) {
                return false;
            }
            out->virtio[out->virtio_count++] = (struct virt_mmio_device){base, irq, edge};
        }
    }
    return true;
}

bool virt_platform_parse(const void *dtb, size_t available) {
    // Do not leave a partly discovered machine visible after failure.
    platform = (struct virt_platform){0};
    const uint8_t *blob = dtb;
    if (!blob || available < 40 || be32(blob) != FDT_MAGIC) {
        return false;
    }
    size_t total = be32(blob + 4), structure = be32(blob + 8);
    size_t strings = be32(blob + 12), reserve = be32(blob + 16);
    size_t strings_size = be32(blob + 32), structure_size = be32(blob + 36);
    unsigned version = be32(blob + 20), last_version = be32(blob + 24);
    if (total < 40 || total > available || total > FDT_MAX_SIZE || version < 17 ||
        last_version > 17 || structure < 40 || strings < 40 || reserve < 40 ||
        (structure & 3) || (reserve & 7) || !span(structure, structure_size, total) ||
        !span(strings, strings_size, total)) {
        return false;
    }
    struct virt_platform out = {0};
    // Check and retain the bootloader-reserved regions before using RAM.
    size_t r = reserve;
    while (span(r, 16, total)) {
        if (!(be32(blob + r) | be32(blob + r + 4) |
              be32(blob + r + 8) | be32(blob + r + 12))) {
            break;
        }
        uint64_t base = ((uint64_t)be32(blob + r) << 32) | be32(blob + r + 4);
        uint64_t size = ((uint64_t)be32(blob + r + 8) << 32) | be32(blob + r + 12);
        if (!size || base > UINT64_MAX - size || out.reserved_count == VIRT_MAX_RESERVED) {
            return false;
        }
        out.reserved[out.reserved_count++] = (struct virt_reserved_region){base, size};
        r += 16;
    }
    if (!span(r, 16, total)) {
        return false;
    }
    struct node stack[FDT_MAX_DEPTH];
    unsigned depth = 0;
    bool root_seen = false, root_closed = false, virt = false;
    size_t pos = structure, end = structure + structure_size;
    while (span(pos, 4, end)) {
        uint32_t token = be32(blob + pos);
        pos += 4;
        if (token == 1) { // BEGIN_NODE
            if (depth == FDT_MAX_DEPTH || root_closed) {
                return false;
            }
            size_t name = pos;
            while (pos < end && blob[pos]) {
                pos++;
            }
            if (pos == end || (!depth && (root_seen || pos != name))) {
                return false;
            }
            pos = (pos + 4) & ~(size_t)3;
            struct node n = {0};
            n.address_cells = n.size_cells = 2;
            if (depth) {
                n.parent_address_cells = n.address_cells = stack[depth - 1].address_cells;
                n.parent_size_cells = n.size_cells = stack[depth - 1].size_cells;
                n.translated_bus = stack[depth - 1].translated_bus;
                n.disabled = stack[depth - 1].disabled;
                n.reserved = stack[depth - 1].reserved_children;
            }
            n.reserved_children = depth == 1 &&
                string_is(blob + name, pos - name, "reserved-memory");
            stack[depth++] = n;
            root_seen = true;
        } else if (token == 2) { // END_NODE
            if (!depth || !finish_node(&stack[depth - 1], depth, &out)) {
                return false;
            }
            depth--;
            if (!depth) {
                root_closed = true;
            }
        } else if (token == 3) { // PROP
            if (!depth || !span(pos, 8, end)) {
                return false;
            }
            size_t len = be32(blob + pos), name = be32(blob + pos + 4);
            pos += 8;
            if (!span(pos, len, end) || name >= strings_size) {
                return false;
            }
            const uint8_t *key = blob + strings + name, *value = blob + pos;
            size_t key_len = strings_size - name;
            size_t k = 0;
            while (k < key_len && key[k]) {
                k++;
            }
            if (k == key_len) {
                return false;
            }
            struct node *n = &stack[depth - 1];
            if (string_is(key, key_len, "#address-cells") ||
                string_is(key, key_len, "#size-cells")) {
                if (len != 4 || be32(value) > 4) {
                    return false;
                }
                if (key[1] == 'a') {
                    n->address_cells = be32(value);
                } else {
                    n->size_cells = be32(value);
                }
            } else if (string_is(key, key_len, "compatible")) {
                if (!len || value[len - 1]) {
                    return false;
                }
                if (depth == 1) {
                    virt = compatible(value, len, "linux,dummy-virt");
                }
                n->uart = compatible(value, len, "arm,pl011");
                n->gic = compatible(value, len, "arm,cortex-a15-gic") ||
                         compatible(value, len, "arm,gic-400");
                n->fw_cfg = compatible(value, len, "qemu,fw-cfg-mmio");
                n->virtio = compatible(value, len, "virtio,mmio");
                n->psci = compatible(value, len, "arm,psci-0.2") ||
                          compatible(value, len, "arm,psci-1.0");
            } else if (string_is(key, key_len, "method")) {
                n->method = string_is(value, len, "hvc")   ? VIRT_PSCI_HVC
                            : string_is(value, len, "smc") ? VIRT_PSCI_SMC
                                                           : VIRT_PSCI_NONE;
            } else if (string_is(key, key_len, "device_type")) {
                n->memory = string_is(value, len, "memory");
                n->cpu = string_is(value, len, "cpu");
            } else if (string_is(key, key_len, "reg")) {
                n->reg = value;
                n->reg_len = len;
            } else if (string_is(key, key_len, "interrupts")) {
                n->interrupts = value;
                n->interrupts_len = len;
            } else if (string_is(key, key_len, "status")) {
                n->disabled = n->disabled || (!string_is(value, len, "okay") &&
                              !string_is(value, len, "ok"));
            } else if (string_is(key, key_len, "ranges") && len) {
                n->translated_bus = true;
            }
            pos = (pos + len + 3) & ~(size_t)3;
        } else if (token == 4) { // NOP
            continue;
        } else if (token == 9) { // END
            if (depth || !root_closed || !virt || !out.ram_size ||
                !out.uart_base || !out.gic_dist) {
                return false;
            }
            out.dtb_base = (uintptr_t)blob;
            out.dtb_size = total;
            platform = out;
            return true;
        } else {
            return false;
        }
    }
    return false;
}

bool virt_platform_init(uintptr_t dtb) {
    platform = (struct virt_platform){0};
    if (!dtb) {
        dtb = VIRT_RAM_BASE;
    }
    if (dtb < VIRT_RAM_BASE || dtb > VIRT_ADDRESS_LIMIT - FDT_MAX_SIZE || (dtb & 7)) {
        return false;
    }
    if (!virt_platform_parse((const void *)dtb, FDT_MAX_SIZE)) {
        return false;
    }
    uintptr_t ram_end = platform.ram_base + platform.ram_size;
    bool valid = dtb >= platform.ram_base && dtb < ram_end &&
                 platform.dtb_size <= ram_end - dtb;
#ifdef KORAOS_VIRT
    extern char kernel_start[], _end[];
    valid = valid && (uintptr_t)kernel_start >= platform.ram_base &&
            (uintptr_t)_end <= ram_end;
#endif
    if (!valid) {
        platform = (struct virt_platform){0};
    }
    return valid;
}

const struct virt_platform *virt_platform_get(void) {
    return &platform;
}
