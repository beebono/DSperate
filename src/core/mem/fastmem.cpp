// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/fastmem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

namespace ds::mem {

bool fastmem_requested() {
  static const bool on = [] { const char* e = std::getenv("DS_FASTMEM"); return e && std::atoi(e) != 0; }();
  return on;
}

#if defined(__linux__)

namespace {

constexpr long TMPFS_MAGIC_ = 0x01021994;

// memfd_create by number: glibc before 2.27 has no wrapper, and a kernel
// before 3.17 has no call (ENOSYS) -- the A30's 3.4 is one.
int try_memfd() {
#if defined(SYS_memfd_create)
  return static_cast<int>(syscall(SYS_memfd_create, "dsperate-guest", 1u /* MFD_CLOEXEC */));
#else
  return -1;
#endif
}

// An unlinked file on a tmpfs: the same pages as a memfd, just with a name for
// a moment. A disk-backed directory is refused -- MAP_SHARED on it would write
// guest RAM to storage -- and so is a tmpfs without room for the whole object:
// the file is sparse, so a small one (spruce mounts /dev, and with it
// /dev/shm, at 512 KB) accepts the ftruncate and then raises SIGBUS on the
// first guest page past its limit.
int try_tmpfs(size_t bytes, const char** where) {
  static const char* const dirs[] = {"/dev/shm", "/tmp/shm", "/tmp"};
  for (const char* d : dirs) {
    struct statfs st {};
    if (statfs(d, &st) != 0 || static_cast<long>(st.f_type) != TMPFS_MAGIC_) continue;
    if (static_cast<u64>(st.f_bavail) * static_cast<u64>(st.f_bsize) < static_cast<u64>(bytes) * 2) continue;   // room, with slack for others
    char path[64];
    std::snprintf(path, sizeof path, "%s/dsperate-guest-XXXXXX", d);
    const int fd = mkostemp(path, O_CLOEXEC);
    if (fd < 0) continue;
    unlink(path);
    *where = d;
    return fd;
  }
  return -1;
}

} // namespace

std::unique_ptr<HostArena> HostArena::create(size_t bytes) {
  const size_t size = (bytes + GuestView::HOST_PAGE - 1) & ~size_t{GuestView::HOST_PAGE - 1};
  std::unique_ptr<HostArena> a(new HostArena);
  const char* where = nullptr;
  a->fd_ = try_memfd();
  if (a->fd_ >= 0) a->kind_ = "memfd";
  else if ((a->fd_ = try_tmpfs(size, &where)) >= 0) a->kind_ = where;
  else return nullptr;
  if (ftruncate(a->fd_, static_cast<off_t>(size)) != 0) return nullptr;
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd_, 0);
  if (p == MAP_FAILED) return nullptr;
  a->base_ = static_cast<u8*>(p);
  a->size_ = size;
  return a;
}

HostArena::~HostArena() {
  if (base_) munmap(base_, size_);
  if (fd_ >= 0) close(fd_);
}

u8* HostArena::take(size_t bytes) {
  const size_t n = (bytes + GuestView::HOST_PAGE - 1) & ~size_t{GuestView::HOST_PAGE - 1};
  if (used_ + n > size_) return nullptr;
  u8* p = base_ + used_;
  used_ += n;
  return p;   // a fresh tmpfs page reads as zero
}

