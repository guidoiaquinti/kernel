// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host tests for libk's TLSF malloc (user/libk/malloc.c), built with
// LIBK_HOST_TEST so its entry points are libk_malloc and friends. The
// alloc_pages/free_pages syscalls are stubbed with page-aligned host memory,
// with a page budget to simulate exhaustion. Pool and direct-block metadata
// are cross-checked with kheap_info() after every phase.

#include "test.h"

int posix_memalign(void **out, size_t align, size_t size);
void free(void *ptr);

// The host's free; koraos.h below renames free to libk_free.
static void host_free(void *ptr) {
    free(ptr);
}

#include "koraos.h"

// alloc_pages/free_pages stubs ------------------------------------------

#define MAX_RUNS 4096

static struct {
    void *base;
    size_t pages;
} runs[MAX_RUNS];
static size_t pages_live;
static size_t page_budget = (size_t)-1;

// Freed runs stay readable for a while, as on the flat-mapped kernel, so a
// double free of a direct block reads stale (not unmapped) memory.
#define GRAVEYARD 8
static void *graveyard[GRAVEYARD];
static unsigned graveyard_next;

static void bury(void *run) {
    host_free(graveyard[graveyard_next]);
    graveyard[graveyard_next] = run;
    graveyard_next = (graveyard_next + 1) % GRAVEYARD;
}

void *alloc_pages(size_t count) {
    if (count == 0 || pages_live + count > page_budget) {
        return NULL;
    }
    for (unsigned i = 0; i < MAX_RUNS; i++) {
        if (runs[i].base == NULL) {
            void *p = NULL;
            if (posix_memalign(&p, KORAOS_PAGE_SIZE, count * KORAOS_PAGE_SIZE) != 0) {
                return NULL;
            }
            memset(p, 0, count * KORAOS_PAGE_SIZE);
            runs[i].base = p;
            runs[i].pages = count;
            pages_live += count;
            return p;
        }
    }
    return NULL;
}

int free_pages(void *base) {
    for (unsigned i = 0; i < MAX_RUNS; i++) {
        if (runs[i].base != NULL && runs[i].base == base) {
            bury(base);
            runs[i].base = NULL;
            pages_live -= runs[i].pages;
            return 0;
        }
    }
    return -1;
}

// The heap's mutex never waits here (one thread), but mutex.c links yield().
void yield(void) {}

// Helpers --------------------------------------------------------------

static struct kheap_info info(void) {
    struct kheap_info i;
    kheap_info(&i);
    return i;
}

static void check_consistent(const char *where) {
    struct kheap_info i = info();
    CHECK(i.corrupt == 0, "%s: %zu pool inconsistencies", where, i.corrupt);
    CHECK(i.used_bytes + i.free_bytes + i.pools * 32 == i.pool_bytes,
          "%s: used %zu + free %zu + overhead != pools %zu", where, i.used_bytes,
          i.free_bytes, i.pool_bytes);
}

static void check_empty(const char *where) {
    struct kheap_info i = info();
    CHECK(i.used_blocks == 0, "%s: %zu blocks still used", where, i.used_blocks);
    CHECK(i.direct_allocs == 0, "%s: %zu direct blocks left", where, i.direct_allocs);
    CHECK(i.pools <= 1, "%s: %zu pools kept, want at most 1", where, i.pools);
    CHECK(pages_live == i.pool_bytes / KORAOS_PAGE_SIZE, "%s: %zu pages held for %zu bytes",
          where, pages_live, i.pool_bytes);
    check_consistent(where);
}

// Tests ------------------------------------------------------------------

static void test_alignment_and_separation(void) {
    enum { N = 300 };
    static unsigned char *p[N];
    static size_t sz[N];
    for (unsigned i = 0; i < N; i++) {
        sz[i] = (i * 97) % 5000;
        p[i] = malloc(sz[i]);
        CHECK(p[i] != NULL, "malloc(%zu) failed", sz[i]);
        CHECK(((unsigned long)p[i] & 15) == 0, "malloc(%zu) misaligned", sz[i]);
        memset(p[i], (int)i, sz[i]);
    }
    check_consistent("after allocation");
    for (unsigned i = 0; i < N; i++) {
        bool ok = true;
        for (size_t j = 0; j < sz[i]; j++) {
            ok = ok && p[i][j] == (unsigned char)i;
        }
        CHECK(ok, "block %u overwritten by a neighbour", i);
    }
    for (unsigned i = 0; i < N; i += 2) {
        free(p[i]);
    }
    check_consistent("after freeing half");
    for (unsigned i = 1; i < N; i += 2) {
        free(p[i]);
    }
    check_empty("alignment");
}

static void test_coalescing(void) {
    void *a = malloc(1000);
    void *b = malloc(1000);
    void *c = malloc(1000);
    free(a);
    free(c);
    free(b);  // merges with both neighbours
    struct kheap_info i = info();
    CHECK(i.pools == 1, "pools %zu", i.pools);
    CHECK(i.free_bytes == i.pool_bytes - 32, "free space fragmented: %zu of %zu",
          i.free_bytes, i.pool_bytes);
    void *big = malloc(60000);  // only fits if the pool is one free block again
    CHECK(big != NULL && info().pools == 1, "coalesced pool not reused");
    free(big);
    check_empty("coalescing");
}

static void test_growth_and_release(void) {
    enum { N = 200 };
    static void *p[N];
    for (unsigned i = 0; i < N; i++) {
        p[i] = malloc(20000);  // 4 MB total: many 64 KB pools
        CHECK(p[i] != NULL, "malloc %u failed", i);
    }
    CHECK(info().pools >= 50, "expected many pools, got %zu", info().pools);
    check_consistent("grown");
    for (unsigned i = 0; i < N; i++) {
        free(p[i]);
    }
    check_empty("released");
}

