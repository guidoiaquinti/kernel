#pragma once

#include "common.h"
#include "fs/fat32.h"
#include "fs/namespace.h"
#include "arch/fpsimd.h"
#include "arch/spinlock.h"
#include "arch/trapframe.h"

// Process model. A process is a loaded program: its image, the pages it
// allocated, its cwd and fd table, its parent and its exit code. It runs as one
// or more threads, each a task with its own kernel stack (the EL0 register
// state saved in a trap frame at its top), its own user stack, FP/SIMD state
// and core, and a saved kernel context (callee-saved registers and stack
// pointer) that cpu_switch() switches between. With one flat address space
// there is nothing else to share. A process's first thread is its main thread,
// whose id is the process id; every thread gets its own id from the same
// counter. The boot thread is task 0: it is not in the table and has no user
// side.
//
// Scheduling is preemptive: the timer tick asks for a reschedule, which
// happens on the way back to EL0, so each runnable task gets a time slice of
// one tick in round-robin order on its core. The kernel itself is not
// preemptible: a task in a syscall runs until it returns or blocks (waiting
// for a child or a thread, sleeping on a wait queue such as console input, or
// in msleep). spawn is create plus wait unless asked not to wait.
//
// A process lives until its last thread exits. exit() (from any thread) or
// kill() marks the remaining threads killed: each one dies on its next kernel
// entry or return to EL0 on its own core (a blocked one is woken for it), so
// no inter-processor interrupt is needed. A finished process is a zombie,
// still holding its memory and threads, until its parent reaps it; one whose
// parent exited first is reaped automatically. An exited thread stays until
// another thread of its process joins it, or until the process is reaped.

#define MAX_TASKS 32         // threads, system-wide
#define MAX_PROCS 16         // processes, zombies included
#define THREAD_STACK_PAGES 4 // 16 KB user stack per created thread
#define EXIT_KILLED (-9)     // exit code of a process ended by kill()
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
    TASK_EXITED,      // finished; joined, or freed with its process
} task_state_t;

