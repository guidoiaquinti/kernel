// SPDX-License-Identifier: GPL-3.0-or-later
//
// Processes, their threads (tasks) and kernel stacks, the scheduler and the
// big kernel lock. See proc/task.h and docs/locking.md.
//
// Locking:
//   * The BKL serializes everything tasks do in the kernel: a task holds it
//     from kernel entry (from EL0) until it returns to EL0 or blocks. The
//     process and task tables, parent links, fd tables and so on are
//     BKL-protected; so are thread creation, exit and kill, which is why a
//     dying thread takes the BKL first.
//   * sched_lock (IRQ-safe) protects task states and the scheduling fields
//     (blocked_on, wait queues, wake_tick), because interrupt handlers, which
//     never take the BKL, wake tasks and run the tick. It is held across the
//     context switch and released by the task switched to.
//   * Order: BKL, then a subsystem lock (tty, heap, ...), then sched_lock.
//     A task blocks by marking itself under sched_lock first and only then
//     dropping the BKL, so a waker that needs the BKL cannot miss it.

#include "proc/task.h"
#include "arch/irq.h"
#include "arch/percpu.h"
#include "arch/spinlock.h"
#include "lib/panic.h"
#include "arch/systick.h"
#include "user/elf.h"
#include "fs/fat32.h"
#include "mm.h"
#include "mm/frame_alloc.h"
#include "mm/kmalloc.h"
#include "proc/placement.h"
#include "proc/user_mem.h"
#include "lib/printf.h"
#include "lib/string.h"

#define KSTACK_SIZE (KSTACK_PAGES * PAGE_SIZE)
#define KSTACK_FILL 0x6b6b6b6b6b6b6b6bull  // untouched kernel stack words
#define KSTACK_GUARD_WORDS 8               // must still hold the fill on a switch

static task_t tasks[MAX_TASKS];
static struct process procs[MAX_PROCS];
static int next_pid = 1;
static size_t kstack_peak_max;  // deepest kernel stack use of any reaped task

// The boot thread: kernel_main runs as task 0 on the boot stack. It spawns
// init and waits for it like any parent, but has no process of its own.
static task_t boot_task = {
    .pid = 0,
    .state = TASK_RUNNABLE,
    .name = "kernel",
};

static struct spinlock sched_lock = SPINLOCK_INIT("sched");
static struct spinlock bkl = SPINLOCK_INIT("bkl");
static task_t *volatile bkl_holder;  // diagnostics

#define current (this_cpu()->curr)

// Every core's idle task: what it runs when none of its tasks can. It waits
// for interrupts and looks for work after each one (its own tick, every 10 ms,
// at the latest: there are no inter-processor interrupts). A secondary core's
// idle task is its boot thread; core 0's has a stack of its own, because core
// 0's boot thread is task 0, which blocks like any other task.
static task_t idle_tasks[MAX_CPUS];
static uint8_t idle0_stack[KSTACK_PAGES * PAGE_SIZE] __attribute__((aligned(16)));

static void idle_setup(task_t *t, unsigned core) {
    t->pid = 0;
    t->core = core;
    t->state = TASK_RUNNABLE;
    const char name[] = "idle";
    for (unsigned i = 0; i < 4; i++) {
        t->name[i] = name[i];
    }
    t->name[4] = (char)('0' + core);
    cpus[core].idle = t;
}

void task_init_idle(void) {
    struct cpu *c = this_cpu();
    idle_setup(&idle_tasks[c->id], c->id);
    c->curr = c->idle;
    c->curr->on_cpu = true;
}

static void idle_entry(void);

void task_init_boot(void) {
    struct cpu *c = this_cpu();
    boot_task.core = 0;
    boot_task.on_cpu = true;
    c->curr = &boot_task;
    idle_setup(&idle_tasks[0], 0);
    idle_tasks[0].ctx.lr = (uint64_t)idle_entry;
    idle_tasks[0].ctx.sp = (uint64_t)(idle0_stack + sizeof(idle0_stack));
    // kernel_main runs in the kernel, so it holds the BKL like any task would.
    spin_lock(&bkl);
    boot_task.bkl = true;
    bkl_holder = &boot_task;
}

// The big kernel lock ---------------------------------------------------

