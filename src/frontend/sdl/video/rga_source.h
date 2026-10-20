// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Included by the Linux RGA presenter only. One staging image is enough:
// each V4L2 job is dequeued before the next image can overwrite its source.
#include "frontend/sdl/dmaheap.h"
#include "frontend/video/nearest_scale.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace ds::sdl {

class RgaSource {
public:
  ~RgaSource() { reset(); }
  RgaSource() = default;
  RgaSource(const RgaSource&) = delete;
  RgaSource& operator=(const RgaSource&) = delete;

  bool resize(u32 width, u32 height, u32 pitch_bytes, u32 bytes, std::string* why) {
    if (!width || !height || pitch_bytes % 4 || pitch_bytes < static_cast<size_t>(width) * 4 ||
        bytes < static_cast<size_t>(pitch_bytes) * height) {
      if (why) *why = "RGA returned an invalid source buffer layout";
      return false;
    }
    if (fd_ >= 0 && width == width_ && height == height_ && bytes == bytes_ && pitch_bytes == pitch_ * 4) return true;
    reset();
    fd_ = dmaheap::alloc(bytes, [](int) { return true; }, "rga source");
    if (fd_ >= 0) {
      void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
      if (p != MAP_FAILED) {
        px_ = static_cast<u32*>(p); bytes_ = bytes; pitch_ = pitch_bytes / 4; width_ = width; height_ = height;
        return true;
      }
    }
    if (why) *why = std::string("cannot allocate/map RGA source: ") + std::strerror(errno);
    reset();
    return false;
  }
  void write(const u32* frame) {
    dmaheap::sync_begin_write(fd_);
    frontend::scale_nearest(frame, px_, pitch_, width_, height_);
    dmaheap::sync_end_write(fd_);
  }
  int fd() const { return fd_; }

private:
  void reset() {
    if (px_) munmap(px_, bytes_);
    if (fd_ >= 0) close(fd_);
    px_ = nullptr; fd_ = -1; bytes_ = 0; pitch_ = width_ = height_ = 0;
  }
  int fd_ = -1;
  u32* px_ = nullptr;
  size_t bytes_ = 0;
  u32 pitch_ = 0, width_ = 0, height_ = 0;
};

} // namespace ds::sdl
