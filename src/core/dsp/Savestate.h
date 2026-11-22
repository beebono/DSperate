// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What the vendored teakra (melonDS's copy, src/core/dsp/teakra) expects of
// melonDS's Savestate, so its sources stay as they are: a flat run of
// little-endian values, written to or read from a byte vector that DsiDsp
// then keeps in a DSperate save state as one block.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace melonDS {

class Savestate {
public:
  explicit Savestate(std::vector<std::uint8_t>& out) : Saving(true), buf_(&out) {}
  Savestate(const std::uint8_t* data, std::size_t size) : Saving(false), in_(data), size_(size) {}
  bool Saving;
  bool Error = false;   // a read ran past the block: what was read after is zero

  void Section(const char*) {}
  void Var8(std::uint8_t* v) { var(v, 1); }
  void Var16(std::uint16_t* v) { var(v, 2); }
  void Var32(std::uint32_t* v) { var(v, 4); }
  void Var64(std::uint64_t* v) { var(v, 8); }
  void VarBool(bool* v) { std::uint8_t b = *v ? 1 : 0; var(&b, 1); if (!Saving) *v = b != 0; }
  void Bool32(bool* v) { std::uint32_t b = *v ? 1 : 0; var(&b, 4); if (!Saving) *v = b != 0; }
  void VarArray(void* data, std::uint32_t len) { var(data, len); }

private:
  void var(void* p, std::size_t n) {
    if (Saving) { const auto* b = static_cast<const std::uint8_t*>(p); buf_->insert(buf_->end(), b, b + n); return; }
    if (size_ - pos_ < n) { Error = true; std::memset(p, 0, n); pos_ = size_; return; }
    std::memcpy(p, in_ + pos_, n); pos_ += n;
  }
  std::vector<std::uint8_t>* buf_ = nullptr;
  const std::uint8_t* in_ = nullptr;
  std::size_t size_ = 0, pos_ = 0;
};

} // namespace melonDS
