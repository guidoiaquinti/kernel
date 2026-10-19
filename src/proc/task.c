// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tasks, their kernel stacks, the scheduler and the big kernel lock. See
// proc/task.h and docs/locking.md.
//
// Locking:
//   * The BKL serializes everything tasks do in the kernel: a task holds it
//     from kernel entry (from EL0) until it returns to EL0 or blocks. The task
//     table, parent links, fd tables and so on are BKL-protected.
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
#include "proc/user_mem.h"
#include "lib/printf.h"
#include "lib/string.h"

#define KSTACK_SIZE (KSTACK_PAGES * PAGE_SIZE)
#define KSTACK_FILL 0x6b6b6b6b6b6b6b6bull  // untouched kernel stack words
#define KSTACK_GUARD_WORDS 8               // must still hold the fill on a switch

static task_t tasks[MAX_TASKS];
static int next_pid = 1;
static size_t kstack_peak_max;  // deepest kernel stack use of any reaped task

// The boot thread: kernel_main runs as task 0 on the boot stack. It spawns
// init and waits for it like any parent, but has no user side of its own.
static task_t boot_task = {
    .pid = 0,
    .state = TASK_RUNNABLE,
    .name = "kernel",
};

static struct spinlock sched_lock = SPINLOCK_INIT("sched");
static struct spinlock bkl = SPINLOCK_INIT("bkl");
static task_t *volatile bkl_holder;  // diagnostics

#define current (this_cpu()->curr)

// Secondary cores' boot threads: what they run when they have no task (all
// of the time until Milestone 7 PR 5 gives them run queues).
static task_t idle_tasks[MAX_CPUS];

void task_init_idle(void) {
    struct cpu *c = this_cpu();
    task_t *t = &idle_tasks[c->id];
    t->pid = 0;
    t->state = TASK_RUNNABLE;
    t->name[0] = 'i';
    t->name[1] = 'd';
    t->name[2] = 'l';
    t->name[3] = 'e';
    t->name[4] = (char)('0' + c->id);
    c->idle = t;
    c->curr = t;
}

void task_init_boot(void) {
    struct cpu *c = this_cpu();
    c->idle = &boot_task;
    c->curr = &boot_task;
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

// Grab a free table slot and give it a fresh pid. Returns NULL if the table is
// full. Leaves the slot RUNNABLE; the caller fills in the rest.
static void task_free(task_t *t);

// Free zombies nobody will reap: their parent exited before them.
static void reap_orphans(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_EXITED && tasks[i].orphan && &tasks[i] != current) {
            task_free(&tasks[i]);
        }
    }
}

// The slot stays UNUSED, so the scheduler ignores it, until task_create()
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

static void set_state(task_t *t, task_state_t state) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    t->state = state;
    spin_unlock_irqrestore(&sched_lock, flags);
}

// Release a task's held memory and return its slot to the pool. Never called
// on the running task: its kernel stack is in use.
static void task_free(task_t *t) {
    size_t peak = task_kstack_peak(t);
    if (peak > kstack_peak_max) {
        kstack_peak_max = peak;
    }
    fpsimd_release(t);
    fs_cwd_release(&t->cwd);
    for (int f = 0; f < MAX_OPEN_FILES; f++) {
        if (t->files[f].used) {
            fat32_close(&t->files[f].file);
            t->files[f].used = false;
        }
    }
    if (t->image != NULL) {
        frame_free_pages(t->image, t->image_pages);
        t->image = NULL;
    }
    if (t->stack != NULL) {
        frame_free(t->stack);
        t->stack = NULL;
    }
    if (t->kstack != NULL) {
        frame_free_pages(t->kstack, KSTACK_PAGES);
        t->kstack = NULL;
    }
    user_pages_release_all(t);
    set_state(t, TASK_UNUSED);
    t->pid = 0;
}

static task_t *parent_of(const task_t *t) {
    return t->parent != NULL ? t->parent : &boot_task;
}

// Scheduling ---------------------------------------------------------------

