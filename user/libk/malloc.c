// SPDX-License-Identifier: GPL-3.0-or-later
//
// libk malloc: a two-level segregated fit (TLSF) allocator over pools of pages
// from alloc_pages(). With one flat address space, memory next to a pool is
// usually someone else's, so the heap is a set of independent pools rather
// than one region grown with a break. TLSF handles that naturally and finds a
// fitting free block in constant time.
//
// Pool layout (a page run):
//
//   [pool header 16][block][block]...[sentinel header 16]
//
// Every block starts with a 16-byte header: the size of the previous block in
// the pool (0 for the first) and its own size including the header, with flag
// bits in the low bits. Payloads are 16-byte aligned. A free block also keeps
// its free-list links in its payload. The sentinel is a used, zero-size block
// that stops coalescing at the end of the pool.
//
// Requests above DIRECT_THRESHOLD bypass the pools: they get their own page
// run, with a header marking them as direct.
//
// Thread safe: one mutex covers the whole heap. Calls are short (constant
// time apart from asking the kernel for pages), so finer locking would not pay.

#include "koraos.h"

#define ALIGN 16
#define HDR 16
#define MIN_BLOCK 32             // header plus the two free-list links
#define SL_LOG2 4                // 16 second-level lists per first level
#define SL_COUNT (1u << SL_LOG2)
#define FL_SHIFT (SL_LOG2 + 4)   // sizes below 256 bytes share first level 0
#define SMALL_BLOCK (1ul << FL_SHIFT)
#define FL_COUNT 40
#define POOL_PAGES 16            // pools grow 64 KB at a time
#define DIRECT_THRESHOLD (256ul * 1024)

#define F_FREE 1ul
#define F_DIRECT 2ul
#define F_MASK 15ul

struct block {
    size_t prev_size;
    size_t size;  // bytes including this header, plus F_* flags
    // Free blocks only:
    struct block *next_free;
    struct block *prev_free;
};

struct pool {
    struct pool *next;
    struct pool *prev;
};

static struct block *lists[FL_COUNT][SL_COUNT];
static unsigned long fl_bitmap;
static unsigned sl_bitmap[FL_COUNT];
static struct pool *pools;
static struct block *cached_empty;  // a whole free pool kept for reuse
static size_t direct_count;
static size_t direct_bytes;
static size_t bad_frees;
static mutex_t heap_lock = MUTEX_INIT;

static size_t bsize(const struct block *b) {
    return b->size & ~F_MASK;
}

static struct block *next_phys(struct block *b) {
    return (struct block *)((char *)b + bsize(b));
}

static struct block *prev_phys(struct block *b) {
    return (struct block *)((char *)b - b->prev_size);
}

static void *payload(struct block *b) {
    return (char *)b + HDR;
}

static struct block *from_payload(void *p) {
    return (struct block *)((char *)p - HDR);
}

static int fls_long(unsigned long x) {
    return 63 - __builtin_clzl(x);
}

static void mapping(size_t size, unsigned *fl, unsigned *sl) {
    if (size < SMALL_BLOCK) {
        *fl = 0;
        *sl = (unsigned)(size / ALIGN);
    } else {
        int f = fls_long(size);
        *sl = (unsigned)(size >> (f - SL_LOG2)) ^ SL_COUNT;
        *fl = (unsigned)(f - FL_SHIFT + 1);
    }
}

static void insert_free(struct block *b) {
    unsigned fl, sl;
    mapping(bsize(b), &fl, &sl);
    b->prev_free = NULL;
    b->next_free = lists[fl][sl];
    if (b->next_free != NULL) {
        b->next_free->prev_free = b;
    }
    lists[fl][sl] = b;
    fl_bitmap |= 1ul << fl;
    sl_bitmap[fl] |= 1u << sl;
    b->size |= F_FREE;
}

