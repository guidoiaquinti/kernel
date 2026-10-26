// SPDX-License-Identifier: GPL-3.0-or-later
//
// SD card through the SDHCI controller, polled. See drivers/sdhci.h.
//
// The Pi 3's card slot sits on the Arasan controller (once its pins are moved
// over from SDHOST); the Pi 4's on EMMC2, a standard SDHCI 3.0 controller
// wired to the slot, with its own clock. Both speak the same registers.
//
// The BCM2835 Arasan controller accepts only 32-bit register accesses, so the
// 16-bit SDHCI registers are written in their 32-bit pairs (block size with
// block count, transfer mode with command), and it needs two card clock cycles
// between writes, which matters at the 400 kHz identification clock.
//
// Bring-up follows the SD Physical Layer Simplified Specification: CMD0, CMD8
// (2.0 cards), ACMD41 until ready (with HCS: high capacity is welcome), CMD2,
// CMD3 for the relative address, CMD9 for the capacity, CMD7 to select it,
// CMD16 for 512-byte blocks on standard-capacity cards, ACMD6 for the 4-bit
// bus, then the 25 MHz default-speed clock.

#include "drivers/sdhci.h"

#ifdef KORAOS_SD

#include "drivers/mailbox.h"
#include "drivers/sd.h"
#include "gpio.h"
#include "lib/printf.h"
#include "lib/timer.h"
#include "peripherals/base.h"

#if RPI_VERSION == 4
#define SDHCI_BASE (PBASE + 0x00340000)  // EMMC2
#define SDHCI_CLOCK MAILBOX_CLOCK_EMMC2
#else
#define SDHCI_BASE (PBASE + 0x00300000)  // Arasan "EMMC"
#define SDHCI_CLOCK MAILBOX_CLOCK_EMMC
#endif

// Registers (32-bit offsets).
#define ARG2 0x00
#define BLKSIZECNT 0x04
#define ARG1 0x08
#define CMDTM 0x0c
#define RESP0 0x10
#define DATA 0x20
#define STATUS 0x24
#define CONTROL0 0x28
#define CONTROL1 0x2c
#define INTERRUPT 0x30
#define IRPT_MASK 0x34
#define IRPT_EN 0x38
#define CAPABILITIES 0x40
#define SLOTISR_VER 0xfc

// STATUS (present state).
#define ST_CMD_INHIBIT (1u << 0)
#define ST_DAT_INHIBIT (1u << 1)

// CONTROL0 (host control, power control).
#define C0_HCTL_DWIDTH (1u << 1)   // 4-bit data bus
#define C0_POWER_33V (0xfu << 8)   // bus power on at 3.3 V

// CONTROL1 (clock control, timeout, software reset).
#define C1_CLK_INTLEN (1u << 0)
#define C1_CLK_STABLE (1u << 1)
#define C1_CLK_EN (1u << 2)
#define C1_DIVISOR_MASK 0xffc0u
#define C1_DATA_TOUNIT (0xeu << 16)  // data timeout: the longest
#define C1_SRST_HC (1u << 24)
#define C1_SRST_CMD (1u << 25)
#define C1_SRST_DATA (1u << 26)

// INTERRUPT.
#define INT_CMD_DONE (1u << 0)
#define INT_DATA_DONE (1u << 1)
#define INT_WRITE_RDY (1u << 4)
#define INT_READ_RDY (1u << 5)
#define INT_ERR (1u << 15)
#define INT_ERR_MASK 0xffff0000u
#define INT_CMD_TIMEOUT (1u << 16)

// CMDTM: transfer mode (low half) and command (high half).
#define TM_BLKCNT_EN (1u << 1)
#define TM_AUTO_CMD12 (1u << 2)
#define TM_DAT_DIR_READ (1u << 4)
#define TM_MULTI_BLOCK (1u << 5)
#define CMD_RSPNS_136 (1u << 16)
#define CMD_RSPNS_48 (2u << 16)
#define CMD_RSPNS_48B (3u << 16)
#define CMD_CRCCHK_EN (1u << 19)
#define CMD_IXCHK_EN (1u << 20)
#define CMD_ISDATA (1u << 21)
#define CMD_INDEX(n) ((uint32_t)(n) << 24)