// The next runnable task after `from` in round-robin order over the boot task
// and the table, or `from` itself if nothing else can run, or NULL.
static task_t *pick_next(task_t *from) {
    int slots = MAX_TASKS + 1;  // slot 0 is the boot task
    int start = from == &boot_task ? 0 : (int)(from - tasks) + 1;
    for (int i = 1; i <= slots; i++) {
        int slot = (start + i) % slots;
        task_t *t = slot == 0 ? &boot_task : &tasks[slot - 1];
        if (t->state == TASK_RUNNABLE) {
            return t;
        }
    }
    return NULL;
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

// Switch to the next runnable task, with sched_lock held and IRQs masked; the
// caller has already set the running task's new state. Returns when this task
// is switched back in, with sched_lock held again (taken over from the task
// that switched to us). With nothing runnable, waits for an interrupt, with
// sched_lock released so the handler can wake someone.
static void switch_away(void) {
    struct cpu *c = this_cpu();
    task_t *prev = c->curr;
    if (c->locks_held != 1) {
        panic("switching away from pid %d with %d spinlocks held", prev->pid,
              c->locks_held);
    }
    task_t *next = pick_next(prev);
    while (next == NULL) {
        spin_unlock(&sched_lock);
        asm volatile("msr daifclr, #2\n\twfi\n\tmsr daifset, #2" ::: "memory");
        spin_lock(&sched_lock);
        next = pick_next(prev);
    }
    if (next != prev) {
        check_kstack(prev);
        c->curr = next;
        fpsimd_switch_to(next);
        cpu_switch(&prev->ctx, &next->ctx);
    }
}

// The first thing a new task runs (from ret_to_user): release the
// sched_lock the switching task held for it.
void schedule_tail(void) {
    spin_unlock(&sched_lock);
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
    this_cpu()->need_resched = true;
}

// Block the running task until `child` (or, if NULL, any child) exits.
static void block_on_child(task_t *child) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (child == NULL || child->state != TASK_EXITED) {
        current->state = TASK_BLOCKED;
        current->blocked_on = BLOCK_CHILD;
        current->waiting_for = child;
    }
    block_and_switch(flags);
}

void task_yield(void) {
    schedule();
}

void wait_queue_sleep(struct wait_queue *wq, struct spinlock *cond) {
    spin_lock(&sched_lock);  // IRQs are already masked: the caller holds `cond`
    task_t *t = current;
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
    current->state = TASK_BLOCKED;
    current->blocked_on = BLOCK_SLEEP;
    // The current tick is already partly over: one more guarantees the minimum.
    current->wake_tick = systick_count() + ticks + 1;
    block_and_switch(flags);
}

static void tick_task(task_t *t, uint64_t now) {
    if (t->state == TASK_BLOCKED && t->blocked_on == BLOCK_SLEEP && now >= t->wake_tick) {
        make_runnable(t);
    }
}

void sched_tick(uint64_t now) {
    spin_lock(&sched_lock);  // in the timer interrupt: IRQs masked
    tick_task(&boot_task, now);
    for (int i = 0; i < MAX_TASKS; i++) {
        tick_task(&tasks[i], now);
    }
    this_cpu()->need_resched = true;  // round robin: the running slice is over
    spin_unlock(&sched_lock);
}

