// SPDX-License-Identifier: GPL-3.0-or-later
// dmabuf allocation for the scanout tiers, from whichever allocator the
// kernel offers.
//
// Mainline names its CMA heap /dev/dma_heap/linux,cma; vendor BSP kernels
// name theirs differently (Rockchip 5.10: cma-uncached, reserved,
// system-uncached-dma32 ...) and older BSPs have no dma-heap at all, only
// /dev/ion. So: try the heap named by DS_DMA_HEAP, else every heap in
// /dev/dma_heap/ contiguous-first, else ION (new or legacy ABI, every heap it
// reports or, on legacy, every heap id bit). The caller's `usable` runs the
// real import (ADDFB2, zwp_linux_buffer_params create) on each candidate and
// says whether the display can take it; the first source that passes is
// pinned for the rest of the process so a tier's buffers all come from one
// place.
//
// DS_DMA_HEAP: a heap name (`cma-uncached`), an absolute path, `ion`
// (skip dma-heap) or `ion:<mask>` (that heap_id_mask, no probing).
#pragma once

#include <cstddef>
#include <functional>

namespace ds::sdl::dmaheap {

// The dmabuf fd (owned by the caller), or -1. `tag` prefixes the log lines.
int alloc(size_t len, const std::function<bool(int fd)>& usable, const char* tag);

// The pinned source, for the log ("/dev/dma_heap/cma-uncached", "ion:cma").
const char* chosen();

// CPU access to a dmabuf has to be bracketed, or the device may not see what
// was written: the mainline CMA heap maps its pages cached and does its cache
// maintenance in this ioctl, so without it the last writes before a commit can
// still be sitting in the CPU's caches when the display controller reads the
// buffer. That shows up as the most recently drawn pixels flickering -- the
// overlays, which are the last thing written before the frame is handed over.
//
// An allocator that does not implement the ioctl (legacy ION) fails it once
// and is not asked again; those heaps are uncached anyway.
void sync_begin_write(int fd);
void sync_end_write(int fd);

} // namespace ds::sdl::dmaheap
