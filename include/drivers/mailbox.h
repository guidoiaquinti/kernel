#pragma once

#include "common.h"

#define MAILBOX_CHANNEL_PROPERTY 8

// `buffer` must be 16-byte aligned and coherent with the VideoCore (e.g. from
// coherent_page()): the call does no cache maintenance.
int mailbox_call(uint8_t channel, volatile uint32_t *buffer);

// Clock ids for mailbox_clock_query (the firmware's property interface).
#define MAILBOX_CLOCK_EMMC 1
#define MAILBOX_CLOCK_EMMC2 12

// Property tags for mailbox_clock_query.
#define MAILBOX_TAG_GET_CLOCK_RATE 0x00030002
#define MAILBOX_TAG_GET_CLOCK_RATE_MEASURED 0x00030047

// What a clock query brought back, for diagnostics as well as the rate.
struct mailbox_clock_reply {
    bool delivered;      // the VideoCore answered the mailbox call
    uint32_t response;   // the buffer's response code (0x80000000: success)
    uint32_t tag_flags;  // the tag's response length word (bit 31: answered)
    uint32_t id;         // the clock id it answered for
    uint32_t rate;       // Hz; 0 if the firmware does not know
};

// Ask the firmware for clock `id` with `tag` (one of the two above). Returns
// the rate in Hz, or 0; *reply (may be NULL) gets the details. Uses its own
// coherent page; callers are serialized by the BKL.
uint32_t mailbox_clock_query(uint32_t tag, uint32_t id, struct mailbox_clock_reply *reply);


// The ARM's share of the first gigabyte of RAM (the GPU's memory follows it):
// false if the firmware did not answer. Same coherent page and rules as above.
bool mailbox_arm_memory(uint32_t *base, uint32_t *size);

// The board revision code (its RAM size among other things), or 0.
uint32_t mailbox_board_revision(void);
