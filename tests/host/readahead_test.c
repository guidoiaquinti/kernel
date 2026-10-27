// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the read-ahead window (src/drivers/readahead.c) the USB mass
// storage driver serves single-sector reads from.
#include "drivers/readahead.h"
#include "test.h"

static void window(void) {
    uint32_t count;
    CHECK(readahead_window(0, 32, 131072, &count) == 0 && count == 32, "first window");
    CHECK(readahead_window(2085, 32, 131072, &count) == 2080 && count == 32,
          "aligned down to the window size");
    CHECK(readahead_window(2111, 32, 131072, &count) == 2080 && count == 32, "last sector of a window");
    CHECK(readahead_window(2112, 32, 131072, &count) == 2112 && count == 32, "next window");
    // A device that is not a whole number of windows: the last one is short.
    CHECK(readahead_window(1000, 32, 1003, &count) == 992 && count == 11, "cut at the end (%u)",
          (unsigned)count);
    CHECK(readahead_window(1002, 32, 1003, &count) == 992 && count == 11, "last sector");
    CHECK(readahead_window(5, 32, 7, &count) == 0 && count == 7, "a device smaller than a window");
    CHECK(readahead_window(0xfffffffeu, 32, 0xffffffffu, &count) == 0xffffffe0u && count == 31,
          "the top of the sector space");
}

static void holds(void) {
    readahead_t ra = {.valid = true, .lba = 2080, .count = 32};
    CHECK(readahead_holds(&ra, 2080) && readahead_holds(&ra, 2111), "both ends");
    CHECK(!readahead_holds(&ra, 2079) && !readahead_holds(&ra, 2112), "just outside");
    ra.count = 11;
    CHECK(readahead_holds(&ra, 2090) && !readahead_holds(&ra, 2091), "a short window");
    ra.valid = false;
    CHECK(!readahead_holds(&ra, 2080), "an invalid window holds nothing");
}

static void overlaps(void) {
    readahead_t ra = {.valid = true, .lba = 2080, .count = 32};
    CHECK(readahead_overlaps(&ra, 2080, 1) && readahead_overlaps(&ra, 2111, 1), "inside");
    CHECK(readahead_overlaps(&ra, 2070, 11) && readahead_overlaps(&ra, 2111, 100), "straddling");
    CHECK(readahead_overlaps(&ra, 2000, 200), "covering it");
    CHECK(!readahead_overlaps(&ra, 2070, 10) && !readahead_overlaps(&ra, 2112, 5), "adjacent");
    CHECK(!readahead_overlaps(&ra, 2085, 0), "an empty range");
    readahead_t top = {.valid = true, .lba = 0xffffffe0u, .count = 31};
    CHECK(readahead_overlaps(&top, 0xfffffff0u, 15) && !readahead_overlaps(&top, 0, 0xffffffe0u),
          "no wraparound at the top");
    ra.valid = false;
    CHECK(!readahead_overlaps(&ra, 2080, 32), "an invalid window overlaps nothing");
}

TEST_MAIN(window, holds, overlaps)