// Response kinds, as CMDTM bits.
#define R_NONE 0u
#define R1 (CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define R1B (CMD_RSPNS_48B | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define R2 (CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define R3 CMD_RSPNS_48  // OCR: no CRC, no index
#define R6 R1
#define R7 R1

#define APP_CMD 0x100u  // flag for app_command: CMD55 first

#define ID_CLOCK_HZ 400000u
#define DEFAULT_CLOCK_HZ 25000000u
#define MAX_BLOCKS 0xffffu  // the block count register's limit
#define BLOCK 512u

struct sd_card {
    uint32_t base_hz;         // the controller's base clock
    unsigned write_delay_us;  // two card clocks, rounded up
    uint32_t rca;             // relative card address, in bits 31:16
    bool high_capacity;       // block addressing (SDHC/SDXC)
    uint64_t sectors;
    blkdev_t dev;
};

static struct sd_card card;

static inline uint32_t rd(unsigned reg) {
    return *(volatile uint32_t *)(uintptr_t)(SDHCI_BASE + reg);
}

static inline void wr(unsigned reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(SDHCI_BASE + reg) = value;
    if (card.write_delay_us) {
        udelay(card.write_delay_us);
    }
}

// Poll until (reg & mask) == want, for at most timeout_us. True on success.
static bool wait_reg(unsigned reg, uint32_t mask, uint32_t want, uint64_t timeout_us) {
    uint64_t deadline = timer_us() + timeout_us;
    while ((rd(reg) & mask) != want) {
        if (timer_us() > deadline) {
            return (rd(reg) & mask) == want;
        }
    }
    return true;
}

// Reset the command or data circuit after an error.
static void reset_lines(uint32_t which) {
    wr(CONTROL1, rd(CONTROL1) | which);
    wait_reg(CONTROL1, which, 0, 100000);
}

// Wait for any of `bits` in INTERRUPT, or an error. Clears what it waited for.
// Returns 0, or -1 on error or timeout (the error bits are cleared too).
static int wait_interrupt(uint32_t bits, uint64_t timeout_us) {
    uint64_t deadline = timer_us() + timeout_us;
    for (;;) {
        uint32_t irq = rd(INTERRUPT);
        if (irq & (INT_ERR | INT_ERR_MASK)) {
            wr(INTERRUPT, irq);
            return -1;
        }
        if (irq & bits) {
            wr(INTERRUPT, irq & bits);
            return 0;
        }
        if (timer_us() > deadline) {
            return -1;
        }
    }
}

// Send command `index` with `arg`; `flags` holds the response kind and, for
// data commands, the transfer mode. resp gets the response (4 words for R2).
static int command(unsigned index, uint32_t arg, uint32_t flags, uint32_t resp[4]) {
    uint32_t inhibit = ST_CMD_INHIBIT | ((flags & CMD_ISDATA) || (flags & CMD_RSPNS_48B) == CMD_RSPNS_48B
                                             ? ST_DAT_INHIBIT : 0);
    if (!wait_reg(STATUS, inhibit, 0, 1000000)) {
        reset_lines(C1_SRST_CMD | C1_SRST_DATA);
        return -1;
    }
    wr(INTERRUPT, 0xffffffffu);
    wr(ARG1, arg);
    wr(CMDTM, CMD_INDEX(index) | flags);
    if (wait_interrupt(INT_CMD_DONE, 500000) != 0) {
        reset_lines(C1_SRST_CMD);
        return -1;
    }
    if (resp != NULL) {
        for (unsigned i = 0; i < 4; i++) {
            resp[i] = rd(RESP0 + 4 * i);
        }
    }
    if ((flags & CMD_RSPNS_48B) == CMD_RSPNS_48B && !(flags & CMD_ISDATA)) {
        // R1b: the card holds DAT0 low while busy.
        if (wait_interrupt(INT_DATA_DONE, 1000000) != 0) {
            reset_lines(C1_SRST_DATA);
            return -1;
        }
    }
    return 0;
}

