// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

// Page runs handed to user programs by the alloc_pages/free_pages syscalls.
// With the flat identity map a run is directly usable at its physical address.
// Each run is recorded against its owning process, so whatever a program does not
// free itself is reclaimed when the process is reaped. Any of its threads may
// allocate or free a run.

struct user_run {
    struct user_run *next;
    void *base;
    size_t pages;
};

struct process;

// Allocate `pages` contiguous zeroed pages for `p`. Returns NULL for a zero
// count or when memory (pages, or the kernel heap for the record) runs out.
void *user_pages_alloc(struct process *p, size_t pages);

// Free a run previously returned to `p` by user_pages_alloc(), by its base
// address. Returns 0, or -1 if `p` owns no run starting at `base`.
int user_pages_free(struct process *p, void *base);

// Free every run `p` still owns (process teardown).
void user_pages_release_all(struct process *p);

// Runs and pages `p` currently owns.
size_t user_pages_count(const struct process *p, size_t *runs);