static void test_direct_blocks(void) {
    size_t before = pages_live;
    unsigned char *a = malloc(1 << 20);
    CHECK(a != NULL, "1 MiB malloc failed");
    CHECK(info().direct_allocs == 1, "1 MiB block not direct");
    CHECK(pages_live == before + 257, "1 MiB + header should take 257 pages, took %zu",
          pages_live - before);
    memset(a, 0x77, 1 << 20);
    free(a);
    CHECK(pages_live == before, "direct pages not returned");
    size_t bad = info().bad_frees;
    free(a);  // double free of a direct block: its pages are gone
    CHECK(info().bad_frees == bad + 1, "direct double free not caught");
    check_empty("direct");
}

static void test_bad_frees(void) {
    unsigned char *a = malloc(100);
    unsigned char *keep = malloc(100);
    size_t bad = info().bad_frees;
    free(a + 4);  // misaligned
    free(a);
    free(a);      // double free
    CHECK(info().bad_frees == bad + 2, "bad frees counted %zu, want %zu", info().bad_frees,
          bad + 2);
    free(keep);
    free(NULL);
    check_empty("bad frees");
}

static void test_realloc_calloc(void) {
    unsigned char *p = realloc(NULL, 10);
    CHECK(p != NULL, "realloc(NULL) failed");
    for (unsigned i = 0; i < 10; i++) {
        p[i] = (unsigned char)(i + 1);
    }
    p = realloc(p, 8);  // shrink in place
    unsigned char *q = realloc(p, 100000);
    CHECK(q != NULL, "realloc grow failed");
    bool ok = true;
    for (unsigned i = 0; i < 8; i++) {
        ok = ok && q[i] == i + 1;
    }
    CHECK(ok, "realloc lost contents");
    q = realloc(q, 300 * 1024);  // into a direct block
    ok = q != NULL;
    for (unsigned i = 0; ok && i < 8; i++) {
        ok = q[i] == i + 1;
    }
    CHECK(ok, "realloc to a direct block lost contents");
    CHECK(realloc(q, 0) == NULL, "realloc(p, 0) should free and return NULL");

    unsigned *z = malloc(4096);
    memset(z, 0xff, 4096);
    free(z);
    z = calloc(1024, sizeof(unsigned));  // likely reuses the dirty block
    ok = z != NULL;
    for (unsigned i = 0; ok && i < 1024; i++) {
        ok = z[i] == 0;
    }
    CHECK(ok, "calloc memory not zeroed");
    free(z);
    CHECK(calloc((size_t)-1 / 2, 4) == NULL, "calloc overflow accepted");
    CHECK(malloc((size_t)-1 - 8) == NULL, "huge malloc accepted");
    check_empty("realloc/calloc");
}

static void test_exhaustion(void) {
    page_budget = pages_live;  // nothing more from the kernel
    void *cached = malloc(100);  // a cached empty pool may still serve this
    CHECK(malloc(100000) == NULL, "malloc succeeded with no pages left");
    CHECK(malloc(400 * 1024) == NULL, "direct malloc succeeded with no pages left");
    page_budget = (size_t)-1;
    void *p = malloc(100000);
    CHECK(p != NULL, "heap unusable after exhaustion");
    free(p);
    free(cached);
    check_empty("exhaustion");
}

static void test_stress(void) {
    enum { SLOTS = 128 };
    static unsigned char *p[SLOTS];
    static size_t sz[SLOTS];
    static unsigned char tag[SLOTS];
    unsigned rng = 12345;
    for (unsigned r = 0; r < 200000; r++) {
        rng = rng * 1103515245u + 12345u;
        unsigned s = (rng >> 8) % SLOTS;
        if (p[s] != NULL) {
            bool ok = true;
            for (size_t i = 0; i < sz[s]; i++) {
                ok = ok && p[s][i] == (unsigned char)(tag[s] + i);
            }
            CHECK(ok, "round %u: slot %u corrupted", r, s);
            free(p[s]);
            p[s] = NULL;
            continue;
        }
        rng = rng * 1103515245u + 12345u;
        unsigned kind = (rng >> 8) % 32;
        sz[s] = kind == 0 ? 200000 + (rng >> 12) % 200000
              : kind < 12 ? (rng >> 12) % 128
              : (rng >> 12) % 9000;
        p[s] = malloc(sz[s]);
        CHECK(p[s] != NULL, "round %u: malloc(%zu) failed", r, sz[s]);
        tag[s] = (unsigned char)(rng >> 16);
        for (size_t i = 0; i < sz[s]; i++) {
            p[s][i] = (unsigned char)(tag[s] + i);
        }
        if (r % 10000 == 0) {
            check_consistent("stress");
        }
    }
    for (unsigned s = 0; s < SLOTS; s++) {
        free(p[s]);
    }
    check_empty("stress");
}

static void test_cleanup(void) {
    for (unsigned i = 0; i < GRAVEYARD; i++) {
        bury(NULL);
    }
    void *last = malloc(1);
    free(last);
    struct kheap_info i = info();
    CHECK(i.pools <= 1, "pools left: %zu", i.pools);
}

TEST_MAIN(test_alignment_and_separation, test_coalescing, test_growth_and_release,
          test_direct_blocks, test_bad_frees, test_realloc_calloc, test_exhaustion,
          test_stress, test_cleanup)