// Take the BKL, with IRQs open while waiting for it: the holder may be on
// another core for a while, and this core still serves interrupts meanwhile
// (handlers never take the BKL). Returns with the caller's IRQ mask.
static void bkl_take(void) {
    uint64_t flags = irq_save();
    while (!spin_trylock(&bkl)) {
        asm volatile("msr daifclr, #2\n\tyield\n\tmsr daifset, #2" ::: "memory");
    }
    bkl_holder = current;
    irq_restore(flags);
}

// Drop the BKL around a switch if the running task holds it; it stays the
// task's (t->bkl), to be retaken when the task runs again.
static bool bkl_drop(void) {
    if (!current->bkl) {
        return false;
    }
    bkl_holder = NULL;
    spin_unlock(&bkl);
    return true;
}

void bkl_enter_from_el0(void) {
    task_t *t = current;
    if (!t->bkl) {  // a forbid()den task kept it
        bkl_take();
        t->bkl = true;
    }
}

void bkl_exit_to_el0(void) {
    task_t *t = current;
    if (t->bkl && t->forbid == 0) {
        t->bkl = false;
        bkl_holder = NULL;
        spin_unlock(&bkl);
    }
}

void bkl_release_for_good(void) {
    bkl_exit_to_el0();
}

int bkl_holder_pid(void) {
    task_t *t = bkl_holder;
    return t != NULL ? t->pid : -1;
}

int task_forbid(void) {
    return ++current->forbid;
}

int task_permit(void) {
    task_t *t = current;
    if (t->forbid == 0) {
        return -1;
    }
    return --t->forbid;
}

static void task_free(task_t *t);
static void proc_free(struct process *p);
static void wait_off_cpu(const task_t *t);

// Free finished processes nobody will reap: their parent exited before them.
static void reap_orphans(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].pid != 0 && procs[i].zombie && procs[i].orphan) {
            proc_free(&procs[i]);
        }
    }
}

// Grab a free table slot and give it a fresh id. Returns NULL if the table is
// full. The slot stays UNUSED, so the scheduler ignores it, until its creator
// has finished setting it up and makes it RUNNABLE.
static task_t *task_alloc(void) {
    reap_orphans();
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_UNUSED && t->pid == 0) {
            memset(t, 0, sizeof(*t));
            t->pid = next_pid++;
            return t;
        }
    }
    return NULL;
}

// A free process slot, zeroed, or NULL. It counts as free (pid 0) until its
// main thread exists and gives it its id.
static struct process *proc_alloc(void) {
    reap_orphans();
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].pid == 0) {
            memset(&procs[i], 0, sizeof(procs[i]));
            return &procs[i];
        }
    }
    return NULL;
}

static struct process *proc_find(int pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (pid > 0 && procs[i].pid == pid) {
            return &procs[i];
        }
    }
    return NULL;
}

static void set_state(task_t *t, task_state_t state) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    t->state = state;
    spin_unlock_irqrestore(&sched_lock, flags);
}

// Release a thread's stacks and FP/SIMD state and return its slot to the
// pool. Never called on the running task: its kernel stack is in use.
static void task_free(task_t *t) {
    wait_off_cpu(t);
    size_t peak = task_kstack_peak(t);
    if (peak > kstack_peak_max) {
        kstack_peak_max = peak;
    }
    fpsimd_release(t);
    if (t->stack != NULL) {
        frame_free_pages(t->stack, t->stack_pages);
        t->stack = NULL;
    }
    if (t->kstack != NULL) {
        frame_free_pages(t->kstack, KSTACK_PAGES);
        t->kstack = NULL;
    }
    t->proc = NULL;
    set_state(t, TASK_UNUSED);
    t->pid = 0;
}

// Release a process: whatever threads it still has (all exited, or never
// started), its files, cwd and memory, and its slot.
static void proc_free(struct process *p) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid != 0 && tasks[i].proc == p) {
            task_free(&tasks[i]);
        }
    }
    fs_cwd_release(&p->cwd);
    for (int f = 0; f < MAX_OPEN_FILES; f++) {
        if (p->files[f].used) {
            fat32_close(&p->files[f].file);
            p->files[f].used = false;
        }
    }
    if (p->image != NULL) {
        frame_free_pages(p->image, p->image_pages);
        p->image = NULL;
    }
    user_pages_release_all(p);
    p->pid = 0;
}

