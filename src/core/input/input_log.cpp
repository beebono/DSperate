// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/input/input_log.h"
#include "core/nds.h"

#include <cstring>

namespace ds::input {

namespace {
constexpr char MAGIC[4] = {'D', 'S', 'I', 'N'};
constexpr u32 VERSION = 1;
constexpr size_t HEADER = 16, RECORD = 8;
}

void apply(NDS& nds, const Frame& f) {
  nds.io.set_buttons(f.buttons);
  nds.io.set_touch(f.x, f.y, f.down);
}

bool Log::open_write(const std::string& path) {
  close();
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) return false;
  u8 h[HEADER] = {};
  std::memcpy(h, MAGIC, 4);
  std::memcpy(h + 4, &VERSION, 4);
  std::fwrite(h, 1, HEADER, f_);
  write_ = true; count_ = 0;
  return true;
}

bool Log::open_read(const std::string& path) {
  close();
  f_ = std::fopen(path.c_str(), "rb");
  if (!f_) return false;
  u8 h[HEADER];
  u32 ver = 0;
  if (std::fread(h, 1, HEADER, f_) != HEADER || std::memcmp(h, MAGIC, 4) != 0) { close(); return false; }
  std::memcpy(&ver, h + 4, 4);
  if (ver != VERSION) { close(); return false; }
  std::memcpy(&count_, h + 8, 4);
  if (count_ == 0) {                       // recording was cut short: count from the size
    const long here = std::ftell(f_);
    std::fseek(f_, 0, SEEK_END);
    count_ = static_cast<u32>((std::ftell(f_) - here) / RECORD);
    std::fseek(f_, here, SEEK_SET);
  }
  write_ = false;
  return true;
}

void Log::close() {
  if (!f_) return;
  if (write_) {                            // patch the frame count into the header
    std::fseek(f_, 8, SEEK_SET);
    std::fwrite(&count_, 4, 1, f_);
  }
  std::fclose(f_);
  f_ = nullptr; write_ = false;
}

void Log::write(const Frame& f) {
  u8 r[RECORD] = {};
  std::memcpy(r, &f.buttons, 2);
  r[2] = f.x; r[3] = f.y; r[4] = f.down ? 1 : 0;
  std::fwrite(r, 1, RECORD, f_);
  ++count_;
}

bool Log::read(Frame& f) {
  u8 r[RECORD];
  if (!f_ || std::fread(r, 1, RECORD, f_) != RECORD) return false;
  std::memcpy(&f.buttons, r, 2);
  f.x = r[2]; f.y = r[3]; f.down = r[4] & 1;
  return true;
}

} // namespace ds::input