void sched_preempt_check(void) {
    struct cpu *c = this_cpu();
    // A forbid()den task keeps its core, as on AmigaOS (until it blocks).
    if (c->need_resched && c->curr->forbid == 0) {
        c->need_resched = false;
        schedule();
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
    const fs_cwd_t *cwd = task_current() != NULL ? &current->cwd : NULL;
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

task_t *task_create(const char *name, int argc, char *const argv[]) {
    struct loaded_prog lp;
    if (load_program(name, &lp) != 0) {
        return NULL;
    }

    void *stack = frame_alloc();
    void *kstack = frame_alloc_pages(KSTACK_PAGES);
    task_t *t = stack != NULL && kstack != NULL ? task_alloc() : NULL;
    if (t == NULL) {
        frame_free_pages(lp.image, lp.image_pages);
        frame_free(stack);
        frame_free_pages(kstack, KSTACK_PAGES);
        printf(stack != NULL && kstack != NULL ? "spawn: task table full\n"
                                               : "spawn: out of memory for '%s' stacks\n",
               name);
        return NULL;
    }
    t->parent = task_current();
    t->image = lp.image;
    t->image_pages = lp.image_pages;
    t->stack = stack;
    t->kstack = kstack;
    size_t n = 0;
    for (; name[n] != '\0' && n < TASK_NAME_MAX - 1; n++) {
        t->name[n] = name[n];
    }
    t->name[n] = '\0';

    int cwd_rc = t->parent != NULL ? fs_cwd_copy(&t->cwd, &t->parent->cwd)
                                   : fs_boot_cwd(&t->cwd);
    if (cwd_rc != 0) {
        task_free(t);
        return NULL;
    }

    uint64_t sp = (uint64_t)stack + PAGE_SIZE;
    uint64_t argv_child = 0;
    if (argc > MAX_ARGS) {
        argc = MAX_ARGS;
    }
    if (argc > 0) {
        if (!build_args(stack, argc, argv, &sp, &argv_child)) {
            printf("spawn: arguments too large for '%s'\n", name);
            task_free(t);  // releases the image and stacks we just took
            return NULL;
        }
    }

    // Fill the kernel stack so its high-water mark can be measured, then put
    // the initial EL0 state at its top: the first switch to the task "returns"
    // through ret_to_user and erets to the entry point with x0/x1 = argc/argv.
    uint64_t *words = kstack;
    for (size_t i = 0; i < KSTACK_SIZE / sizeof(uint64_t); i++) {
        words[i] = KSTACK_FILL;
    }
    t->tf = (struct trapframe *)((uint8_t *)kstack + KSTACK_SIZE - sizeof(struct trapframe));
    memset(t->tf, 0, sizeof(*t->tf));
    t->tf->elr = lp.entry;
    t->tf->sp = sp;
    t->tf->spsr = 0;  // EL0t, interrupts unmasked
    t->tf->regs[0] = (uint64_t)argc;
    t->tf->regs[1] = argv_child;
    t->ctx.lr = (uint64_t)ret_to_user;
    t->ctx.sp = (uint64_t)t->tf;

    printf("  [pid %d] run '%s': entry=0x%lx sp=0x%lx (%d image pages)\n",
           t->pid, name, lp.entry, sp, (int)t->image_pages);
    set_state(t, TASK_RUNNABLE);
    return t;
}

int task_spawn(const char *name, int argc, char *const argv[], int flags) {
    task_t *child = task_create(name, argc, argv);
    if (child == NULL) {
        return -1;
    }
    int pid = child->pid;
    if ((flags & SPAWN_NOWAIT) == 0) {
        while (child->state != TASK_EXITED) {
            block_on_child(child);
        }
    }
    return pid;
}

int task_wait_ex(int pid, int *code, int flags) {
    task_t *me = task_current();
    for (;;) {
        task_t *match = NULL;
        bool exited = false;
        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *t = &tasks[i];
            if (t->state == TASK_UNUSED || t->parent != me || t->orphan ||
                (pid != -1 && t->pid != pid)) {
                continue;
            }
            if (match == NULL || (!exited && t->state == TASK_EXITED)) {
                match = t;
                exited = t->state == TASK_EXITED;
            }
        }
        if (match == NULL) {
            return -1;
        }
        if (exited) {
            int found = match->pid;
            if (code != NULL) {
                *code = match->exit_code;
            }
            task_free(match);
            return found;
        }
        if (flags & WAIT_NOHANG) {
            return 0;
        }
        block_on_child(pid == -1 ? NULL : match);
    }
}

int task_wait(int pid) {
    int code;
    return task_wait_ex(pid, &code, 0) > 0 ? code : -1;
}

int task_getpid(void) {
    return current->pid;
}

task_t *task_current(void) {
    task_t *t = current;
    return t != this_cpu()->idle ? t : NULL;
}

task_t *task_running(void) {
    return current;
}

void task_exit(int code) {
    task_t *me = current;
    me->forbid = 0;  // exiting ends a forbid(), as Permit() would
    fpsimd_release(me);

    // Children outlive us as orphans: reaped as soon as they are zombies.
    // (Under the BKL: no other task changes parent links meanwhile.)
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state != TASK_UNUSED && t->parent == me) {
            t->parent = NULL;
            t->orphan = true;
            if (t->state == TASK_EXITED) {
                task_free(t);
            }
        }
    }

    uint64_t flags = spin_lock_irqsave(&sched_lock);
    me->state = TASK_EXITED;
    me->exit_code = code;
    task_t *parent = parent_of(me);
    if (!me->orphan && parent->state == TASK_BLOCKED && parent->blocked_on == BLOCK_CHILD &&
        (parent->waiting_for == me || parent->waiting_for == NULL)) {
        make_runnable(parent);
    }
    block_and_switch(flags);  // never picks this task again
    panic("exited task %d was switched back in", me->pid);
}

void task_reap_all(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && &tasks[i] != current) {
            task_free(&tasks[i]);
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

void task_for_each(void (*fn)(const task_t *t, void *ctx), void *ctx) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            fn(&tasks[i], ctx);
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}