// Scheduling ---------------------------------------------------------------
//
// Every task belongs to one core for life (t->core, chosen at creation); a
// core only ever runs its own tasks, so the table filtered by core is that
// core's run queue. One IRQ-safe sched_lock covers all of them: wake-ups often
// cross cores (a keyboard interrupt on core 0 wakes a shell on core 1, a child
// exiting wakes its parent elsewhere), and with at most four cores under the
// BKL a lock per queue would only add lock ordering. The woken task's core
// notices on its next interrupt (its tick, within 10 ms): no IPIs.

// The next runnable task of `core` after `from` in round-robin order over the
// boot task (core 0) and the table, or `from` itself if nothing else can run,
// or NULL. Idle tasks are not candidates: switch_away falls back to them.
static task_t *pick_next(unsigned core, task_t *from) {
    int slots = MAX_TASKS + 1;  // slot 0 is the boot task
    int start = 0;
    if (from >= tasks && from < tasks + MAX_TASKS) {
        start = (int)(from - tasks) + 1;
    }
    for (int i = 1; i <= slots; i++) {
        int slot = (start + i) % slots;
        task_t *t = slot == 0 ? &boot_task : &tasks[slot - 1];
        if (t->state == TASK_RUNNABLE && t->core == core) {
            return t;
        }
    }
    return NULL;
}

// After a context switch, on the incoming task's stack: the outgoing task is
// now off its stack (an exited one may be freed from here on).
static void finish_switch(void) {
    struct cpu *c = this_cpu();
    __atomic_store_n(&c->switched_from->on_cpu, false, __ATOMIC_RELEASE);
}

// A task's kernel stack must not be freed while its core is still switching
// away from it (it exited, but cpu_switch has not left its stack yet).
static void wait_off_cpu(const task_t *t) {
    while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
        asm volatile("yield");
    }
}

static void check_kstack(const task_t *t) {
    if (t->kstack == NULL) {
        return;  // the boot task runs on the boot stack
    }
    const uint64_t *guard = t->kstack;
    for (int i = 0; i < KSTACK_GUARD_WORDS; i++) {
        if (guard[i] != KSTACK_FILL) {
            panic("kernel stack overflow in pid %d (%s)", t->pid, t->name);
        }
    }
}

// Switch to this core's next runnable task, or its idle task if there is
// none, with sched_lock held and IRQs masked; the caller has already set the
// running task's new state. Returns when this task is switched back in, with
// sched_lock held again (taken over from the task that switched to us).
static void switch_away(void) {
    struct cpu *c = this_cpu();
    task_t *prev = c->curr;
    if (c->locks_held != 1) {
        panic("switching away from pid %d with %d spinlocks held", prev->pid,
              c->locks_held);
    }
    task_t *next = pick_next(c->id, prev);
    if (next == NULL) {
        next = c->idle;
    }
    if (next != prev) {
        check_kstack(prev);
        c->curr = next;
        c->switched_from = prev;
        next->on_cpu = true;
        fpsimd_switch_to(next);
        cpu_switch(&prev->ctx, &next->ctx);
        finish_switch();
    }
}

// The first thing a new task runs (from ret_to_user): release the
// sched_lock the switching task held for it.
void schedule_tail(void) {
    finish_switch();
    spin_unlock(&sched_lock);
}

// The idle loop: hand the core to any runnable task of its, otherwise wait
// for an interrupt. Runs with IRQs masked except in WFI.
static void idle_loop(void) __attribute__((noreturn));
static void idle_loop(void) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sched_lock);
        switch_away();  // returns at once if nothing is runnable
        spin_unlock_irqrestore(&sched_lock, flags);
        asm volatile("msr daifclr, #2\n\twfi\n\tmsr daifset, #2" ::: "memory");
    }
}

// Core 0's idle task starts here the first time it is switched to.
static void idle_entry(void) {
    schedule_tail();
    idle_loop();
}

void task_idle_forever(void) {
    irq_disable();
    idle_loop();
    __builtin_unreachable();
}

