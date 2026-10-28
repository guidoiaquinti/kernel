#include "mm/frame_alloc.h"
#include "arch/spinlock.h"
#include "mm.h"
#ifdef KORAOS_VIRT
#include "platform/virt.h"
#endif

// Defined by the linker script: first byte after the kernel image.
extern char _end[];

// Size of the reserved physical pool that starts just past the kernel image.
// Everything the frame allocator hands out lives inside this window, so we
// never touch RAM we have not confirmed exists.
#define POOL_BYTES (16 * 1024 * 1024)
#define MAX_FRAMES (POOL_BYTES / PAGE_SIZE)
#define BITS_PER_WORD 64
#define BITMAP_WORDS ((MAX_FRAMES + BITS_PER_WORD - 1) / BITS_PER_WORD)

// One bit per page: 1 = allocated, 0 = free.
// frame_lock (IRQ-safe) covers the bitmap and the free count.
static struct spinlock frame_lock = SPINLOCK_INIT("frames");

static uint64_t bitmap[BITMAP_WORDS];
static uintptr_t pool_base;
static size_t pool_frames;
static size_t free_frames;

static uintptr_t align_up(uintptr_t v, uintptr_t a) {
    return (v + a - 1) & ~(a - 1);
}

static bool frame_is_used(size_t i) {
    return (bitmap[i / BITS_PER_WORD] >> (i % BITS_PER_WORD)) & 1u;
}

static void frame_set_used(size_t i) {
    bitmap[i / BITS_PER_WORD] |= (uint64_t)1 << (i % BITS_PER_WORD);
}

static void frame_set_free(size_t i) {
    bitmap[i / BITS_PER_WORD] &= ~((uint64_t)1 << (i % BITS_PER_WORD));
}

static void zero_pages(void *page, size_t count) {
    volatile uint64_t *p = (volatile uint64_t *)page;
    size_t words = (count * PAGE_SIZE) / sizeof(uint64_t);
    for (size_t i = 0; i < words; i++) {
        p[i] = 0;
    }
}

void frame_alloc_init(void) {
    pool_base = align_up((uintptr_t)_end, PAGE_SIZE);

    // The pool ends a fixed distance past the (unaligned) kernel end; the
    // alignment above may cost us up to one frame, so derive the count from the
    // aligned base rather than assuming the maximum.
    uintptr_t pool_end = (uintptr_t)_end + POOL_BYTES;
#ifdef KORAOS_VIRT
    const struct virt_platform *machine = virt_platform_get();
    uintptr_t ram_end = machine->ram_base + machine->ram_size;
    if (pool_base < machine->ram_base || pool_base >= ram_end) {
        pool_frames = 0;
        return;
    }
    if (pool_end > ram_end) {
        pool_end = ram_end;
    }
#endif
    pool_frames = (pool_end - pool_base) / PAGE_SIZE;
    if (pool_frames > MAX_FRAMES) {
        pool_frames = MAX_FRAMES;
    }

    for (size_t i = 0; i < BITMAP_WORDS; i++) {
        bitmap[i] = 0;
    }
#ifdef KORAOS_VIRT
    // Keep the boot device tree intact if it occupies part of the frame pool.
    uintptr_t dtb_end = machine->dtb_base + machine->dtb_size;
    for (size_t i = 0; i < pool_frames; i++) {
        uintptr_t page = pool_base + i * PAGE_SIZE;
        if (page < dtb_end && page + PAGE_SIZE > machine->dtb_base) {
            frame_set_used(i);
        }
        for (unsigned r = 0; r < machine->reserved_count; r++) {
            uintptr_t base = machine->reserved[r].base;
            uintptr_t end = base + machine->reserved[r].size;
            if (page < end && page + PAGE_SIZE > base) {
                frame_set_used(i);
            }
        }
    }
#endif
    free_frames = 0;
    for (size_t i = 0; i < pool_frames; i++) {
        if (!frame_is_used(i)) {
            free_frames++;
        }
    }
}

void *frame_alloc_pages(size_t count) {
    if (count == 0 || count > pool_frames) {
        return NULL;
    }

    // The kernel heap allocates from interrupt handlers too (the USB stack's
    // completion routines), hence the IRQ-safe lock. Zeroing happens after
    // dropping it: the run is already ours.
    uint64_t flags = spin_lock_irqsave(&frame_lock);
    void *page = NULL;
    // Linear first-fit scan for a run of `count` consecutive free frames.
    for (size_t start = 0; start + count <= pool_frames; start++) {
        size_t run = 0;
        while (run < count && !frame_is_used(start + run)) {
            run++;
        }
        if (run == count) {
            for (size_t i = 0; i < count; i++) {
                frame_set_used(start + i);
            }
            free_frames -= count;
            page = (void *)(pool_base + start * PAGE_SIZE);
            break;
        }
        // Skip past the used frame that broke the run.
        start += run;
    }
    spin_unlock_irqrestore(&frame_lock, flags);

    if (page != NULL) {
        zero_pages(page, count);
    }
    return page;
}

void *frame_alloc(void) {
    return frame_alloc_pages(1);
}

void frame_free_pages(void *pages, size_t count) {
    if (pages == NULL || count == 0) {
        return;
    }

    uintptr_t addr = (uintptr_t)pages;
    if (addr < pool_base || (addr - pool_base) % PAGE_SIZE != 0) {
        return;  // Not a page from this pool.
    }

    size_t start = (addr - pool_base) / PAGE_SIZE;
    if (start >= pool_frames || start + count > pool_frames) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&frame_lock);
    for (size_t i = 0; i < count; i++) {
        if (frame_is_used(start + i)) {
            frame_set_free(start + i);
            free_frames++;
        }
    }
    spin_unlock_irqrestore(&frame_lock, flags);
}

void frame_free(void *page) {
    frame_free_pages(page, 1);
}

size_t frame_alloc_free_count(void) {
    return free_frames;
}

size_t frame_alloc_total_count(void) {
    return pool_frames;
}

uintptr_t frame_alloc_pool_end(void) {
    return pool_base + pool_frames * PAGE_SIZE;
}
