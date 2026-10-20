// SPDX-License-Identifier: GPL-3.0-or-later
//
// libk jobs, after TempleOS's JobQue()/JobResGet(): each job is a thread of
// its own on the core it was queued to, and collecting the result joins it.
// The kernel places and time-slices the threads, so jobs queued to one core
// share it and jobs on different cores run in parallel.

#include "koraos.h"

struct job {
    int tid;
};

job_t *job_queue(thread_fn fn, void *arg, int core) {
    job_t *job = malloc(sizeof(*job));
    if (job == NULL) {
        return NULL;
    }
    job->tid = thread_create(fn, arg, core);
    if (job->tid < 0) {
        free(job);
        return NULL;
    }
    return job;
}

long job_result(job_t *job) {
    long result = -1;
    if (thread_join(job->tid, &result) != 0) {
        result = -1;
    }
    free(job);
    return result;
}
