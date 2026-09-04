// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace ds::state {

// Save states. A file is "DSST", a u32 format version, then chunks: a
// four-character tag, a u32 payload length and the payload. Every subsystem
// serialises itself through one `template <class S> void sync_state(S& s)`
// that lists its fields with `s.fields(a, b, c)`; the same function writes
// (S = Writer) and reads (S = Reader), so the two cannot drift apart. Only
// scalars and arrays of scalars are written -- never whole structs, whose
// padding would make two saves of the same state differ byte for byte --
// and never host pointers: everything derived from the saved fields is
// rebuilt in the subsystem's after_load().
//
// A chunk may grow: a reader that reaches the end of a chunk early stops
// taking fields (`more()` is false), and a writer that appends fields keeps
// old files loadable as long as the new fields default sensibly.
constexpr u32 FORMAT_VERSION = 1;

class Writer {
public:
  void begin(const char tag[4]) { blob(tag, 4); len_at_ = buf_.size(); u32 z = 0; blob(&z, 4); }
  void end() { const u32 n = static_cast<u32>(buf_.size() - len_at_ - 4); std::memcpy(&buf_[len_at_], &n, 4); }
  bool more() const { return true; }

  template <class T> void put(const T& v) {
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) blob(&v, sizeof v);
    else if constexpr (std::is_array_v<T>) for (const auto& e : v) put(e);
    else if constexpr (is_std_array<T>::value) for (const auto& e : v) put(e);
    else static_assert(sizeof(T) == 0, "sync_state: list the struct's fields, not the struct");
  }
  template <class... T> void fields(const T&... v) { (put(v), ...); }
  void blob(const void* p, size_t n) { const u8* b = static_cast<const u8*>(p); buf_.insert(buf_.end(), b, b + n); }
  // A vector's live contents; the reader resizes to match.
  template <class T> void vec(const std::vector<T>& v) { put(static_cast<u32>(v.size())); for (const auto& e : v) put(e); }

  std::vector<u8>& data() { return buf_; }
  static constexpr bool reading = false;

private:
  template <class T> struct is_std_array : std::false_type {};
  template <class T, size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
  std::vector<u8> buf_;
  size_t len_at_ = 0;
};

class Reader {
public:
  Reader(const u8* p, size_t n) : p_(p), end_(p + n) {}

  // Positions on the next chunk, which must carry `tag`; false (and the
  // reader is failed) otherwise. end() skips whatever the chunk still holds.
  bool begin(const char tag[4]) {
    chunk_end_ = end_;
    if (!ok_ || end_ - p_ < 8 || std::memcmp(p_, tag, 4) != 0) { fail(std::string("expected chunk ") + std::string(tag, 4)); return false; }
    u32 n; std::memcpy(&n, p_ + 4, 4);
    p_ += 8;
    if (static_cast<size_t>(end_ - p_) < n) { fail("truncated chunk"); return false; }
    chunk_end_ = p_ + n;
    return true;
  }
  void end() { if (ok_) p_ = chunk_end_; }
  bool more() const { return ok_ && p_ < chunk_end_; }

  template <class T> void put(T& v) {   // named like the writer's so sync_state reads the same
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) { if (more()) blob(&v, sizeof v); }
    else if constexpr (std::is_array_v<T>) for (auto& e : v) put(e);
    else if constexpr (is_std_array<T>::value) for (auto& e : v) put(e);
    else static_assert(sizeof(T) == 0, "sync_state: list the struct's fields, not the struct");
  }
  template <class... T> void fields(T&... v) { (put(v), ...); }
  void blob(void* p, size_t n) {
    if (!ok_) return;
    if (static_cast<size_t>(chunk_end_ - p_) < n) { fail("short read"); std::memset(p, 0, n); return; }
    std::memcpy(p, p_, n); p_ += n;
  }
  template <class T> void vec(std::vector<T>& v) { u32 n = 0; put(n); v.resize(n); for (auto& e : v) put(e); }

  // Bytes outside any chunk (the file magic and version).
  void blob_raw(void* p, size_t n) { if (!ok_) return; if (static_cast<size_t>(end_ - p_) < n) { fail("short file"); std::memset(p, 0, n); return; } std::memcpy(p, p_, n); p_ += n; }
  // Whether anything follows the chunks read so far: the frontend appends a
  // chunk of its own after the machine's, and an older file simply ends.
  bool at_end() const { return !ok_ || p_ >= end_; }
  bool ok() const { return ok_; }
  const std::string& error() const { return err_; }
  void fail(const std::string& why) { if (ok_) { ok_ = false; err_ = why; } }
  static constexpr bool reading = true;

private:
  template <class T> struct is_std_array : std::false_type {};
  template <class T, size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
  const u8* p_; const u8* end_; const u8* chunk_end_ = nullptr;
  bool ok_ = true;
  std::string err_;
};

} // namespace ds::state
