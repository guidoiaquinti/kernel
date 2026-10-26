#include "drivers/mailbox.h"
#include "mm/coherent.h"
#include "peripherals/mailbox.h"

static inline uint32_t arm_to_bus(uintptr_t addr) {
#ifdef QEMU_TESTING
    return (uint32_t)addr;
#else
    // The VideoCore's uncached alias (Pi 2/3/4). The 0x4 alias would route its
    // accesses through the VC L2, which the ARM does not see.
    return (uint32_t)(addr | 0xC0000000u);
#endif
}

static inline uint32_t mailbox_make_request(uint8_t channel, volatile uint32_t *buffer) {
    uint32_t addr = arm_to_bus((uintptr_t)buffer);
    return (uint32_t)((addr & ~0xFu) | (channel & 0xFu));
}

int mailbox_call(uint8_t channel, volatile uint32_t *buffer) {
    uint32_t request = mailbox_make_request(channel, buffer);
    uint32_t timeout = 0x100000;

    while ((REGS_MAILBOX->status & MAILBOX_STATUS_FULL) && timeout--) {
        // Wait for space
    }
    if (timeout == 0) {
        return 0;
    }
    // The buffer is Normal (non-cacheable) memory: make sure its writes have
    // landed before the VideoCore is told to read it.
    asm volatile("dsb sy" ::: "memory");
    REGS_MAILBOX->write = request;

    timeout = 0x100000;
    while (timeout--) {
        while ((REGS_MAILBOX->status & MAILBOX_STATUS_EMPTY) && timeout--) {
            // Wait for response
        }
        if (timeout == 0) {
            return 0;
        }
        uint32_t response = REGS_MAILBOX->read;
        if (response == request) {
            asm volatile("dsb sy" ::: "memory");  // read the reply after it lands
            return buffer[1] == 0x80000000;
        }
    }

    return 0;
}

#define MAILBOX_REQUEST 0
#define MAILBOX_RESPONSE_OK 0x80000000u

uint32_t mailbox_clock_query(uint32_t tag, uint32_t id, struct mailbox_clock_reply *reply) {
    volatile uint32_t *msg = coherent_page(COHERENT_SLOT_KORA_PROPERTY);
    unsigned i = 0;
    msg[i++] = 0;  // total size, below
    msg[i++] = MAILBOX_REQUEST;
    msg[i++] = tag;
    msg[i++] = 8;  // value buffer: clock id, rate
    msg[i++] = 0;
    msg[i++] = id;
    msg[i++] = 0;
    msg[i++] = 0;  // end tag
    msg[0] = i * 4;
    struct mailbox_clock_reply r = {0};
    r.delivered = mailbox_call(MAILBOX_CHANNEL_PROPERTY, msg) != 0;
    r.response = msg[1];
    r.tag_flags = msg[4];
    r.id = msg[5];
    r.rate = msg[6];
    if (reply != NULL) {
        *reply = r;
    }
    bool ok = r.delivered && r.response == MAILBOX_RESPONSE_OK && (r.tag_flags & 0x80000000u) &&
              r.id == id;
    return ok ? r.rate : 0;
}
