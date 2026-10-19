/* ps: list the running tasks, with the core each one runs on. */
#include "libk/koraos.h"

static void pad_int(long v, unsigned width) {
    char digits[24];
    unsigned n = 0;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    do {
        digits[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0) {
        digits[n++] = '-';
    }
    for (unsigned i = n; i < width; i++) {
        write(1, " ", 1);
    }
    while (n > 0) {
        write(1, &digits[--n], 1);
    }
}

int main(void) {
    static const char *const states[] = {"running ", "ready   ", "sleeping", "zombie  "};
    struct task_info info;
    kputs("  PID  PPID  CPU  STATE     NAME\n");
    for (unsigned i = 0; task_info(i, &info) == 1; i++) {
        pad_int(info.pid, 5);
        pad_int(info.ppid, 6);
        pad_int((long)info.core, 5);
        kputs("  ");
        kputs(info.state <= TASK_ZOMBIE ? states[info.state] : "?       ");
        kputs("  ");
        kputs(info.name);
        kputs("\n");
    }
    return 0;
}