static void remove_free(struct block *b) {
    unsigned fl, sl;
    mapping(bsize(b), &fl, &sl);
    if (b->prev_free != NULL) {
        b->prev_free->next_free = b->next_free;
    } else {
        lists[fl][sl] = b->next_free;
        if (lists[fl][sl] == NULL) {
            sl_bitmap[fl] &= ~(1u << sl);
            if (sl_bitmap[fl] == 0) {
                fl_bitmap &= ~(1ul << fl);
            }
        }
    }
    if (b->next_free != NULL) {
        b->next_free->prev_free = b->prev_free;
    }
    b->size &= ~F_FREE;
    if (b == cached_empty) {
        cached_empty = NULL;
    }
}

// The first free block of at least `size` bytes: round the request up to the
// next list boundary so any block in the list found is large enough.
static struct block *find_free(size_t size) {
    if (size >= SMALL_BLOCK) {
        size += (1ul << (fls_long(size) - SL_LOG2)) - 1;
    }
    unsigned fl, sl;
    mapping(size, &fl, &sl);
    if (fl >= FL_COUNT) {
        return NULL;
    }
    unsigned sl_map = sl_bitmap[fl] & (~0u << sl);
    if (sl_map == 0) {
        unsigned long fl_map = fl + 1 < FL_COUNT ? fl_bitmap & (~0ul << (fl + 1)) : 0;
        if (fl_map == 0) {
            return NULL;
        }
        fl = (unsigned)__builtin_ctzl(fl_map);
        sl_map = sl_bitmap[fl];
    }
    sl = (unsigned)__builtin_ctz(sl_map);
    return lists[fl][sl];
}

static int is_last(struct block *b) {
    return bsize(next_phys(b)) == 0;
}

// Add a pool big enough for one block of `size` bytes; returns that block
// (free), or NULL when the kernel has no pages left.
static struct block *grow(size_t size) {
    size_t need = sizeof(struct pool) + size + HDR;
    size_t pages = (need + KORAOS_PAGE_SIZE - 1) / KORAOS_PAGE_SIZE;
    if (pages < POOL_PAGES) {
        pages = POOL_PAGES;
    }
    struct pool *pool = alloc_pages(pages);
    if (pool == NULL) {
        return NULL;
    }
    pool->prev = NULL;
    pool->next = pools;
    if (pools != NULL) {
        pools->prev = pool;
    }
    pools = pool;

    size_t bytes = pages * KORAOS_PAGE_SIZE;
    struct block *b = (struct block *)(pool + 1);
    b->prev_size = 0;
    b->size = bytes - sizeof(struct pool) - HDR;
    struct block *sentinel = next_phys(b);
    sentinel->prev_size = bsize(b);
    sentinel->size = 0;
    insert_free(b);
    return b;
}

static void release_pool(struct block *b) {
    struct pool *pool = (struct pool *)b - 1;
    remove_free(b);
    if (pool->prev != NULL) {
        pool->prev->next = pool->next;
    } else {
        pools = pool->next;
    }
    if (pool->next != NULL) {
        pool->next->prev = pool->prev;
    }
    free_pages(pool);
}

// Split `b` (in use) so it is `size` bytes, freeing the remainder.
static void split(struct block *b, size_t size) {
    size_t total = bsize(b);
    if (total - size < MIN_BLOCK) {
        return;
    }
    struct block *rest = (struct block *)((char *)b + size);
    rest->prev_size = size;
    rest->size = total - size;
    next_phys(rest)->prev_size = bsize(rest);
    b->size = size | (b->size & F_MASK);
    insert_free(rest);
}

static void *direct_alloc(size_t size) {
    size_t pages = (size + HDR + KORAOS_PAGE_SIZE - 1) / KORAOS_PAGE_SIZE;
    struct block *b = alloc_pages(pages);
    if (b == NULL) {
        return NULL;
    }
    b->prev_size = 0;
    b->size = (pages * KORAOS_PAGE_SIZE) | F_DIRECT;
    direct_count++;
    direct_bytes += bsize(b);
    return payload(b);
}