// An application-specific command: CMD55 with the card's address first.
static int app_command(unsigned index, uint32_t arg, uint32_t flags, uint32_t resp[4]) {
    if (command(55, card.rca, R1, NULL) != 0) {
        return -1;
    }
    return command(index, arg, flags, resp);
}

static bool set_clock(uint32_t target_hz) {
    unsigned divisor = sdhci_clock_divisor(card.base_hz, target_hz);
    uint32_t hz = sdhci_clock_hz(card.base_hz, divisor);
    // Two card clocks between register writes (BCM2835): 5 us at 400 kHz.
    unsigned delay = (unsigned)((2000000u + hz - 1) / hz);
    if (delay > card.write_delay_us) {
        card.write_delay_us = delay;  // slow down before the clock does
    }
    uint32_t c1 = rd(CONTROL1) & ~(C1_CLK_EN | C1_DIVISOR_MASK);
    wr(CONTROL1, c1);
    c1 |= sdhci_divisor_bits(divisor) | C1_CLK_INTLEN | C1_DATA_TOUNIT;
    wr(CONTROL1, c1);
    if (!wait_reg(CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, 100000)) {
        return false;
    }
    wr(CONTROL1, c1 | C1_CLK_EN);
    udelay(2000);  // let the card see the new clock settle
    card.write_delay_us = delay;
    return true;
}

// Move a run of blocks; `write` selects the direction.
static int transfer(uint32_t lba, uint32_t count, void *buf, bool write) {
    uint32_t *words = buf;
    while (count > 0) {
        uint32_t n = count > MAX_BLOCKS ? MAX_BLOCKS : count;
        uint32_t addr = card.high_capacity ? lba : lba * BLOCK;
        uint32_t mode = TM_BLKCNT_EN | CMD_ISDATA | (write ? 0 : TM_DAT_DIR_READ);
        unsigned index = write ? 24 : 17;
        if (n > 1) {
            mode |= TM_MULTI_BLOCK | TM_AUTO_CMD12;
            index = write ? 25 : 18;
        }
        if (!wait_reg(STATUS, ST_DAT_INHIBIT, 0, 1000000)) {
            reset_lines(C1_SRST_DATA);
            return BLK_ERR_IO;
        }
        wr(BLKSIZECNT, (n << 16) | BLOCK);
        if (command(index, addr, R1 | mode, NULL) != 0) {
            return BLK_ERR_IO;
        }
        for (uint32_t b = 0; b < n; b++) {
            if (wait_interrupt(write ? INT_WRITE_RDY : INT_READ_RDY, 1000000) != 0) {
                reset_lines(C1_SRST_CMD | C1_SRST_DATA);
                return BLK_ERR_IO;
            }
            for (unsigned i = 0; i < BLOCK / 4; i++) {
                if (write) {
                    *(volatile uint32_t *)(uintptr_t)(SDHCI_BASE + DATA) = *words++;
                } else {
                    *words++ = rd(DATA);
                }
            }
        }
        // Transfer complete: for writes, also the end of the card's busy time.
        if (wait_interrupt(INT_DATA_DONE, write ? 5000000 : 1000000) != 0) {
            reset_lines(C1_SRST_CMD | C1_SRST_DATA);
            return BLK_ERR_IO;
        }
        lba += n;
        count -= n;
    }
    return 0;
}

static int sd_read(blkdev_t *dev, uint32_t lba, uint32_t count, void *buf) {
    (void)dev;
    return transfer(lba, count, buf, false);
}

