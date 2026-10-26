// SPDX-License-Identifier: GPL-3.0-or-later
// EL0 regression fixture for threads, process lifetime, kill and libk under
// threads. <cores> is how many cores the machine has online.
//   threadprobe join <cores>    a thread per core: results, placement, a shared
//                               counter under a mutex, and join's error cases
//   threadprobe jobs <cores>    the libk job API across the cores
//   threadprobe malloc <cores>  every core's thread hammers one shared heap
//   threadprobe exit <cores>    exit(7) from main while other threads spin on
//                               every core or block (msleep, console read,
//                               join): the program must still end with 7
//   threadprobe last            main calls thread_exit(); the program lives on
//                               until its other thread returns (exit code 0)
//   threadprobe hang <cores>    spin on every core until killed
//   threadprobe chatter <ms>    print a numbered line every tick for <ms>, from
//                               a thread on every core (the serial line must
//                               keep each one whole)
#include "../../user/libk/koraos.h"

// Result lines are assembled first and written with one write(): tasks on
// other cores print at the same time, and only a single write is atomic.
static char line[128];
static unsigned line_len;

static void add(const char *s) {
    while (*s && line_len < sizeof(line) - 1) {
        line[line_len++] = *s++;
    }
}

static void add_num(long v) {
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
    while (n > 0 && line_len < sizeof(line) - 1) {
        line[line_len++] = digits[--n];
    }
}

static void put_line(void) {
    write(1, line, line_len);
    line_len = 0;
}

static int fail(const char *what, long a, long b) {
    add("threadprobe: FAILED ");
    add(what);
    add(" (");
    add_num(a);
    add(", ");
    add_num(b);
    add(")\n");
    put_line();
    return 1;
}

// The core thread `tid` runs on, from task_info, or -1.
static int core_of(int tid) {
    struct task_info info;
    for (unsigned i = 0; task_info(i, &info) == 1; i++) {
        if (info.tid == tid) {
            return (int)info.core;
        }
    }
    return -1;
}

// Pure EL0 work: no syscalls, so only the tick (or a kill) interrupts it.
static long sum_to(void *arg) {
    long n = (long)arg;
    long sum = 0;
    for (long i = 1; i <= n; i++) {
        sum += i;
        __asm__ volatile("" ::: "memory");
    }
    return sum;
}

static mutex_t counter_lock = MUTEX_INIT;
static volatile long counter;
#define INCREMENTS 20000

static long count_up(void *unused) {
    (void)unused;
    for (int i = 0; i < INCREMENTS; i++) {
        mutex_lock(&counter_lock);
        long v = counter;
        __asm__ volatile("" ::: "memory");
        counter = v + 1;
        mutex_unlock(&counter_lock);
    }
    return getpid();
}

static int join_test(int cores) {
    int tids[4];
    for (int c = 0; c < cores; c++) {
        tids[c] = thread_create(sum_to, (void *)(100000L * (c + 1)), c);
        if (tids[c] <= 0) {
            return fail("thread_create", c, tids[c]);
        }
    }
    for (int c = 0; c < cores; c++) {
        int core = core_of(tids[c]);
        if (core != c && core != -1) {  // -1: it may already be gone
            return fail("thread on wrong core", c, core);
        }
    }
    for (int c = 0; c < cores; c++) {
        long n = 100000L * (c + 1);
        long value = 0;
        if (thread_join(tids[c], &value) != 0 || value != n * (n + 1) / 2) {
            return fail("join result", c, value);
        }
    }
    // Shared memory: every thread bumps one counter under the mutex, and sees
    // the program's own pid.
    for (int c = 0; c < cores; c++) {
        tids[c] = thread_create(count_up, NULL, c);
        if (tids[c] <= 0) {
            return fail("thread_create", c, tids[c]);
        }
    }
    for (int c = 0; c < cores; c++) {
        long pid = 0;
        if (thread_join(tids[c], &pid) != 0 || pid != getpid()) {
            return fail("getpid in a thread", pid, getpid());
        }
    }
    if (counter != (long)cores * INCREMENTS) {
        return fail("shared counter", counter, (long)cores * INCREMENTS);
    }
    // Errors: joining twice, oneself, a stranger, or a core that is not there.
    if (thread_join(tids[0], NULL) != -1 || thread_join(getpid(), NULL) != -1 ||
        thread_join(1, NULL) != -1 || thread_create(sum_to, NULL, 9) != -1) {
        return fail("join/create error cases", 0, 0);
    }
    add("threadprobe: joined ");
    add_num(2 * cores);
    add(" threads on ");
    add_num(cores);
    add(" cores, results ok\n");
    put_line();
    return 0;
}

