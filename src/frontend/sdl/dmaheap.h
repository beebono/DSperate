// SPDX-License-Identifier: GPL-3.0-or-later
// dmabuf allocation for the scanout tiers: tries DS_DMA_HEAP, else every
// /dev/dma_heap/* (the system heap first where the display controller can
// scan plain pages, i.e. sits behind an IOMMU; else contiguous memory
// first), else udmabuf (a memfd's pages wrapped
// as a dma-buf), else ION (new/legacy ABI), else the DRM driver's own GEM
// buffers (msm render node, then any card node's dumb buffer). The caller's
// `usable` runs the real import against each candidate; the first that
// passes is pinned for the rest of the process.
//
// DS_DMA_HEAP: a heap name (`cma-uncached`), an absolute path, `udmabuf`,
// `ion` (skip dma-heap), `ion:<mask>` (that heap_id_mask, no probing) or
// `drm` (only the DRM GEM sources).
#pragma once

#include <cstddef>
#include <functional>

namespace ds::sdl::dmaheap {

// The dmabuf fd (owned by the caller), or -1. `tag` prefixes the log lines.
int alloc(size_t len, const std::function<bool(int fd)>& usable, const char* tag);

// The pinned source, for the log ("/dev/dma_heap/cma-uncached", "ion:cma").
const char* chosen();

// Must bracket CPU writes to a dmabuf: the heaps map pages cached, and
// without this the display can read stale cache. Allocators without the
// ioctl (legacy ION, uncached anyway) fail once and are not asked again.
void sync_begin_write(int fd);
void sync_end_write(int fd);

} // namespace ds::sdl::dmaheap