#ifdef KORAOS_SD_WRITABLE
static int sd_write(blkdev_t *dev, uint32_t lba, uint32_t count, const void *buf) {
    (void)dev;
    return transfer(lba, count, (void *)buf, true);
}

// No write cache to flush; make sure the card has finished programming.
static int sd_flush(blkdev_t *dev) {
    (void)dev;
    uint32_t resp[4];
    uint64_t deadline = timer_us() + 1000000;
    do {
        if (command(13, card.rca, R1, resp) != 0) {
            return BLK_ERR_IO;
        }
        // READY_FOR_DATA, in the transfer state.
        if ((resp[0] & (1u << 8)) && ((resp[0] >> 9) & 0xf) == 4) {
            return 0;
        }
    } while (timer_us() < deadline);
    return BLK_ERR_IO;
}
#endif

// Pi 3: route GPIO 48-53 (the card slot) from SDHOST to the Arasan
// controller. It can also reach the WiFi chip's SDIO on GPIO 34-39; KoraOS has
// no WiFi driver, so those become plain inputs and the card has the bus to
// itself. The Pi 4's slot is wired to EMMC2: nothing to route.
static void route_pins(void) {
#if RPI_VERSION == 3
    for (uint8_t pin = 34; pin <= 39; pin++) {
        gpio_pin_set_func(pin, GFInput);
    }
    for (uint8_t pin = 48; pin <= 53; pin++) {
        gpio_pin_set_func(pin, GFAlt3);
        gpio_pin_set_pull(pin, pin == 48 ? GPNone : GPUp);  // CLK; CMD and DAT0-3
    }
#endif
}

// The controller's base clock. The firmware normally says (its current rate);
// if it does not, its measured rate, the controller's own claim or a
// conservative guess will do, and the log tells which and why.
static uint32_t base_clock(void) {
    const char *name = RPI_VERSION == 4 ? "EMMC2" : "EMMC";
    struct mailbox_clock_reply set, measured = {0};
    uint32_t set_hz = mailbox_clock_query(MAILBOX_TAG_GET_CLOCK_RATE, SDHCI_CLOCK, &set);
    uint32_t measured_hz = set_hz != 0 ? 0 : mailbox_clock_query(MAILBOX_TAG_GET_CLOCK_RATE_MEASURED,
                                                                  SDHCI_CLOCK, &measured);
    enum sd_clock_source source;
    uint32_t hz = sd_base_clock(set_hz, measured_hz, rd(CAPABILITIES), &source);
    if (source != SD_CLOCK_FIRMWARE) {
        printf("[sd] firmware gave no %s clock rate (clock %u: delivered %d, response 0x%x, "
               "tag 0x%x, id %u, rate %u)\n", name, (unsigned)SDHCI_CLOCK, set.delivered,
               (unsigned)set.response, (unsigned)set.tag_flags, (unsigned)set.id,
               (unsigned)set.rate);
        static const char *const sources[] = {"firmware", "the firmware's measurement",
                                              "the controller's capabilities", "an assumption"};
        printf("[sd] using a %u MHz base clock from %s\n", (unsigned)(hz / 1000000),
               sources[source]);
    }
    return hz;
}

// Reset the controller and start the identification clock. False if the
// controller does not respond.
static bool host_reset(void) {
    card.write_delay_us = 10;  // conservative until the clock is known
    wr(CONTROL0, 0);
    wr(CONTROL1, C1_SRST_HC);
    if (!wait_reg(CONTROL1, C1_SRST_HC | C1_SRST_CMD | C1_SRST_DATA, 0, 100000)) {
        return false;
    }
    wr(CONTROL0, C0_POWER_33V);
    wr(IRPT_MASK, 0xffffffffu);  // report every event in INTERRUPT...
    wr(IRPT_EN, 0);              // ...without raising interrupts
    wr(INTERRUPT, 0xffffffffu);
    return set_clock(ID_CLOCK_HZ);
}