// Give up the CPU after the caller marked the running task under sched_lock
// (taken with spin_lock_irqsave, `flags`): drop the BKL, switch, then take it
// back when this task runs again.
static void block_and_switch(uint64_t flags) {
    bool had_bkl = bkl_drop();
    switch_away();
    spin_unlock_irqrestore(&sched_lock, flags);
    if (had_bkl) {
        bkl_take();
    }
}

// Let other runnable tasks have the CPU; the running task stays runnable.
static void schedule(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    block_and_switch(flags);
}

static void make_runnable(task_t *t) {
    t->state = TASK_RUNNABLE;
    t->blocked_on = BLOCK_NONE;
    t->waiting_for = NULL;
    t->wq = NULL;
    t->wq_next = NULL;
    cpus[t->core].need_resched = true;  // its own core, maybe another one
}

// Is `t` blocked for `why` on `what` (for BLOCK_CHILD, or on any child)?
static bool blocked_for(const task_t *t, block_reason_t why, const void *what) {
    return t->state == TASK_BLOCKED && t->blocked_on == why &&
           (t->waiting_for == what || (why == BLOCK_CHILD && t->waiting_for == NULL));
}

// Block the running task until child process `child` (or, if NULL, any
// child) finishes. Does not block a killed task.
static void block_on_child(struct process *child) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (!current->killed && (child == NULL || !child->zombie)) {
        current->state = TASK_BLOCKED;
        current->blocked_on = BLOCK_CHILD;
        current->waiting_for = child;
    }
    block_and_switch(flags);
}

// Block the running task until thread `t` exits. Does not block a killed task.
static void block_on_join(task_t *t) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (!current->killed && t->state != TASK_EXITED) {
        current->state = TASK_BLOCKED;
        current->blocked_on = BLOCK_JOIN;
        current->waiting_for = t;
    }
    block_and_switch(flags);
}

void task_yield(void) {
    schedule();
}

void wait_queue_sleep(struct wait_queue *wq, struct spinlock *cond) {
    task_t *t = current;
    if (t->killed) {
        return;  // its caller gives up and lets it die
    }
    spin_lock(&sched_lock);  // IRQs are already masked: the caller holds `cond`
    t->state = TASK_BLOCKED;
    t->blocked_on = BLOCK_QUEUE;
    t->wq = wq;
    t->wq_next = wq->head;
    wq->head = t;
    spin_unlock(cond);
    bool had_bkl = bkl_drop();
    switch_away();
    spin_unlock(&sched_lock);
    if (had_bkl) {
        bkl_take();
    }
    spin_lock(cond);
}

void wait_queue_wake_all(struct wait_queue *wq) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    task_t *t = wq->head;
    wq->head = NULL;
    while (t != NULL) {
        task_t *next = t->wq_next;
        make_runnable(t);
        t = next;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

void task_msleep(uint64_t ms) {
    uint64_t hz = systick_hz();
    uint64_t ticks = hz != 0 ? (ms * hz + 999) / 1000 : 0;
    if (ticks == 0) {
        task_yield();
        return;
    }
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (!current->killed) {
        current->state = TASK_BLOCKED;
        current->blocked_on = BLOCK_SLEEP;
        // The current tick is already partly over: one more guarantees the minimum.
        current->wake_tick = systick_count() + ticks + 1;
    }
    block_and_switch(flags);
}

static void tick_task(task_t *t, uint64_t now) {
    if (t->state == TASK_BLOCKED && t->blocked_on == BLOCK_SLEEP && now >= t->wake_tick) {
        make_runnable(t);
    }
}

// Every core's own tick: wake this core's due sleepers and end the running
// task's slice. `now` is the system (core 0) tick count.
void sched_tick(uint64_t now) {
    struct cpu *c = this_cpu();
    spin_lock(&sched_lock);  // in the timer interrupt: IRQs masked
    if (c->id == 0) {
        tick_task(&boot_task, now);
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].core == c->id) {
            tick_task(&tasks[i], now);
        }
    }
    c->need_resched = true;  // round robin: the running slice is over
    spin_unlock(&sched_lock);
}

static void thread_die(long value) __attribute__((noreturn));

