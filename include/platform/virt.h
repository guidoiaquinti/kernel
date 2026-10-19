// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

#define VIRT_MAX_VIRTIO 32
#define VIRT_MAX_RESERVED 16

struct virt_reserved_region {
    uintptr_t base;
    size_t size;
};

struct virt_mmio_device {
    uintptr_t base;
    unsigned irq;
    bool edge_triggered;
};

#define VIRT_MAX_CPUS 8

// How to reach PSCI firmware calls (CPU_ON), from the /psci node's method.
enum virt_psci_method { VIRT_PSCI_NONE = 0, VIRT_PSCI_HVC, VIRT_PSCI_SMC };

struct virt_platform {
    uintptr_t ram_base;
    size_t ram_size;
    uintptr_t dtb_base;
    size_t dtb_size;
    uintptr_t uart_base;
    unsigned uart_irq;
    unsigned gic_version;                  // 2 (GICv2) or 3 (GICv3)
    uintptr_t gic_dist;
    uintptr_t gic_cpu;                     // GICv2: CPU interface
    uintptr_t gic_redist;                  // GICv3: redistributor region
    size_t gic_redist_size;
    uintptr_t fw_cfg_base;
    unsigned reserved_count;
    struct virt_reserved_region reserved[VIRT_MAX_RESERVED];
    unsigned virtio_count;
    struct virt_mmio_device virtio[VIRT_MAX_VIRTIO];
    unsigned cpu_count;                    // /cpus/cpu@N nodes, in DTB order
    uint64_t cpu_mpidr[VIRT_MAX_CPUS];     // their MPIDR affinity (reg)
    enum virt_psci_method psci_method;
};

// Direct-kernel boot passes the DTB in x0; ELF boot places it at RAM base.
bool virt_platform_init(uintptr_t dtb);
// Pure parser with an explicit readable bound, also used by host tests.
bool virt_platform_parse(const void *dtb, size_t available);
const struct virt_platform *virt_platform_get(void);
