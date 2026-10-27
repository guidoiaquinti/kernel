// SPDX-License-Identifier: GPL-3.0-or-later
//
// KoraOS bridge for the few Circle memory/runtime hooks the USB stack needs.
// operator new/delete are already provided kernel-wide (src/cxx/cxx_runtime.cpp);
// here we cover Circle's heap-typed new, coherent DMA pages, the page
// allocator, the assertion handler, and the busy-loop delay.

#include <circle/memory.h>
#include <circle/new.h>
#include <circle/synchronize.h>

#include "mm.h"
#include "mm/coherent.h"
#include "mm/frame_alloc.h"
#include "mm/kmalloc.h"

// new (HEAP_DMA30 etc.): Circle asks for a buffer a device can reach by DMA,
// e.g. the bounce buffer the mass storage driver stages an unaligned transfer
// through. The kernel heap is low in RAM, within every DMA window, so the type
// only needs whole cache lines: the driver cleans and invalidates the buffer
// around the transfer, which must not touch a neighbour's data.
static void *dma_new(size_t nSize) {
    return kmalloc_aligned((nSize + DATA_CACHE_LINE_LENGTH_MAX - 1) & ~(size_t)(DATA_CACHE_LINE_LENGTH_MAX - 1),
                           DATA_CACHE_LINE_LENGTH_MAX);
}

void *operator new(size_t nSize, int nType) {
    (void)nType;
    return dma_new(nSize);
}

void *operator new[](size_t nSize, int nType) {
    (void)nType;
    return dma_new(nSize);
}

extern "C" void tfp_printf(const char *fmt, ...);

// Coherent pages requested by slot (e.g. the property-mailbox buffer the
// VideoCore reads). Backed by the MMU's Normal non-cacheable pool so no cache
// maintenance is needed, matching Circle's assumption for these buffers.
uintptr CMemorySystem::GetCoherentPage(unsigned nSlot) {
    return (uintptr)coherent_page(nSlot);
}

extern "C" {

// Circle's page allocator hooks: single 4 KB pages from the frame allocator.
void *palloc(void) {
    return frame_alloc();
}

void pfree(void *pPage) {
    frame_free(pPage);
}

// Circle's assert() calls this on failure.
void assertion_failed(const char *pExpr, const char *pFile, unsigned nLine) {
    tfp_printf("circle assert failed: %s at %s:%u\n", pExpr, pFile, nLine);
    for (;;) {
        asm volatile("wfi");
    }
}

// Busy-loop delay referenced by CTimer::nsDelay (unused on the fast path, but
// keep the symbol defined).
void DelayLoop(unsigned nCount) {
    for (volatile unsigned i = 0; i < nCount; i++) {
    }
}

}  // extern "C"
