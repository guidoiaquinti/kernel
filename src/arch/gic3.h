// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GICv3 behind intc.h on QEMU virt (src/arch/irq_gic3.c), private to
// src/arch; irq_gic.c calls these when the device tree has a GICv3.
void gic3_init(void);
void gic3_init_cpu(void);
void gic3_enable(unsigned irq);
void gic3_disable(unsigned irq);
void gic3_handle(void);

// Shared with the GICv2 code: make the virtio-mmio SPIs the device tree marks
// rising-edge edge-triggered in GICD_ICFGR (same layout in both versions).
void gic_virt_edge_triggers(void);
