// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host tests for libk under threads: the mutex (user/libk/mutex.c), malloc
// shared by many threads at once (user/libk/malloc.c) and the job API
// (user/libk/job.c). Built with LIBK_HOST_TEST and -pthread: host threads
// stand in for KoraOS threads (thread_new/thread_join are stubbed with
// pthreads) and alloc_pages/free_pages with page-aligned host memory.

#include "test.h"

typedef unsigned long host_thread_t;
int pthread_create(host_thread_t *thread, const void *attr, void *(*start)(void *), void *arg);
int pthread_join(host_thread_t thread, void **result);
int sched_yield(void);
int posix_memalign(void **out, size_t align, size_t size);
void free(void *ptr);

// The host's free; koraos.h below renames free to libk_free.
static void host_free(void *ptr) {
    free(ptr);
}

#include "koraos.h"

// A tiny lock for the stubs themselves (not the code under test).
static int stub_lock;

static void stub_acquire(void) {
    while (__atomic_exchange_n(&stub_lock, 1, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
}

static void stub_release(void) {
    __atomic_store_n(&stub_lock, 0, __ATOMIC_RELEASE);
}

// alloc_pages/free_pages stubs ------------------------------------------

#define MAX_RUNS 4096

static struct {
    void *base;
    size_t pages;
} runs[MAX_RUNS];
static size_t pages_live;

void *alloc_pages(size_t count) {
    void *p = NULL;
    if (count == 0 || posix_memalign(&p, KORAOS_PAGE_SIZE, count * KORAOS_PAGE_SIZE) != 0) {
        return NULL;
    }
    memset(p, 0, count * KORAOS_PAGE_SIZE);
    stub_acquire();
    for (unsigned i = 0; i < MAX_RUNS; i++) {
        if (runs[i].base == NULL) {
            runs[i].base = p;
            runs[i].pages = count;
            pages_live += count;
            stub_release();
            return p;
        }
    }
    stub_release();
    host_free(p);
    return NULL;
}

int free_pages(void *base) {
    stub_acquire();
    for (unsigned i = 0; i < MAX_RUNS; i++) {
        if (runs[i].base != NULL && runs[i].base == base) {
            runs[i].base = NULL;
            pages_live -= runs[i].pages;
            stub_release();
            host_free(base);
            return 0;
        }
    }
    stub_release();
    return -1;
}

static void release_all_runs(void) {
    for (unsigned i = 0; i < MAX_RUNS; i++) {
        if (runs[i].base != NULL) {
            host_free(runs[i].base);
            runs[i].base = NULL;
        }
    }
    pages_live = 0;
}

void yield(void) {
    sched_yield();
}

// Thread stubs: a KoraOS thread becomes a host thread ------------------

#define MAX_THREADS 64
#define BAD_CORE 99  // thread_new fails for it, as for a core that does not exist

static struct {
    bool used;
    host_thread_t thread;
    thread_fn fn;
    void *arg;
    long result;
} threads[MAX_THREADS];

static void *thread_body(void *slot) {
    unsigned i = (unsigned)(unsigned long)slot;
    threads[i].result = threads[i].fn(threads[i].arg);
    return NULL;
}

int thread_new(void (*start)(void), thread_fn fn, void *arg, int core) {
    if (start != libk_thread_start || core == BAD_CORE) {
        return -1;
    }
    stub_acquire();
    for (unsigned i = 0; i < MAX_THREADS; i++) {
        if (!threads[i].used) {
            threads[i].used = true;
            threads[i].fn = fn;
            threads[i].arg = arg;
            stub_release();
            if (pthread_create(&threads[i].thread, NULL, thread_body,
                               (void *)(unsigned long)i) != 0) {
                threads[i].used = false;
                return -1;
            }
            return (int)i + 1;  // ids start at 1
        }
    }
    stub_release();
    return -1;
}

int thread_join(int tid, long *value) {
    if (tid < 1 || tid > MAX_THREADS || !threads[tid - 1].used) {
        return -1;
    }
    pthread_join(threads[tid - 1].thread, NULL);
    if (value != NULL) {
        *value = threads[tid - 1].result;
    }
    threads[tid - 1].used = false;
    return 0;
}

void libk_thread_start(void) {}

// Helpers --------------------------------------------------------------

#define WORKERS 8

static void run_workers(void *(*body)(void *)) {
    host_thread_t t[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        CHECK(pthread_create(&t[i], NULL, body, (void *)(unsigned long)i) == 0, "worker %u", i);
    }
    for (unsigned i = 0; i < WORKERS; i++) {
        pthread_join(t[i], NULL);
    }
}

// Tests ------------------------------------------------------------------

static mutex_t counter_lock = MUTEX_INIT;
static unsigned long counter;  // updated non-atomically, under counter_lock

#define INCREMENTS 50000

static void *count_up(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < INCREMENTS; i++) {
        mutex_lock(&counter_lock);
        unsigned long v = counter;
        __asm__ volatile("" ::: "memory");  // keep the read-modify-write split
        counter = v + 1;
        mutex_unlock(&counter_lock);
    }
    return NULL;
}

static void test_mutex_excludes(void) {
    run_workers(count_up);
    CHECK(counter == (unsigned long)WORKERS * INCREMENTS, "counter %lu, want %lu", counter,
          (unsigned long)WORKERS * INCREMENTS);
    CHECK(mutex_trylock(&counter_lock), "a free mutex was not taken");
    CHECK(!mutex_trylock(&counter_lock), "a held mutex was taken again");
    mutex_unlock(&counter_lock);
}

// Every worker keeps a set of live blocks, each filled with a pattern unique
// to the worker and block; any overlap between threads shows up as a mismatch.
#define SLOTS 64
#define ROUNDS 20000

static unsigned bad_blocks;

static void *churn(void *arg) {
    unsigned id = (unsigned)(unsigned long)arg;
    unsigned char *blocks[SLOTS] = {0};
    size_t sizes[SLOTS] = {0};
    unsigned seed = 0x9e3779b9u * (id + 1);
    unsigned bad = 0;
    for (unsigned round = 0; round < ROUNDS; round++) {
        seed = seed * 1103515245u + 12345u;
        unsigned slot = (seed >> 8) % SLOTS;
        unsigned char tag = (unsigned char)(id * 31 + slot);
        if (blocks[slot] != NULL) {
            for (size_t i = 0; i < sizes[slot]; i++) {
                bad += blocks[slot][i] != tag;
            }
        }
        unsigned op = (seed >> 20) % 4;
        // Mostly pool blocks; now and then a direct one (its own page run).
        size_t size = (seed >> 4) % 64 == 0 ? 300000 : (seed >> 12) % 3000;
        if (op == 0 && blocks[slot] != NULL) {
            free(blocks[slot]);
            blocks[slot] = NULL;
        } else if (op == 1 && blocks[slot] != NULL) {
            unsigned char *grown = realloc(blocks[slot], size + 1);
            if (grown == NULL) {
                bad++;
                continue;
            }
            blocks[slot] = grown;
            sizes[slot] = size + 1;
            memset(grown, tag, sizes[slot]);
        } else {
            free(blocks[slot]);
            blocks[slot] = op == 2 ? calloc(1, size) : malloc(size);
            if (blocks[slot] == NULL) {
                bad++;
                continue;
            }
            sizes[slot] = size;
            memset(blocks[slot], tag, size);
        }
    }
    for (unsigned slot = 0; slot < SLOTS; slot++) {
        free(blocks[slot]);
    }
    __atomic_fetch_add(&bad_blocks, bad, __ATOMIC_RELAXED);
    return NULL;
}

static void test_malloc_parallel(void) {
    run_workers(churn);
    CHECK(bad_blocks == 0, "%u corrupted or failed blocks", bad_blocks);
    struct kheap_info i;
    kheap_info(&i);
    CHECK(i.corrupt == 0, "%zu pool inconsistencies", i.corrupt);
    CHECK(i.bad_frees == 0, "%zu bad frees", i.bad_frees);
    CHECK(i.used_blocks == 0, "%zu blocks still used", i.used_blocks);
    CHECK(i.direct_allocs == 0, "%zu direct blocks left", i.direct_allocs);
    CHECK(pages_live == i.pool_bytes / KORAOS_PAGE_SIZE, "%zu pages held for %zu pool bytes",
          pages_live, i.pool_bytes);
}

static long sum_to(void *arg) {
    long n = (long)arg;
    long sum = 0;
    for (long i = 1; i <= n; i++) {
        sum += i;
    }
    return sum;
}

static void test_jobs(void) {
    enum { JOBS = 16 };
    job_t *jobs[JOBS];
    for (long i = 0; i < JOBS; i++) {
        jobs[i] = job_queue(sum_to, (void *)(1000 * (i + 1)), (int)(i % 4));
        CHECK(jobs[i] != NULL, "job %ld was not queued", i);
    }
    for (long i = 0; i < JOBS; i++) {
        long n = 1000 * (i + 1);
        long got = jobs[i] != NULL ? job_result(jobs[i]) : -1;
        CHECK(got == n * (n + 1) / 2, "job %ld returned %ld", i, got);
    }
    CHECK(job_queue(sum_to, (void *)1, BAD_CORE) == NULL, "a job on a bad core was queued");
    struct kheap_info info;
    kheap_info(&info);
    CHECK(info.used_blocks == 0, "jobs left %zu blocks", info.used_blocks);
}

static void test_cleanup(void) {
    release_all_runs();  // the heap's cached pools: LSan must see no leaks
}

TEST_MAIN(test_mutex_excludes, test_malloc_parallel, test_jobs, test_cleanup)