void sched_preempt_check(void) {
    struct cpu *c = this_cpu();
    // A forbid()den task keeps its core, as on AmigaOS (until it blocks).
    if (c->need_resched && c->curr->forbid == 0) {
        c->need_resched = false;
        schedule();
    }
    // A killed thread dies here rather than return to EL0: after a syscall, a
    // tick, or being switched back in (or in for the first time).
    if (__atomic_load_n(&c->curr->killed, __ATOMIC_RELAXED)) {
        bkl_enter_from_el0();  // thread exit is BKL-protected (a forbidden one holds it)
        thread_die(0);
    }
}

static size_t cstr_len(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void copy_bytes(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

// Lay out argc argument strings and a NULL-terminated argv[] array at the top of
// a task's stack, so the program can be entered as main(argc, argv). On success
// writes the new 16-byte-aligned stack pointer and the address of the argv array
// (both in the task's address space) and returns true; returns false if the
// arguments do not fit in the stack page.
static bool build_args(void *stack, int argc, char *const argv[],
                       uint64_t *out_sp, uint64_t *out_argv) {
    uint64_t base = (uint64_t)stack;
    uint64_t p = base + PAGE_SIZE;
    uint64_t str_addr[MAX_ARGS];

    for (int i = 0; i < argc; i++) {
        size_t len = cstr_len(argv[i]) + 1;
        if (p < base + len) {
            return false;
        }
        p -= len;
        copy_bytes((void *)p, argv[i], len);
        str_addr[i] = p;
    }

    // argv[]: argc + 1 pointers (NULL-terminated), 16-byte aligned.
    p &= ~(uint64_t)15;
    uint64_t need = (uint64_t)(argc + 1) * sizeof(uint64_t);
    if (p < base + need + 16) {
        return false;
    }
    p -= need;
    p &= ~(uint64_t)15;
    uint64_t *arr = (uint64_t *)p;
    for (int i = 0; i < argc; i++) {
        arr[i] = str_addr[i];
    }
    arr[argc] = 0;

    *out_argv = p;
    *out_sp = p - 16;  // leave a small gap below argv[]; stays 16-aligned
    return true;
}

// Read a program off the filesystem and load it into memory via elf_load. The
// file is read into a scratch buffer, which elf_load copies out of, so the
// buffer is freed before returning. Returns 0, or -1 on any failure (a missing
// file is reported silently so the caller -- e.g. the shell -- can react).
static int load_program(const char *name, struct loaded_prog *lp) {
    fs_cwd_t *initial = NULL;
    const fs_cwd_t *cwd = proc_current() != NULL ? &proc_current()->cwd : NULL;
    if (cwd == NULL) {
        initial = kmalloc(sizeof(*initial));
        if (initial == NULL || fs_boot_cwd(initial) != 0) {
            kfree(initial);
            return -1;
        }
        cwd = initial;
    }
    fat32_file_t f;
    int rc = fs_program_open(cwd, name, &f);
    fs_cwd_release(initial);
    kfree(initial);
    if (rc != 0) return -1;
    if (f.size == 0) {
        fat32_close(&f);
        return -1;
    }

    size_t npages = ((size_t)f.size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *buf = frame_alloc_pages(npages);
    if (buf == NULL) {
        fat32_close(&f);
        printf("spawn: out of memory reading '%s'\n", name);
        return -1;
    }
    uint32_t got = 0;
    while (got < f.size) {
        long n = fat32_read(&f, buf + got, f.size - got);
        if (n <= 0) {
            frame_free_pages(buf, npages);
            fat32_close(&f);
            printf("spawn: read('%s') failed\n", name);
            return -1;
        }
        got += (uint32_t)n;
    }

    rc = elf_load(buf, f.size, lp);
    fat32_close(&f);
    frame_free_pages(buf, npages);
    if (rc != 0) {
        printf("spawn: elf_load('%s') failed: %d\n", name, rc);
        return -1;
    }
    return 0;
}

// The core a new task gets: `requested` if it is online (else -1), or for -1
// the online core with the fewest live tasks (runnable or blocked).
static int choose_core(int requested) {
    unsigned load[MAX_CPUS] = {0};
    bool online[MAX_CPUS];
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        online[i] = i == 0 || __atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE);
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        task_state_t st = tasks[i].state;
        if (st == TASK_RUNNABLE || st == TASK_BLOCKED) {
            load[tasks[i].core]++;
        }
    }
    return sched_place(load, online, MAX_CPUS, requested);
}

// Fill a new thread's kernel stack, so its high-water mark can be measured,
// then put its initial EL0 state at the top: the first switch to it "returns"
// through ret_to_user and erets to `entry` on user stack `sp`, with x0/x1 =
// a0/a1.
static void thread_setup(task_t *t, uint64_t entry, uint64_t sp, uint64_t a0, uint64_t a1) {
    uint64_t *words = t->kstack;
    for (size_t i = 0; i < KSTACK_SIZE / sizeof(uint64_t); i++) {
        words[i] = KSTACK_FILL;
    }
    t->tf = (struct trapframe *)((uint8_t *)t->kstack + KSTACK_SIZE - sizeof(struct trapframe));
    memset(t->tf, 0, sizeof(*t->tf));
    t->tf->elr = entry;
    t->tf->sp = sp;
    t->tf->spsr = 0;  // EL0t, interrupts unmasked
    t->tf->regs[0] = a0;
    t->tf->regs[1] = a1;
    t->ctx.lr = (uint64_t)ret_to_user;
    t->ctx.sp = (uint64_t)t->tf;
}

static void copy_name(char *dst, const char *src) {
    size_t n = 0;
    for (; src[n] != '\0' && n < TASK_NAME_MAX - 1; n++) {
        dst[n] = src[n];
    }
    dst[n] = '\0';
}

// A new thread of `p` for `core`: a table slot, a kernel stack and
// `stack_pages` of user stack, still UNUSED (not yet runnable). NULL if the
// table or memory runs out; *full tells which.
static task_t *thread_alloc(struct process *p, unsigned core, size_t stack_pages, bool *full) {
    void *stack = frame_alloc_pages(stack_pages);
    void *kstack = frame_alloc_pages(KSTACK_PAGES);
    task_t *t = stack != NULL && kstack != NULL ? task_alloc() : NULL;
    if (t == NULL) {
        frame_free_pages(stack, stack_pages);
        frame_free_pages(kstack, KSTACK_PAGES);
        *full = stack != NULL && kstack != NULL;
        return NULL;
    }
    t->proc = p;
    t->core = core;
    t->stack = stack;
    t->stack_pages = stack_pages;
    t->kstack = kstack;
    copy_name(t->name, p->name);
    return t;
}

struct process *task_create(const char *name, int argc, char *const argv[], int core) {
    core = choose_core(core);
    if (core < 0) {
        return NULL;  // a requested core that does not exist or is offline
    }
    struct loaded_prog lp;
    if (load_program(name, &lp) != 0) {
        return NULL;
    }
    struct process *p = proc_alloc();
    if (p == NULL) {
        frame_free_pages(lp.image, lp.image_pages);
        printf("spawn: process table full\n");
        return NULL;
    }
    p->image = lp.image;
    p->image_pages = lp.image_pages;
    p->parent = proc_current();
    copy_name(p->name, name);

    bool full;
    task_t *t = thread_alloc(p, (unsigned)core, 1, &full);
    if (t == NULL) {
        printf(full ? "spawn: task table full\n" : "spawn: out of memory for '%s' stacks\n", name);
        proc_free(p);  // releases the image
        return NULL;
    }
    p->pid = t->pid;
    p->threads = 1;

    int cwd_rc = p->parent != NULL ? fs_cwd_copy(&p->cwd, &p->parent->cwd)
                                   : fs_boot_cwd(&p->cwd);
    if (cwd_rc != 0) {
        proc_free(p);
        return NULL;
    }

    uint64_t sp = (uint64_t)t->stack + PAGE_SIZE;
    uint64_t argv_child = 0;
    if (argc > MAX_ARGS) {
        argc = MAX_ARGS;
    }
    if (argc > 0) {
        if (!build_args(t->stack, argc, argv, &sp, &argv_child)) {
            printf("spawn: arguments too large for '%s'\n", name);
            proc_free(p);  // releases the image and stacks we just took
            return NULL;
        }
    }

    thread_setup(t, lp.entry, sp, (uint64_t)argc, argv_child);
    printf("  [pid %d] run '%s' on cpu %u: entry=0x%lx sp=0x%lx (%d image pages)\n",
           t->pid, name, t->core, lp.entry, sp, (int)p->image_pages);
    set_state(t, TASK_RUNNABLE);
    return p;
}

int task_spawn(const char *name, int argc, char *const argv[], int flags) {
    struct process *child = task_create(name, argc, argv, SPAWN_CORE_OF(flags));
    if (child == NULL) {
        return -1;
    }
    int pid = child->pid;
    if ((flags & SPAWN_NOWAIT) == 0) {
        // Another thread of ours may reap the child meanwhile, freeing its slot.
        while (child->pid == pid && !child->zombie && !task_killed()) {
            block_on_child(child);
        }
    }
    return pid;
}

int task_wait_ex(int pid, int *code, int flags) {
    struct process *me = proc_current();
    for (;;) {
        struct process *match = NULL;
        for (int i = 0; i < MAX_PROCS; i++) {
            struct process *p = &procs[i];
            if (p->pid == 0 || p->parent != me || p->orphan || (pid != -1 && p->pid != pid)) {
                continue;
            }
            if (match == NULL || (!match->zombie && p->zombie)) {
                match = p;
            }
        }
        if (match == NULL) {
            return -1;
        }
        if (match->zombie) {
            int found = match->pid;
            if (code != NULL) {
                *code = match->exit_code;
            }
            proc_free(match);
            return found;
        }
        if (flags & WAIT_NOHANG) {
            return 0;
        }
        if (task_killed()) {
            return -1;
        }
        block_on_child(pid == -1 ? NULL : match);
    }
}

int task_wait(int pid) {
    int code;
    return task_wait_ex(pid, &code, 0) > 0 ? code : -1;
}

int task_getpid(void) {
    struct process *p = current->proc;
    return p != NULL ? p->pid : 0;
}

task_t *task_current(void) {
    task_t *t = current;
    return t != &boot_task && t != this_cpu()->idle ? t : NULL;
}

struct process *proc_current(void) {
    return current->proc;
}

task_t *task_running(void) {
    return current;
}

// Threads, exit and kill -------------------------------------------------
//
// All under the BKL. A thread that has to die elsewhere is only marked
// killed: it notices on its own core, on its next kernel entry
// (task_die_if_killed) or on its way back to EL0 (sched_preempt_check), so no
// other core ever needs interrupting. A blocked one is woken for it, and its
// blocking loop gives up (task_killed) and unwinds the syscall normally, so it
// holds no lock or buffer when it dies.

// Mark every live thread of `p` killed (the caller's too, if it is one) and
// wake those that are blocked.
static void kill_threads(struct process *p) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->proc != p || (t->state != TASK_RUNNABLE && t->state != TASK_BLOCKED)) {
            continue;
        }
        __atomic_store_n(&t->killed, true, __ATOMIC_RELAXED);
        if (t->state == TASK_BLOCKED) {
            if (t->blocked_on == BLOCK_QUEUE) {
                for (task_t **link = &t->wq->head; *link != NULL; link = &(*link)->wq_next) {
                    if (*link == t) {
                        *link = t->wq_next;
                        break;
                    }
                }
            }
            make_runnable(t);
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

// Under sched_lock: wake whoever waits for `p` to finish, the threads of its
// parent process (or the boot task, for the kernel's children).
static void wake_parent(struct process *p) {
    if (p->orphan) {
        return;
    }
    if (p->parent == NULL) {
        if (blocked_for(&boot_task, BLOCK_CHILD, p)) {
            make_runnable(&boot_task);
        }
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].proc == p->parent && blocked_for(&tasks[i], BLOCK_CHILD, p)) {
            make_runnable(&tasks[i]);
        }
    }
}

