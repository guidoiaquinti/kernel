// SPDX-License-Identifier: GPL-3.0-or-later
// EL0 fixture: checksum files without printing them.
//   sumprobe <path>...   prints "sumprobe: <path> <size> <fnv1a64 in hex>" per
//                        file (one write each), exit 1 if any cannot be read
// The smoke test compares against the host's copy of the same bytes.
#include "../../user/libk/koraos.h"

static char line[160];
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

static void add_hex(unsigned long v) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (line_len < sizeof(line) - 1) {
            line[line_len++] = "0123456789abcdef"[(v >> shift) & 0xf];
        }
    }
}

int main(int argc, char **argv) {
    static unsigned char buf[8192];
    int failed = 0;
    for (int a = 1; a < argc; a++) {
        int fd = open(argv[a], O_RDONLY);
        unsigned long hash = 0xcbf29ce484222325ul;
        unsigned long size = 0;
        long n = fd < 0 ? -1 : 0;
        while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0) {
            for (long i = 0; i < n; i++) {
                hash = (hash ^ buf[i]) * 0x100000001b3ul;
            }
            size += (unsigned long)n;
        }
        if (fd >= 0) {
            close(fd);
        }
        add("sumprobe: ");
        add(argv[a]);
        if (n < 0) {
            add(" unreadable\n");
            failed = 1;
        } else {
            add(" ");
            add_num(size);
            add(" ");
            add_hex(hash);
            add("\n");
        }
        write(1, line, line_len);
        line_len = 0;
    }
    return failed;
}
