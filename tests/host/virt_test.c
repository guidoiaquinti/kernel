// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#include "platform/virt.h"

static uint8_t blob[8192];
static uint8_t names[2048];
static size_t pos, name_pos, irq_offset, ram_size_offset, psci_method_offset;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void word(uint32_t v) {
    put32(blob + pos, v);
    pos += 4;
}

static size_t length(const char *s) {
    size_t n = 0;
    while (s[n]) { n++; }
    return n;
}

static void begin(const char *s) {
    word(1);
    do { blob[pos++] = *s; } while (*s++);
    while (pos & 3) { blob[pos++] = 0; }
}

static size_t prop(const char *name, const void *data, size_t bytes) {
    size_t n = name_pos;
    do { names[name_pos++] = *name; } while (*name++);
    word(3); word(bytes); word(n);
    size_t value = pos;
    for (size_t i = 0; i < bytes; i++) { blob[pos++] = ((const uint8_t *)data)[i]; }
    while (pos & 3) { blob[pos++] = 0; }
    return value;
}

static void text(const char *name, const char *value) {
    prop(name, value, length(value) + 1);
}

static void cell(const char *name, uint32_t value) {
    uint8_t data[4]; put32(data, value); prop(name, data, sizeof(data));
}

static void device(const char *name, const char *compat, uint32_t addr, unsigned irq) {
    uint8_t reg[16] = {0}, interrupt[12] = {0};
    put32(reg + 4, addr); put32(reg + 12, 0x1000);
    put32(interrupt + 4, irq); put32(interrupt + 8, 4);
    begin(name); text("compatible", compat); prop("reg", reg, sizeof(reg));
    irq_offset = prop("interrupts", interrupt, sizeof(interrupt)); word(2);
}

static size_t fixture(unsigned virtio_count) {
    for (size_t i = 0; i < sizeof(blob); i++) { blob[i] = 0; }
    pos = 72; name_pos = 0;
    put32(blob + 44, 0x41000000); put32(blob + 52, 0x1000);
    begin(""); text("compatible", "linux,dummy-virt");
    cell("#address-cells", 2); cell("#size-cells", 2);
    // PCI uses three address cells even though this milestone does not drive it.
    begin("pcie@10000000"); text("compatible", "pci-host-ecam-generic");
    cell("#address-cells", 3); cell("#size-cells", 2); word(2);
    begin("memory@40000000"); text("device_type", "memory");
    uint8_t memory[16] = {0}; put32(memory + 4, 0x40000000); put32(memory + 12, 0x08000000);
    ram_size_offset = prop("reg", memory, sizeof(memory)) + 12; word(2);
    begin("intc@8100000"); text("compatible", "arm,cortex-a15-gic");
    uint8_t gic[32] = {0};
    put32(gic + 4, 0x08100000); put32(gic + 12, 0x10000);
    put32(gic + 20, 0x08200000); put32(gic + 28, 0x10000);
    prop("reg", gic, sizeof(gic)); word(2);
    device("uart@9100000", "arm,pl011", 0x09100000, 2);
    // fw_cfg requires no IRQ, unlike the transport nodes.
    begin("fw-cfg@9020000"); text("compatible", "qemu,fw-cfg-mmio");
    uint8_t fw[16] = {0}; put32(fw + 4, 0x09020000); put32(fw + 12, 0x18);
    prop("reg", fw, sizeof(fw)); word(2);
    for (unsigned i = 0; i < virtio_count; i++) {
        device("virtio_mmio", "virtio,mmio", 0x0a000000 + 0x1000 * i, 16 + i);
    }
    // Two cores (MPIDR 0 and 1) brought up through PSCI over HVC, as QEMU
    // describes them without EL2.
    begin("cpus"); cell("#address-cells", 1); cell("#size-cells", 0);
    begin("cpu@0"); text("device_type", "cpu"); text("compatible", "arm,cortex-a72");
    cell("reg", 0); text("enable-method", "psci"); word(2);
    begin("cpu@1"); text("device_type", "cpu"); text("compatible", "arm,cortex-a72");
    cell("reg", 1); text("enable-method", "psci"); word(2);
    word(2);
    begin("psci");
    prop("compatible", "arm,psci-1.0\0arm,psci-0.2\0arm,psci", 35);
    psci_method_offset = prop("method", "hvc", 4); word(2);
    begin("reserved-memory"); cell("#address-cells", 2); cell("#size-cells", 2);
    prop("ranges", NULL, 0); begin("test@41234000");
    uint8_t reserved[32] = {0};
    put32(reserved + 4, 0x41234000); put32(reserved + 12, 0x2000);
    put32(reserved + 20, 0x41300000); put32(reserved + 28, 0x3000);
    prop("reg", reserved, sizeof(reserved)); word(2); word(2);
    word(2); word(9);
    size_t structure_size = pos - 72, strings = pos;
    for (size_t i = 0; i < name_pos; i++) { blob[pos++] = names[i]; }
    put32(blob, 0xd00dfeed); put32(blob + 4, pos);
    put32(blob + 8, 72); put32(blob + 12, strings); put32(blob + 16, 40);
    put32(blob + 20, 17); put32(blob + 24, 16);
    put32(blob + 32, name_pos); put32(blob + 36, structure_size);
    return pos;
}

