// SPDX-License-Identifier: GPL-3.0-or-later
// EL0 regression fixture for the scheduler.
//   schedprobe sleep <ms>  msleep(ms), then report how long it really took
//                          (exit 1 if it returned early).
//   schedprobe spin <ms>   busy-loop for ms of wall time without a single
//                          syscall: the rest of the system (the shell) only
//                          keeps running meanwhile if the timer preempts us
//                          (or it runs on another core). Reports the longest
//                          gap the loop saw: time slices lost to others.
#include "../../user/libk/koraos.h"

// Result lines are assembled first and written with one write(): tasks on
// other cores print at the same time, and only a single write is atomic.
static char line[96];
static unsigned line_len;

static void add(const char *s) {
    while (*s && line_len < sizeof(line) - 1) {
        line[line_len++] = *s++;
    }
}

static void add_num(unsigned long v) {
    char digits[24];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0 && line_len < sizeof(line) - 1) {
        line[line_len++] = digits[--n];
    }
}

static void put_line(void) {
    write(1, line, line_len);
    line_len = 0;
}

static unsigned long parse_ms(const char *s) {
    unsigned long v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (unsigned long)(*s - '0');
    }
    return v;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        kputs("usage: schedprobe sleep|spin <ms>\n");
        return 2;
    }
    unsigned long ms = parse_ms(argv[2]);
    unsigned long start = uptime_us();
    if (kstreq(argv[1], "sleep")) {
        msleep(ms);
        unsigned long took = (uptime_us() - start) / 1000;
        add("schedprobe: slept ");
        add_num(took);
        add(" ms\n");
        put_line();
        return took >= ms ? 0 : 1;
    }
    if (kstreq(argv[1], "spin")) {
        // Also report the longest stretch the loop lost the CPU for: whole
        // time slices if another task shares this core, next to nothing if
        // it has the core to itself.
        unsigned long last = start;
        unsigned long longest = 0;
        while (last - start < ms * 1000) {
            unsigned long now = uptime_us();
            if (now - last > longest) {
                longest = now - last;
            }
            last = now;
        }
        add("schedprobe: spun ");
        add_num((uptime_us() - start) / 1000);
        add(" ms, longest gap ");
        add_num(longest);
        add(" us\n");
        put_line();
        return 0;
    }
    kputs("usage: schedprobe sleep|spin <ms>\n");
    return 2;
}
