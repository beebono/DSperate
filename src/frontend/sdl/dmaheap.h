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

} // namespace ds::sdl::dmaheap
