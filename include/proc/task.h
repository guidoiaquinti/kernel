#pragma once

#include "common.h"
#include "fs/fat32.h"
#include "fs/namespace.h"
#include "arch/fpsimd.h"
#include "arch/spinlock.h"
#include "arch/trapframe.h"

// Process model. Every task has its own kernel stack, with the EL0 register
// state saved in a trap frame at its top, and a saved kernel context (callee-
// saved registers and stack pointer) that cpu_switch() switches between. The
// boot thread is task 0: it is not in the table and has no user side.
//
// Scheduling is preemptive and single-core: the timer tick asks for a
// reschedule, which happens on the way back to EL0, so each runnable task gets
// a time slice of one tick in round-robin order. The kernel itself is not
// preemptible: a task in a syscall runs until it returns or blocks (waiting
// for a child, sleeping on a wait queue such as console input, or in msleep).
// spawn is create plus wait unless asked not to wait. A finished task becomes a
// zombie, its memory still held so its exit code remains valid, until its
// parent reaps it; a task whose parent exited first is reaped automatically.

#define MAX_TASKS 8
#define KSTACK_PAGES 4       // 16 KB kernel stack per task
#define MAX_ARGS 16          // most argv entries a spawned program may receive
#define MAX_OPEN_FILES 16    // open file/directory handles per task
#define FD_BASE 3            // fds 0/1/2 are the console; real files start here
#define TASK_NAME_MAX 16

// One open file or directory handle in a task's descriptor table. The FAT32
// handle is a plain value cursor (no external resource), so closing just frees
// the slot.
typedef struct {
    bool used;
    fat32_file_t file;
} open_file_t;

typedef enum {
    TASK_UNUSED = 0,  // free table slot
    TASK_RUNNABLE,    // running, or ready to run
    TASK_BLOCKED,     // waiting; see block_reason_t
    TASK_EXITED,      // finished but not yet reaped (memory still held)
} task_state_t;

typedef enum {
    BLOCK_NONE = 0,
    BLOCK_CHILD,      // a child to exit (waiting_for, or any child if NULL)
    BLOCK_QUEUE,      // a wake-up on a wait queue (wq)
    BLOCK_SLEEP,      // the tick count to reach wake_tick
} block_reason_t;

// Tasks sleeping until an event; woken all at once by wait_queue_wake_all().
struct wait_queue {
    struct task *head;
};

// Kernel context saved by cpu_switch(); layout mirrored in src/arch/entry.S.
struct cpu_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp;  // x29
    uint64_t lr;  // x30: where cpu_switch() returns to
    uint64_t sp;
};

typedef struct task {
    int pid;                          // >0 when in use, 0 when UNUSED (and task 0)
    task_state_t state;
    int exit_code;
    char name[TASK_NAME_MAX];
    struct task *parent;              // who spawned this task (NULL: the kernel)
    block_reason_t blocked_on;        // why a BLOCKED task waits
    struct task *waiting_for;         // BLOCK_CHILD: the child, or NULL for any child
    struct wait_queue *wq;            // BLOCK_QUEUE: the queue it sleeps on
    struct task *wq_next;             // next sleeper on that queue
    uint64_t wake_tick;               // BLOCK_SLEEP: systick count to wake at
    bool orphan;                      // its parent exited: reaped automatically
    bool bkl;                         // holds the BKL (dropped while switched out)
    int forbid;                       // forbid() depth: keeps the BKL and the core
    void *image;                      // loaded ELF region (for reclaim)
    size_t image_pages;
    void *stack;                      // user stack region (for reclaim)
    void *kstack;                     // kernel stack (KSTACK_PAGES pages)
    struct trapframe *tf;             // EL0 state, at the top of kstack
    struct cpu_context ctx;           // kernel context while switched out
    struct user_run *runs;            // pages from alloc_pages (proc/user_mem.h)
    fs_cwd_t cwd;                     // inherited per-task filesystem location
    open_file_t files[MAX_OPEN_FILES];  // per-task fd table (indexed fd - FD_BASE)
    bool fp_used;                     // fp holds this task's FP/SIMD state
    struct fpsimd_state fp;           // saved when another task takes the registers
} task_t;