static int jobs_test(int cores) {
    enum { JOBS = 8 };
    job_t *jobs[JOBS];
    for (long i = 0; i < JOBS; i++) {
        jobs[i] = job_queue(sum_to, (void *)(50000 * (i + 1)), (int)(i % cores));
        if (jobs[i] == NULL) {
            return fail("job_queue", i, 0);
        }
    }
    for (long i = 0; i < JOBS; i++) {
        long n = 50000 * (i + 1);
        long got = job_result(jobs[i]);
        if (got != n * (n + 1) / 2) {
            return fail("job result", i, got);
        }
    }
    if (job_queue(sum_to, NULL, 9) != NULL) {
        return fail("job on a missing core", 9, 0);
    }
    add("threadprobe: ");
    add_num(JOBS);
    add(" jobs on ");
    add_num(cores);
    add(" cores, results ok\n");
    put_line();
    return 0;
}

// Each thread keeps live blocks filled with its own pattern and checks them
// before every free: a heap shared without exclusion corrupts them quickly.
#define SLOTS 32
#define ROUNDS 20000

static long churn(void *arg) {
    long id = (long)arg;
    unsigned char *blocks[SLOTS] = {0};
    unsigned sizes[SLOTS] = {0};
    unsigned seed = 0x9e3779b9u * (unsigned)(id + 1);
    long bad = 0;
    for (int round = 0; round < ROUNDS; round++) {
        seed = seed * 1103515245u + 12345u;
        unsigned slot = (seed >> 8) % SLOTS;
        unsigned char tag = (unsigned char)(id * 31 + slot);
        if (blocks[slot] != NULL) {
            for (unsigned i = 0; i < sizes[slot]; i++) {
                bad += blocks[slot][i] != tag;
            }
            free(blocks[slot]);
        }
        sizes[slot] = (seed >> 4) % 128 == 0 ? 300000 : (seed >> 12) % 2000;
        blocks[slot] = (seed >> 20) & 1 ? malloc(sizes[slot]) : calloc(1, sizes[slot]);
        if (blocks[slot] == NULL) {
            bad++;
            sizes[slot] = 0;
            continue;
        }
        memset(blocks[slot], tag, sizes[slot]);
    }
    for (unsigned slot = 0; slot < SLOTS; slot++) {
        free(blocks[slot]);
    }
    return bad;
}

static int malloc_test(int cores) {
    struct kheap_info before, after;
    kheap_info(&before);
    int tids[4];
    for (int c = 0; c < cores; c++) {
        tids[c] = thread_create(churn, (void *)(long)c, c);
        if (tids[c] <= 0) {
            return fail("thread_create", c, tids[c]);
        }
    }
    long bad = 0;
    for (int c = 0; c < cores; c++) {
        long value = 1;
        thread_join(tids[c], &value);
        bad += value;
    }
    kheap_info(&after);
    if (bad != 0 || after.corrupt != 0 || after.bad_frees != 0 ||
        after.used_blocks != before.used_blocks || after.direct_allocs != before.direct_allocs) {
        return fail("shared heap", bad, (long)after.corrupt + (long)after.bad_frees);
    }
    add("threadprobe: malloc from ");
    add_num(cores);
    add(" threads ok\n");
    put_line();
    return 0;
}

static long spin_forever(void *unused) {
    (void)unused;
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
    return 0;
}

static long sleep_long(void *unused) {
    (void)unused;
    msleep(600000);
    return 0;
}

