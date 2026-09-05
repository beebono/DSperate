// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises src/frontend/sdl/dmaheap.cpp on a device: which dmabuf source it
// picks, and that the buffer maps and takes writes. Usage: dmaheap_probe [bytes]
// with DS_DMA_HEAP honoured as in the frontend; DS_DMA_HEAP_HOLD=<s> keeps the
// buffers alive that long so /sys/kernel/debug/ion can be read.
#include "../src/frontend/sdl/dmaheap.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char** argv) {
  const size_t len = argc > 1 ? std::strtoul(argv[1], nullptr, 0) : 640u * 480u * 4u;
  int fd = ds::sdl::dmaheap::alloc(len, [&](int f) {
    void* m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    if (m == MAP_FAILED) { std::perror("probe: mmap"); return false; }
    std::memset(m, 0xA5, len);
    const bool ok = static_cast<unsigned char*>(m)[len - 1] == 0xA5;
    munmap(m, len);
    return ok;
  }, "probe");
  if (fd < 0) return 1;
  std::printf("ok: %zu bytes from %s (fd %d)\n", len, ds::sdl::dmaheap::chosen(), fd);
  // A second allocation goes through the pinned source.
  int fd2 = ds::sdl::dmaheap::alloc(len, [](int) { return true; }, "probe");
  std::printf("second: %s\n", fd2 >= 0 ? "ok" : "FAILED");
  if (const char* h = std::getenv("DS_DMA_HEAP_HOLD")) sleep(std::atoi(h));   // for a look at debugfs
  close(fd); if (fd2 >= 0) close(fd2);
  return 0;
}