// End the running thread for good. The last thread of a process makes it a
// zombie: its children become orphans and its parent is woken.
static void thread_die(long value) {
    task_t *me = current;
    struct process *p = me->proc;
    me->forbid = 0;  // exiting ends a forbid(), as Permit() would
    fpsimd_release(me);
    me->exit_value = value;
    bool last = --p->threads == 0;
    if (last) {
        // Children outlive us as orphans: reaped as soon as they are zombies.
        for (int i = 0; i < MAX_PROCS; i++) {
            struct process *child = &procs[i];
            if (child->pid != 0 && child->parent == p) {
                child->parent = NULL;
                child->orphan = true;
                if (child->zombie) {
                    proc_free(child);
                }
            }
        }
    }

    uint64_t flags = spin_lock_irqsave(&sched_lock);
    me->state = TASK_EXITED;
    if (last) {
        p->zombie = true;
        wake_parent(p);
    } else {
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].proc == p && blocked_for(&tasks[i], BLOCK_JOIN, me)) {
                make_runnable(&tasks[i]);
            }
        }
    }
    block_and_switch(flags);  // never picks this task again
    panic("exited task %d was switched back in", me->pid);
}

// End process `p` with `code` (unless it is already ending): every live
// thread is marked killed.
static void proc_exit(struct process *p, int code) {
    if (!p->exiting) {
        p->exiting = true;
        p->exit_code = code;
    }
    kill_threads(p);
}