// Load the named program and create a runnable task for it: argv holds argc
// string pointers (readable in the caller's address space), copied onto the
// child's stack and delivered as main(argc, argv). The child does not run
// until the caller blocks or yields. Returns the new task, or NULL.
task_t *task_create(const char *name, int argc, char *const argv[]);

#define SPAWN_NOWAIT 1  // return as soon as the child exists (a background job)

// Create a task and, unless SPAWN_NOWAIT, block until it exits. Returns the
// child's pid, or -1 on failure. The child stays an unreaped zombie until its
// parent collects it with task_wait_ex().
int task_spawn(const char *name, int argc, char *const argv[], int flags);

#define WAIT_NOHANG 1  // do not block if no matching child has exited

// Reap an exited child of the current task: `pid`, or any child for -1. Blocks
// until one exits unless WAIT_NOHANG. Stores the exit code in *code (if not
// NULL) and returns the child's pid; returns 0 for WAIT_NOHANG with nothing to
// reap yet, and -1 if the task has no such child.
int task_wait_ex(int pid, int *code, int flags);

// Reap child `pid`, blocking until it exits; returns its exit code, or -1.
int task_wait(int pid);

// Block the current task for at least `ms` milliseconds (rounded up to ticks).
void task_msleep(uint64_t ms);

// Sleep on `wq` until wait_queue_wake_all(). Call holding `cond`, the
// spinlock protecting the condition being waited for (taken with
// spin_lock_irqsave), after checking it: the task is queued before `cond` is
// released, so a waker, which takes `cond` to change the condition, cannot slip
// in between. Returns holding `cond` again.
void wait_queue_sleep(struct wait_queue *wq, struct spinlock *cond);

// Make every task sleeping on `wq` runnable. Safe from interrupt handlers.
void wait_queue_wake_all(struct wait_queue *wq);

// Called by the timer interrupt on every tick: wakes due sleepers and asks
// for a reschedule (taken on the way back to EL0).
void sched_tick(uint64_t now);

// On the way back to EL0 (from src/arch/vectors.S): switch tasks if a
// reschedule was requested, unless the task is forbid()den. Called with IRQs
// masked and without the BKL.
void sched_preempt_check(void);

// Make the boot thread task 0 of this core; it starts out holding the BKL.
void task_init_boot(void);

// On a secondary core: give it an idle task (pid 0) to be "current".
void task_init_idle(void);

// The big kernel lock (docs/locking.md). Taken on every kernel entry from EL0
// and released on the way back (from src/arch/vectors.S), except while the task
// is forbid()den; dropped while a task is switched out. The boot thread
// releases it for good once it only serves the serial line.
void bkl_enter_from_el0(void);
void bkl_exit_to_el0(void);
void bkl_release_for_good(void);
int bkl_holder_pid(void);  // -1 if free

// forbid()/permit() (syscalls 17/18): keep the BKL across returns to EL0 and
// stop preemption on this core, nesting. Blocking breaks the forbid while the
// task sleeps, as on AmigaOS. They return the new depth (permit: -1 if the
// task was not forbidden).
int task_forbid(void);
int task_permit(void);

// pid of the current task (0 for the kernel's own boot task).
int task_getpid(void);

// The current user task, or NULL while the kernel's boot task runs.
task_t *task_current(void);

// The running task, including the boot task (never NULL).
task_t *task_running(void);

// Mark the current task exited, wake a waiting parent and switch away for good.
void task_exit(int code) __attribute__((noreturn));

// Give up the CPU to another runnable task, if there is one.
void task_yield(void);

// Release every task-table slot and its held memory. The kernel calls this to
// tear down after the top-level task and any descendants finish.
void task_reap_all(void);

// Kernel stack bytes a task has used at most (from the untouched fill pattern),
// and the most any task has used since boot.
size_t task_kstack_peak(const task_t *t);
size_t task_kstack_peak_max(void);

// Visit every live task (for diagnostics).
void task_for_each(void (*fn)(const task_t *t, void *ctx), void *ctx);

// --- implemented in src/arch/entry.S ---
// Save the callee-saved context into *from and resume *to.
void cpu_switch(struct cpu_context *from, struct cpu_context *to);
// First return of a new task: release the sched_lock it inherited
// (schedule_tail), restore its trap frame (at sp) and eret to EL0.
void ret_to_user(void);
void schedule_tail(void);