static void discovery(void) {
    size_t size = fixture(32);
    CHECK(virt_platform_parse(blob, size), "valid virt DTB rejected");
    const struct virt_platform *p = virt_platform_get();
    CHECK(p->ram_base == 0x40000000 && p->ram_size == 0x08000000, "RAM discovery");
    CHECK(p->uart_base == 0x09100000 && p->uart_irq == 34, "dynamic UART discovery");
    CHECK(p->gic_dist == 0x08100000 && p->gic_cpu == 0x08200000, "dynamic GIC discovery");
    CHECK(p->fw_cfg_base == 0x09020000, "fw_cfg discovery");
    CHECK(p->reserved_count == 3 && p->reserved[0].base == 0x41000000 &&
          p->reserved[1].base == 0x41234000 && p->reserved[1].size == 0x2000 &&
          p->reserved[2].base == 0x41300000 && p->reserved[2].size == 0x3000,
          "reserve map and reserved-memory discovery");
    CHECK(p->virtio_count == 32 && p->virtio[31].irq == 79, "all virtio slots discovered");
    CHECK(p->dtb_base == (uintptr_t)blob && p->dtb_size == size, "DTB reservation metadata");
    CHECK(p->cpu_count == 2 && p->cpu_mpidr[0] == 0 && p->cpu_mpidr[1] == 1,
          "CPU discovery (%u cores)", p->cpu_count);
    CHECK(p->psci_method == VIRT_PSCI_HVC, "PSCI method hvc");
    blob[psci_method_offset] = 's'; blob[psci_method_offset + 1] = 'm';
    CHECK(virt_platform_parse(blob, size) && virt_platform_get()->psci_method == VIRT_PSCI_SMC,
          "PSCI method smc");
    blob[psci_method_offset] = 'h'; blob[psci_method_offset + 1] = 'v';
    put32(blob + irq_offset + 8, 1);
    CHECK(virt_platform_parse(blob, size), "rising-edge virtio IRQ rejected");
    CHECK(virt_platform_get()->virtio[31].edge_triggered, "edge IRQ type lost");
}

static void malformed(void) {
    size_t size = fixture(0);
    CHECK(!virt_platform_parse(NULL, size), "NULL DTB accepted");
    CHECK(!virt_platform_parse(blob, 39), "truncated header accepted");
    CHECK(!virt_platform_parse(blob, size - 1), "truncated blob accepted");
    put32(blob, 0);
    CHECK(!virt_platform_parse(blob, size), "invalid magic accepted");
    size = fixture(0); put32(blob + 8, 0xfffffffc);
    CHECK(!virt_platform_parse(blob, size), "overflowing structure accepted");
    size = fixture(0); put32(blob + 32, 0xffffffff);
    CHECK(!virt_platform_parse(blob, size), "overflowing strings accepted");
    size = fixture(0); put32(blob + 16, size - 8);
    CHECK(!virt_platform_parse(blob, size), "truncated reservation table accepted");
    size = fixture(0); put32(blob + 40, 0xffffffff); put32(blob + 44, 0xffffffff);
    CHECK(!virt_platform_parse(blob, size), "overflowing reserved extent accepted");
    size = fixture(0); put32(blob + irq_offset + 4, 224);
    CHECK(!virt_platform_parse(blob, size), "out-of-range interrupt accepted");
    size = fixture(0); put32(blob + irq_offset + 8, 2);
    CHECK(!virt_platform_parse(blob, size), "unsupported falling-edge IRQ accepted");
    size = fixture(0); put32(blob + ram_size_offset, 0x08000001);
    CHECK(!virt_platform_parse(blob, size), "unaligned RAM accepted");
    size = fixture(33);
    CHECK(!virt_platform_parse(blob, size), "transport array overflow accepted");
    CHECK(!virt_platform_get()->ram_size && !virt_platform_get()->uart_base,
          "failed parse exposed partial state");
}

static void truncations(void) {
    size_t size = fixture(2);
    for (size_t i = 0; i < size; i++) {
        CHECK(!virt_platform_parse(blob, i), "truncation %lu accepted", i);
    }
}

TEST_MAIN(discovery, malformed, truncations)