void task_exit(int code) {
    proc_exit(current->proc, code);
    thread_die(0);
}

void task_thread_exit(long value) {
    thread_die(value);
}

int task_thread_create(uint64_t entry, uint64_t a0, uint64_t a1, int core) {
    struct process *p = proc_current();
    if (p == NULL || p->exiting) {
        return -1;
    }
    core = choose_core(core);
    if (core < 0) {
        return -1;
    }
    bool full;
    task_t *t = thread_alloc(p, (unsigned)core, THREAD_STACK_PAGES, &full);
    if (t == NULL) {
        return -1;
    }
    p->threads++;
    thread_setup(t, entry, (uint64_t)t->stack + THREAD_STACK_PAGES * PAGE_SIZE, a0, a1);
    set_state(t, TASK_RUNNABLE);
    return t->pid;
}

int task_thread_join(int tid, long *value) {
    task_t *me = current;
    for (;;) {
        task_t *t = NULL;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tid > 0 && tasks[i].pid == tid && tasks[i].state != TASK_UNUSED) {
                t = &tasks[i];
            }
        }
        if (t == NULL || t == me || me->proc == NULL || t->proc != me->proc) {
            return -1;
        }
        if (t->state == TASK_EXITED) {
            if (value != NULL) {
                *value = t->exit_value;
            }
            task_free(t);
            return 0;
        }
        if (task_killed()) {
            return -1;
        }
        block_on_join(t);
    }
}

