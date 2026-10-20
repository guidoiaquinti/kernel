// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host tests for per-process user page runs (src/proc/user_mem.c): the bookkeeping
// behind the alloc_pages/free_pages syscalls and the reclaim on process teardown.
// The frame allocator and the kernel heap are stubbed with host memory and
// failure switches; every stub allocation is tracked to catch leaks.

#include "mm/frame_alloc.h"
#include "mm/kmalloc.h"
#include "proc/task.h"
#include "proc/user_mem.h"
#include "test.h"

void *malloc(size_t size);
void free(void *ptr);

static int frames_live;
static int records_live;
static bool fail_frames;
static bool fail_records;

void *frame_alloc_pages(size_t count) {
    if (fail_frames || count == 0) {
        return NULL;
    }
    frames_live++;
    return malloc(count * 4096);
}

void frame_free_pages(void *pages, size_t count) {
    (void)count;
    frames_live--;
    free(pages);
}

void *kmalloc(size_t size) {
    if (fail_records) {
        return NULL;
    }
    records_live++;
    return malloc(size);
}

void kfree(void *ptr) {
    records_live--;
    free(ptr);
}

static void test_alloc_free(void) {
    struct process t = {0};
    void *a = user_pages_alloc(&t, 1);
    void *b = user_pages_alloc(&t, 5);
    void *c = user_pages_alloc(&t, 2);
    CHECK(a && b && c, "allocations failed");
    size_t runs = 0;
    CHECK(user_pages_count(&t, &runs) == 8 && runs == 3, "count %zu pages in %zu runs",
          user_pages_count(&t, NULL), runs);
    CHECK(user_pages_free(&t, b) == 0, "free of a middle run failed");
    CHECK(user_pages_free(&t, b) == -1, "double free accepted");
    CHECK(user_pages_free(&t, (char *)a + 4096) == -1, "free inside a run accepted");
    CHECK(user_pages_count(&t, &runs) == 3 && runs == 2, "count after free");
    CHECK(user_pages_alloc(&t, 0) == NULL, "zero pages accepted");
    user_pages_release_all(&t);
    CHECK(t.runs == NULL && frames_live == 0 && records_live == 0,
          "release_all left %d frames, %d records", frames_live, records_live);
}

static void test_runs_are_per_process(void) {
    struct process t1 = {0};
    struct process t2 = {0};
    void *a = user_pages_alloc(&t1, 1);
    CHECK(user_pages_free(&t2, a) == -1, "another process freed t1's run");
    user_pages_release_all(&t2);
    CHECK(user_pages_count(&t1, NULL) == 1, "t1 lost its run");
    user_pages_release_all(&t1);
    CHECK(frames_live == 0 && records_live == 0, "leak");
}

static void test_failures_leak_nothing(void) {
    struct process t = {0};
    fail_records = true;
    CHECK(user_pages_alloc(&t, 1) == NULL, "succeeded without a record");
    fail_records = false;
    fail_frames = true;
    CHECK(user_pages_alloc(&t, 1) == NULL, "succeeded without frames");
    fail_frames = false;
    CHECK(t.runs == NULL && frames_live == 0 && records_live == 0,
          "failure leaked %d frames, %d records", frames_live, records_live);
}

TEST_MAIN(test_alloc_free, test_runs_are_per_process, test_failures_leak_nothing)