typedef enum {
    BLOCK_NONE = 0,
    BLOCK_CHILD,      // a child process to finish (waiting_for, or any if NULL)
    BLOCK_JOIN,       // a thread of its process to exit (waiting_for)
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

// What the threads of one program share. BKL-protected, except `zombie`,
// which also changes under sched_lock (see block_on_child).
struct process {
    int pid;                          // its main thread's id; 0 for a free slot
    int threads;                      // threads that have not exited yet
    bool exiting;                     // exit() or kill(): the other threads die
    bool zombie;                      // every thread exited: waits to be reaped
    bool orphan;                      // its parent exited: reaped automatically
    int exit_code;
    struct process *parent;           // who spawned it (NULL: the kernel)
    char name[TASK_NAME_MAX];
    void *image;                      // loaded ELF region (for reclaim)
    size_t image_pages;
    struct user_run *runs;            // pages from alloc_pages (proc/user_mem.h)
    fs_cwd_t cwd;                     // inherited filesystem location
    open_file_t files[MAX_OPEN_FILES];  // fd table (indexed fd - FD_BASE)
};

typedef struct task {
    int pid;                          // thread id: >0 when in use, 0 when UNUSED (and task 0)
    task_state_t state;
    unsigned core;                    // the core it runs on, for life
    volatile bool on_cpu;             // running, or its core is switching away
    volatile bool killed;             // dies on its next kernel entry or return to EL0
    long exit_value;                  // thread_exit()'s value, for thread_join()
    char name[TASK_NAME_MAX];
    struct process *proc;             // NULL for the boot and idle tasks
    block_reason_t blocked_on;        // why a BLOCKED task waits
    const void *waiting_for;          // BLOCK_CHILD: the process (NULL: any); BLOCK_JOIN: the task
    struct wait_queue *wq;            // BLOCK_QUEUE: the queue it sleeps on
    struct task *wq_next;             // next sleeper on that queue
    uint64_t wake_tick;               // BLOCK_SLEEP: systick count to wake at
    bool bkl;                         // holds the BKL (dropped while switched out)
    int forbid;                       // forbid() depth: keeps the BKL and the core
    void *stack;                      // user stack (for reclaim)
    size_t stack_pages;
    void *kstack;                     // kernel stack (KSTACK_PAGES pages)
    struct trapframe *tf;             // EL0 state, at the top of kstack
    struct cpu_context ctx;           // kernel context while switched out
    bool fp_used;                     // fp holds this task's FP/SIMD state
    struct fpsimd_state fp;           // saved when another task takes the registers
} task_t;

// Load the named program and create a process for it, with a runnable main
// thread: argv holds argc string pointers (readable in the caller's address
// space), copied onto the child's stack and delivered as main(argc, argv). The
// thread runs on `core` for life, or for -1 on the online core with the fewest
// tasks. Returns the new process, or NULL (also for a core that does not exist
// or is offline).
struct process *task_create(const char *name, int argc, char *const argv[], int core);

#define SPAWN_NOWAIT 1  // return as soon as the child exists (a background job)
// Run the child on core n (0..MAX_CPUS-1); without it the kernel picks the
// least-loaded core. Bits 8..11 hold n + 1.
#define SPAWN_CORE(n) ((((n) + 1) & 0xf) << 8)
#define SPAWN_CORE_MASK 0xf00
#define SPAWN_CORE_OF(flags) ((int)(((flags) & SPAWN_CORE_MASK) >> 8) - 1)

// Create a process and, unless SPAWN_NOWAIT, block until it finishes. Returns
// the child's pid, or -1 on failure. The child stays an unreaped zombie until
// its parent collects it with task_wait_ex().
int task_spawn(const char *name, int argc, char *const argv[], int flags);

#define WAIT_NOHANG 1  // do not block if no matching child has exited

// Reap a finished child of the current process (any of its threads may
// reap): `pid`, or any child for -1. Blocks until one finishes unless
// WAIT_NOHANG. Stores the exit code in *code (if not NULL) and returns the
// child's pid; returns 0 for WAIT_NOHANG with nothing to reap yet, and -1 if
// there is no such child (or the waiting thread was killed).
int task_wait_ex(int pid, int *code, int flags);

// Reap child `pid`, blocking until it exits; returns its exit code, or -1.
int task_wait(int pid);

// Block the current task for at least `ms` milliseconds (rounded up to ticks).
void task_msleep(uint64_t ms);

// Sleep on `wq` until wait_queue_wake_all(). Call holding `cond`, the
// spinlock protecting the condition being waited for (taken with
// spin_lock_irqsave), after checking it: the task is queued before `cond` is
// released, so a waker, which takes `cond` to change the condition, cannot slip
// in between. Returns holding `cond` again. A killed task is woken too (and
// does not sleep at all): callers that loop must check task_killed().
void wait_queue_sleep(struct wait_queue *wq, struct spinlock *cond);

// Make every task sleeping on `wq` runnable. Safe from interrupt handlers.
void wait_queue_wake_all(struct wait_queue *wq);

// Called by the timer interrupt on every tick: wakes due sleepers and asks
// for a reschedule (taken on the way back to EL0).
void sched_tick(uint64_t now);

// On the way back to EL0 (from src/arch/vectors.S): switch tasks if a
// reschedule was requested, unless the task is forbid()den, and let a killed
// task die. Called with IRQs masked and without the BKL (unless forbidden).
void sched_preempt_check(void);

// Make the boot thread task 0 of this core; it starts out holding the BKL.
void task_init_boot(void);

// On a secondary core: give it an idle task (pid 0) to be "current".
void task_init_idle(void);

// Run this core's idle loop for good: schedule its tasks, WFI in between
// (the end of a secondary core's bring-up).
void task_idle_forever(void) __attribute__((noreturn));

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

// pid of the current process (0 for the kernel's own boot task).
int task_getpid(void);

// The current user task, or NULL while the kernel's boot task runs.
task_t *task_current(void);

// The current task's process, or NULL while the kernel's boot task runs.
struct process *proc_current(void);

// The running task, including the boot task (never NULL).
task_t *task_running(void);

// exit(): end the current process with `code`. The other threads are marked
// killed; this one exits at once and never returns.
void task_exit(int code) __attribute__((noreturn));

// Threads (syscalls 20 to 22). thread_create starts a thread of the current
// process at `entry` (EL0) with x0/x1 = a0/a1 and a fresh user stack of
// THREAD_STACK_PAGES, on `core` for life (-1: the least-loaded online core).
// Returns its id, or -1 (no such core, out of memory or table slots, or the
// process is exiting).
int task_thread_create(uint64_t entry, uint64_t a0, uint64_t a1, int core);

// End the calling thread with `value` for thread_join. The last thread to exit
// ends its process (with code 0 unless exit() or kill() set one).
void task_thread_exit(long value) __attribute__((noreturn));

// Wait for thread `tid` of the current process to exit, store its value in
// *value (if not NULL) and free it. Returns 0, or -1 for no such thread, the
// caller itself, a thread already joined, or a killed caller.
int task_thread_join(int tid, long *value);

// kill (syscall 23): end process `pid` as exit(EXIT_KILLED) would, from
// outside; its threads die on their own cores. Returns 0, or -1 if there is no
// such live process.
int task_kill(int pid);

// True if the current task was killed: a blocking loop must give up and
// return, so the task can die on its way back to EL0.
bool task_killed(void);

// Die now if the current task was killed (on kernel entry from EL0, holding
// the BKL). Returns otherwise.
void task_die_if_killed(void);

// Give up the CPU to another runnable task, if there is one.
void task_yield(void);

// Release every process and task-table slot and their held memory. The kernel
// calls this to tear down after the top-level process and any descendants
// finish.
void task_reap_all(void);

// Kernel stack bytes a task has used at most (from the untouched fill pattern),
// and the most any task has used since boot.
size_t task_kstack_peak(const task_t *t);
size_t task_kstack_peak_max(void);

// task_info syscall (19): the index-th live task (thread), in the layout of
// user/libk/koraos.h's struct task_info. Returns 1, 0 past the last, -1 for a
// bad pointer.
struct ktask_info {
    int32_t pid;     // its process
    int32_t ppid;    // that process's parent (0: the kernel, or orphaned)
    uint32_t core;
    uint32_t state;  // 0 running, 1 ready, 2 sleeping, 3 zombie (exited)
    char name[TASK_NAME_MAX];
    int32_t tid;     // the thread itself (== pid for the main thread)
};
int task_info_get(unsigned index, struct ktask_info *out);

// Live tasks (threads) and processes, zombies included (for sysinfo).
void task_counts(unsigned *tasks, unsigned *procs);

// Visit every live task (for diagnostics).
void task_for_each(void (*fn)(const task_t *t, void *ctx), void *ctx);

// --- implemented in src/arch/entry.S ---
// Save the callee-saved context into *from and resume *to.
void cpu_switch(struct cpu_context *from, struct cpu_context *to);
// First return of a new task: release the sched_lock it inherited
// (schedule_tail), pass sched_preempt_check (it may have been killed before it
// ever ran), restore its trap frame (at sp) and eret to EL0.
void ret_to_user(void);
void schedule_tail(void);