static void *malloc_locked(size_t n) {
    if (n > ~0ul / 2) {
        return NULL;
    }
    size_t size = (n + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    if (size < MIN_BLOCK) {
        size = MIN_BLOCK;
    }
    if (size > DIRECT_THRESHOLD) {
        return direct_alloc(n);
    }
    struct block *b = find_free(size);
    if (b == NULL) {
        b = grow(size);
        if (b == NULL) {
            return NULL;
        }
    }
    remove_free(b);
    split(b, size);
    return payload(b);
}

static void free_locked(void *p) {
    struct block *b = from_payload(p);
    if (((unsigned long)p & (ALIGN - 1)) != 0 || (b->size & F_FREE) != 0) {
        bad_frees++;  // misaligned pointer or double free: ignore it
        return;
    }
    if ((b->size & F_DIRECT) != 0) {
        size_t bytes = bsize(b);
        if (free_pages(b) != 0) {
            bad_frees++;  // not a live run: already freed, or never ours
            return;
        }
        direct_count--;
        direct_bytes -= bytes;
        return;
    }

    struct block *next = next_phys(b);
    if ((next->size & F_FREE) != 0) {
        remove_free(next);
        b->size += bsize(next);
        next_phys(b)->prev_size = bsize(b);
    }
    if (b->prev_size != 0) {
        struct block *prev = prev_phys(b);
        if ((prev->size & F_FREE) != 0) {
            remove_free(prev);
            prev->size += bsize(b);
            next_phys(prev)->prev_size = bsize(prev);
            b = prev;
        }
    }
    insert_free(b);

    // A pool that is entirely free goes back to the kernel, except one kept
    // for the next allocation burst.
    if (b->prev_size == 0 && is_last(b)) {
        if (cached_empty == NULL) {
            cached_empty = b;
        } else if (cached_empty != b) {
            release_pool(b);
        }
    }
}

void *malloc(size_t n) {
    mutex_lock(&heap_lock);
    void *p = malloc_locked(n);
    mutex_unlock(&heap_lock);
    return p;
}

void free(void *p) {
    if (p == NULL) {
        return;
    }
    mutex_lock(&heap_lock);
    free_locked(p);
    mutex_unlock(&heap_lock);
}

void *calloc(size_t count, size_t size) {
    if (size != 0 && count > ~0ul / size) {
        return NULL;
    }
    size_t n = count * size;
    void *p = malloc(n);
    if (p != NULL) {
        memset(p, 0, n);
    }
    return p;
}

void *realloc(void *p, size_t n) {
    if (p == NULL) {
        return malloc(n);
    }
    if (n == 0) {
        free(p);
        return NULL;
    }
    mutex_lock(&heap_lock);
    struct block *b = from_payload(p);
    size_t have = bsize(b) - HDR;
    void *q = p;
    if (n > have) {
        q = malloc_locked(n);
        if (q != NULL) {
            memcpy(q, p, have);
            free_locked(p);
        }
    }
    mutex_unlock(&heap_lock);
    return q;
}

void kheap_info(struct kheap_info *out) {
    memset(out, 0, sizeof(*out));
    mutex_lock(&heap_lock);
    for (struct pool *pool = pools; pool != NULL; pool = pool->next) {
        out->pools++;
        struct block *b = (struct block *)(pool + 1);
        size_t prev = 0;
        for (;;) {
            if (b->prev_size != prev || (bsize(b) != 0 && bsize(b) < MIN_BLOCK) ||
                (bsize(b) & (ALIGN - 1)) != 0) {
                out->corrupt++;
                break;
            }
            out->pool_bytes += bsize(b);
            if (bsize(b) == 0) {
                out->pool_bytes += sizeof(struct pool) + HDR;
                break;
            }
            if ((b->size & F_FREE) != 0) {
                out->free_bytes += bsize(b);
                if ((next_phys(b)->size & F_FREE) != 0) {
                    out->corrupt++;  // two free neighbours should have merged
                }
            } else {
                out->used_blocks++;
                out->used_bytes += bsize(b);
            }
            prev = bsize(b);
            b = next_phys(b);
        }
    }
    out->direct_allocs = direct_count;
    out->direct_bytes = direct_bytes;
    out->bad_frees = bad_frees;
    mutex_unlock(&heap_lock);
}