std::unique_ptr<GuestView> GuestView::create(const HostArena& arena, const PageTable& table) {
  std::unique_ptr<GuestView> v(new GuestView(arena, table));
  void* p = mmap(nullptr, static_cast<size_t>(RESERVE), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  v->base_ = static_cast<u8*>(p);
  const u32 pages = SPAN / HOST_PAGE;
  v->dirty_.assign(pages, 0);
  v->laid_.assign(pages, Laid{});
  v->pending_.reserve(pages);
  v->note_all();
  return v;
}

GuestView::~GuestView() {
  if (base_) munmap(base_, static_cast<size_t>(RESERVE));
}

void GuestView::note_all() {
  for (u32 v = 0; v < SPAN / HOST_PAGE; ++v) note(v << 1);
}

// What view page `v` should be, from the table: both 2 KB halves backed by the
// arena at consecutive offsets starting on a 4 KB boundary, outside VRAM.
GuestView::Laid GuestView::desired(u32 v) const {
  const u32 a0 = v * HOST_PAGE;
  if ((a0 >> 24) == 0x06) return {};   // VRAM: its traps toggle every frame; the table serves it
  u32 off[2];
  bool writable = true;
  for (int h = 0; h < 2; ++h) {
    const u32 a = a0 + static_cast<u32>(h) * PAGE_SIZE;
    const Entry e = table_.entry(a);
    const uintptr_t base = e << 2;
    if (!base) return {};
    const u8* host = reinterpret_cast<const u8*>(base + a);
    if (!arena_.contains(host) || !arena_.contains(host + PAGE_SIZE - 1)) return {};
    off[h] = static_cast<u32>(host - arena_.base());
    if (e & (TAG_CODE | TAG_SPECIAL)) writable = false;
  }
  if ((off[0] & (HOST_PAGE - 1)) != 0 || off[1] != off[0] + PAGE_SIZE) return {};
  return {off[0] + 1, static_cast<u8>(writable)};
}

bool GuestView::lay_run(u32 v0, u32 n, const Laid& first) {
  void* const addr = base_ + size_t{v0} * HOST_PAGE;
  const size_t len = size_t{n} * HOST_PAGE;
  void* r;
  if (!first.off_plus1)
    r = mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  else
    r = mmap(addr, len, PROT_READ | (first.writable ? PROT_WRITE : 0), MAP_SHARED | MAP_FIXED, arena_.fd(),
             static_cast<off_t>(first.off_plus1 - 1));
  ++stats_.map_calls;
  stats_.pages_laid += n;
  if (r == MAP_FAILED) {
    std::fprintf(stderr, "fastmem: mmap of view pages %05x+%u failed\n", v0, n);
    return false;
  }
  return true;
}

bool GuestView::flush() {
  if (pending_.empty()) return true;
  ++stats_.flushes;
  std::sort(pending_.begin(), pending_.end());
  bool ok = true;
  // Runs of consecutive pages that become one mapping: all unmapped, or the
  // same protection at consecutive offsets.
  size_t i = 0;
  while (i < pending_.size()) {
    const u32 v0 = pending_[i];
    dirty_[v0] = 0;
    const Laid d0 = desired(v0);
    if (d0 == laid_[v0]) { ++i; continue; }
    u32 n = 1;
    laid_[v0] = d0;
    while (i + n < pending_.size() && pending_[i + n] == v0 + n) {
      const u32 v = v0 + n;
      const Laid d = desired(v);
      const bool extends = d.writable == d0.writable && (d0.off_plus1 ? d.off_plus1 == d0.off_plus1 + n * HOST_PAGE : d.off_plus1 == 0);
      if (!extends || d == laid_[v]) break;
      dirty_[v] = 0;
      laid_[v] = d;
      ++n;
    }
    ok &= lay_run(v0, n, d0);
    i += n;
  }
  pending_.clear();
  return ok;
}

bool GuestView::verify(std::string* why) {
  if (!flush()) { *why = "flush failed"; return false; }
  char buf[160];
  for (u32 v = 0; v < SPAN / HOST_PAGE; ++v) {
    const Laid d = desired(v), l = laid_[v];
    if (!(d == l)) {
      std::snprintf(buf, sizeof buf, "view page %08x laid off %x w%d, table wants off %x w%d", v * HOST_PAGE, l.off_plus1, l.writable, d.off_plus1, d.writable);
      *why = buf;
      return false;
    }
    if (!l.off_plus1) continue;
    // Independent of desired(): each half must be what the table itself
    // hands the interpreter, and the bytes through the view must be those.
    for (int h = 0; h < 2; ++h) {
      const u32 a = v * HOST_PAGE + static_cast<u32>(h) * PAGE_SIZE;
      const u8* t = table_.read_ptr(a);
      bool code = false;
      const PageTable& tt = table_;
      const u8* w = tt.write_ptr(a, &code);
      if (t != arena_.base() + (l.off_plus1 - 1) + static_cast<u32>(h) * PAGE_SIZE || (l.writable && (!w || code))) {
        std::snprintf(buf, sizeof buf, "guest %08x: view serves %s, table read %p write %p code %d", a, l.writable ? "rw" : "r", static_cast<void*>(const_cast<u8*>(t)),
                      static_cast<void*>(const_cast<u8*>(w)), code ? 1 : 0);
        *why = buf;
        return false;
      }
      if (std::memcmp(base_ + a, t, PAGE_SIZE) != 0) {
        std::snprintf(buf, sizeof buf, "guest %08x: bytes through the view differ from the table's", a);
        *why = buf;
        return false;
      }
    }
  }
  return true;
}

#else   // not Linux: no arena, no views

std::unique_ptr<HostArena> HostArena::create(size_t) { return nullptr; }
HostArena::~HostArena() = default;
u8* HostArena::take(size_t) { return nullptr; }
std::unique_ptr<GuestView> GuestView::create(const HostArena&, const PageTable&) { return nullptr; }
GuestView::~GuestView() = default;
void GuestView::note_all() {}
GuestView::Laid GuestView::desired(u32) const { return {}; }
bool GuestView::lay_run(u32, u32, const Laid&) { return false; }
bool GuestView::flush() { return true; }
bool GuestView::verify(std::string*) { return true; }

#endif

} // namespace ds::mem