blkdev_t *sdhci_init(bool *present) {
    *present = false;
    card = (struct sd_card){0};
    card.base_hz = base_clock();
    route_pins();
    if (!host_reset()) {
        printf("[sd] controller did not reset\n");
        return NULL;
    }

    uint32_t resp[4];
    command(0, 0, R_NONE, NULL);  // GO_IDLE_STATE
    // SEND_IF_COND: a 2.0 card echoes the check pattern; a 1.x card is silent.
    bool v2 = command(8, 0x1aa, R7, resp) == 0 && (resp[0] & 0xfff) == 0x1aa;
    if (!v2) {
        command(0, 0, R_NONE, NULL);
    }
    // SD_SEND_OP_COND until the card leaves its busy state (up to a second).
    uint32_t ocr = 0;
    uint64_t deadline = timer_us() + 1000000;
    bool answered = false;
    do {
        if (app_command(41, 0x00ff8000u | (v2 ? (1u << 30) : 0), R3, resp) == 0) {
            answered = true;
            ocr = resp[0];
        } else if (!answered) {
            printf("[sd] no card\n");
            return NULL;  // nothing in the slot (or nothing that speaks SD)
        }
        if (!(ocr & (1u << 31))) {
            udelay(10000);
        }
    } while (!(ocr & (1u << 31)) && timer_us() < deadline);
    *present = true;
    if (!(ocr & (1u << 31))) {
        printf("[sd] card stayed busy (OCR 0x%x)\n", (unsigned)ocr);
        return NULL;
    }
    card.high_capacity = v2 && (ocr & (1u << 30));

    uint32_t csd[4];
    if (command(2, 0, R2, resp) != 0 ||        // ALL_SEND_CID
        command(3, 0, R6, resp) != 0) {        // SEND_RELATIVE_ADDR
        printf("[sd] card did not identify itself\n");
        return NULL;
    }
    card.rca = resp[0] & 0xffff0000u;
    if (command(9, card.rca, R2, csd) != 0 ||  // SEND_CSD
        command(7, card.rca, R1B, resp) != 0) {  // SELECT_CARD
        printf("[sd] card did not report its size\n");
        return NULL;
    }
    card.sectors = sd_csd_sectors(csd);
    if (card.sectors == 0) {
        printf("[sd] unknown CSD structure\n");
        return NULL;
    }
    if ((!card.high_capacity && command(16, BLOCK, R1, resp) != 0) ||  // SET_BLOCKLEN
        app_command(6, 2, R1, resp) != 0) {                             // 4-bit bus
        printf("[sd] card refused the bus setup\n");
        return NULL;
    }
    wr(CONTROL0, rd(CONTROL0) | C0_HCTL_DWIDTH);
    if (!set_clock(DEFAULT_CLOCK_HZ)) {
        printf("[sd] clock did not settle\n");
        return NULL;
    }

    uint32_t sectors = card.sectors > 0xffffffffu ? 0xffffffffu : (uint32_t)card.sectors;
    card.dev = (blkdev_t){.read = sd_read, .sector_count = sectors, .ctx = &card,
#ifdef KORAOS_SD_WRITABLE
                          .write = sd_write, .flush = sd_flush, .read_only = false,
#else
                          .read_only = true,
#endif
    };
    printf("[sd] %s card, %u MB, %s, %u kHz\n", card.high_capacity ? "SDHC/SDXC" : "SDSC",
           (unsigned)(card.sectors / 2048), card.dev.read_only ? "read-only" : "writable",
           (unsigned)(sdhci_clock_hz(card.base_hz,
                                     sdhci_clock_divisor(card.base_hz, DEFAULT_CLOCK_HZ)) /
                      1000));
    return &card.dev;
}

#else

blkdev_t *sdhci_init(bool *present) {
    *present = false;
    return NULL;
}

#endif