static long read_console(void *unused) {
    (void)unused;
    char buf[8];
    read(0, buf, sizeof(buf));
    return 0;
}

static long join_other(void *tid) {
    thread_join((int)(long)tid, NULL);
    return 0;
}

static int exit_test(int cores) {
    int spinner = -1;
    for (int c = 0; c < cores; c++) {
        spinner = thread_create(spin_forever, NULL, c);
        if (spinner <= 0) {
            return fail("thread_create", c, spinner);
        }
    }
    if (thread_create(sleep_long, NULL, -1) <= 0 || thread_create(read_console, NULL, -1) <= 0 ||
        thread_create(join_other, (void *)(long)spinner, -1) <= 0) {
        return fail("thread_create", -1, 0);
    }
    msleep(100);  // let them all get going (or blocked)
    add("threadprobe: exiting with 7\n");
    put_line();
    exit(7);
}

static long outlive_main(void *unused) {
    (void)unused;
    msleep(300);
    add("threadprobe: last thread done\n");
    put_line();
    return 0;
}

static int last_test(void) {
    if (thread_create(outlive_main, NULL, -1) <= 0) {
        return fail("thread_create", -1, 0);
    }
    add("threadprobe: main thread exits\n");
    put_line();
    thread_exit(5);  // a thread's value, not the program's exit code
}

static int hang_test(int cores) {
    for (int c = 0; c < cores; c++) {
        if (thread_create(spin_forever, NULL, c) <= 0) {
            return fail("thread_create", c, 0);
        }
    }
    add("threadprobe: hanging\n");
    put_line();
    for (;;) {
        msleep(1000);
    }
}

static unsigned long chatter_ms;

// Append the decimal digits of v to buf at *len (a thread's own buffer).
static void append_num(char *buf, unsigned *len, unsigned long v) {
    char digits[24];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        buf[(*len)++] = digits[--n];
    }
}

static long chatter(void *id) {
    unsigned long start = uptime_us();
    for (unsigned long n = 0; uptime_us() - start < chatter_ms * 1000; n++) {
        char buf[64];
        unsigned len = 0;
        for (const char *s = "threadprobe: chatter "; *s; s++) {
            buf[len++] = *s;
        }
        append_num(buf, &len, (unsigned long)id);
        for (const char *s = " line "; *s; s++) {
            buf[len++] = *s;
        }
        append_num(buf, &len, n);
        for (const char *s = " end\n"; *s; s++) {
            buf[len++] = *s;
        }
        write(1, buf, len);
        msleep(10);
    }
    return 0;
}

static int chatter_test(void) {
    struct sysinfo info;
    int cores = sysinfo(&info) == 0 ? (int)info.cpus : 1;
    int tids[4];
    for (int c = 0; c < cores; c++) {
        tids[c] = thread_create(chatter, (void *)(long)c, c);
    }
    for (int c = 0; c < cores; c++) {
        if (tids[c] > 0) {
            thread_join(tids[c], NULL);
        }
    }
    add("threadprobe: chatter done\n");
    put_line();
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && kstreq(argv[1], "chatter")) {
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) {
            chatter_ms = chatter_ms * 10 + (unsigned long)(*p - '0');
        }
        return chatter_test();
    }
    int cores = argc > 2 ? argv[2][0] - '0' : 1;
    if (argc < 2 || cores < 1 || cores > 4) {
        kputs("usage: threadprobe join|jobs|malloc|exit|hang <cores>, threadprobe last\n");
        return 2;
    }
    if (kstreq(argv[1], "join")) {
        return join_test(cores);
    }
    if (kstreq(argv[1], "jobs")) {
        return jobs_test(cores);
    }
    if (kstreq(argv[1], "malloc")) {
        return malloc_test(cores);
    }
    if (kstreq(argv[1], "exit")) {
        return exit_test(cores);
    }
    if (kstreq(argv[1], "last")) {
        return last_test();
    }
    if (kstreq(argv[1], "hang")) {
        return hang_test(cores);
    }
    kputs("usage: threadprobe join|jobs|malloc|exit|hang <cores>, threadprobe last\n");
    return 2;
}
