#pragma once

#include "common.h"

#define MAILBOX_CHANNEL_PROPERTY 8

// `buffer` must be 16-byte aligned and coherent with the VideoCore (e.g. from
// coherent_page()): the call does no cache maintenance.
int mailbox_call(uint8_t channel, volatile uint32_t *buffer);

// Clock ids for mailbox_clock_rate (the firmware's property interface).
#define MAILBOX_CLOCK_EMMC 1
#define MAILBOX_CLOCK_EMMC2 12

// The firmware's current rate of clock `id` in Hz, or 0 if it does not say.
// Uses its own coherent page; callers are serialized by the BKL.
uint32_t mailbox_clock_rate(uint32_t id);