int task_kill(int pid) {
    struct process *p = proc_find(pid);
    if (p == NULL || p->zombie) {
        return -1;
    }
    proc_exit(p, EXIT_KILLED);
    return 0;
}

bool task_killed(void) {
    return __atomic_load_n(&current->killed, __ATOMIC_RELAXED);
}

void task_die_if_killed(void) {
    if (task_killed()) {
        thread_die(0);
    }
}

// Free every finished process left unreaped (at the end of the kernel's
// run). One still running keeps running.
void task_reap_all(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].pid != 0 && procs[i].zombie) {
            proc_free(&procs[i]);
        }
    }
}

size_t task_kstack_peak(const task_t *t) {
    if (t->kstack == NULL) {
        return 0;
    }
    const uint64_t *words = t->kstack;
    size_t n = KSTACK_SIZE / sizeof(uint64_t);
    size_t i = 0;
    while (i < n && words[i] == KSTACK_FILL) {
        i++;
    }
    return (n - i) * sizeof(uint64_t);
}

size_t task_kstack_peak_max(void) {
    size_t peak = kstack_peak_max;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && task_kstack_peak(&tasks[i]) > peak) {
            peak = task_kstack_peak(&tasks[i]);
        }
    }
    return peak;
}

int task_info_get(unsigned index, struct ktask_info *out) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    int found = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_UNUSED || index-- != 0) {
            continue;
        }
        struct process *p = t->proc;
        out->pid = p != NULL ? p->pid : t->pid;
        out->tid = t->pid;
        out->ppid = p != NULL && p->parent != NULL ? p->parent->pid : 0;
        out->core = t->core;
        out->state = t->state == TASK_EXITED    ? 3
                     : t->state == TASK_BLOCKED ? 2
                     : t->on_cpu                ? 0
                                                : 1;
        for (unsigned n = 0; n < TASK_NAME_MAX; n++) {
            out->name[n] = t->name[n];
        }
        found = 1;
        break;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return found;
}

void task_for_each(void (*fn)(const task_t *t, void *ctx), void *ctx) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            fn(&tasks[i], ctx);
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}
