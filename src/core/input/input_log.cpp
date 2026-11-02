// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/input/input_log.h"
#include "core/nds.h"

#include <cstdlib>
#include <cstring>

namespace ds::input {

namespace {
constexpr char MAGIC[4] = {'D', 'S', 'I', 'N'};
constexpr u32 VERSION = 3;
constexpr size_t HEADER = 16, RECORD_V1 = 8, RECORD_V2 = 16, RECORD = 24;

// Where the ARM7 relay of a shocoman CPAD patch leaves the stick state for
// its ARM9 hook (RTCOM_DATA_OUTPUT). On an emulator there is no ARM7/ARM11
// relay: the host writes the same layout directly, once per frame.
//   +0: u16  CPadX | CPadY<<8          (signed bytes)
//   +4: u32  ZLZR | NubX<<8 | NubY<<16 (ZL = bit 2, ZR = bit 1 of the low
//       byte; nub axes unrotated -- see apply_analog)
constexpr u32 ANALOG_OUT = 0x027FFDF0;

s8 analog_byte(s16 v) { return static_cast<s8>(v / 256); }

void apply_analog(NDS& nds, const Frame& f) {
  const s8 cx = analog_byte(f.cpad_x), cy = analog_byte(f.cpad_y);
  nds.bus.dma_write16(Cpu::ARM9, ANALOG_OUT,
                      static_cast<u16>(static_cast<u8>(cx) | (static_cast<u16>(static_cast<u8>(cy)) << 8)));
  if (!nds.analog_full) {   // plain CPAD variant: the second word stays zero
    nds.bus.dma_write32(Cpu::ARM9, ANALOG_OUT + 4, 0);
    if (nds.analog_layout == NDS::AnalogLayout::Extended) {
      nds.bus.dma_write32(Cpu::ARM9, ANALOG_OUT + 8, 0);
      nds.bus.dma_write32(Cpu::ARM9, ANALOG_OUT + 12, 0);
    }
    return;
  }
  // No 45-degree rotation here, although the ARM7 relay applies one:
  // that rotation compensates the New 3DS C-Stick sensor being mounted
  // diagonally in the shell. Host sticks are axis-aligned, so rotating
  // would turn a pure right push into a diagonal (camera yaw + pitch
  // together) -- which is exactly the fault this replaces.
  const s8 rx = analog_byte(f.cstick_x), ry = analog_byte(f.cstick_y);
  const u32 low = (f.zl ? 4u : 0u) | (f.zr ? 2u : 0u);
  if (nds.analog_layout == NDS::AnalogLayout::Extended) {
    // Metroid's relay: ZL/ZR byte at +4, nub as s16 at +6/+8, and the
    // gyro words at +10..+14 (zero here; there is no gyro source).
    nds.bus.dma_write32(Cpu::ARM9, ANALOG_OUT + 4, low);
    nds.bus.dma_write16(Cpu::ARM9, ANALOG_OUT + 6, static_cast<u16>(static_cast<s16>(rx)));
    nds.bus.dma_write16(Cpu::ARM9, ANALOG_OUT + 8, static_cast<u16>(static_cast<s16>(ry)));
    nds.bus.dma_write16(Cpu::ARM9, ANALOG_OUT + 10, 0);   // gyro words: halfwords,
    nds.bus.dma_write16(Cpu::ARM9, ANALOG_OUT + 12, 0);   // a word write at +10
    nds.bus.dma_write16(Cpu::ARM9, ANALOG_OUT + 14, 0);   // would align down onto +8
    return;
  }
  nds.bus.dma_write32(Cpu::ARM9, ANALOG_OUT + 4,
                      low | (static_cast<u32>(static_cast<u8>(rx)) << 8) | (static_cast<u32>(static_cast<u8>(ry)) << 16));
}
} // namespace

void clear_analog(NDS& nds) {
  for (u32 a = ANALOG_OUT; a < ANALOG_OUT + 16; a += 4) nds.bus.dma_write32(Cpu::ARM9, a, 0);
}

void apply(NDS& nds, const Frame& f, const s16* mic, size_t mic_count) {
  nds.io.set_buttons(f.buttons);
  nds.io.set_touch(f.x, f.y, f.down);
  nds.io.set_lid(f.lid);
  if (nds.slot2 && nds.slot2->kind() == slot2::Kind::GuitarGrip) static_cast<slot2::GuitarGrip*>(nds.slot2.get())->set_frets(f.frets);
  if (nds.analog_patch) apply_analog(nds, f);
  if (mic) { nds.io.set_mic(mic, mic_count); return; }
  // Widen the logged samples into the core's own buffer once per frame.
  static thread_local s16 widened[Frame::MIC_SAMPLES];
  for (int i = 0; i < Frame::MIC_SAMPLES; ++i) widened[i] = static_cast<s16>(f.mic[i] * 256);
  nds.io.set_mic(widened, f.silent() ? 0 : Frame::MIC_SAMPLES);
}

void decimate_mic(Frame& f, const s16* mic, size_t n) {
  for (int i = 0; i < Frame::MIC_SAMPLES; ++i) {
    const size_t a = n * i / Frame::MIC_SAMPLES, b = n * (i + 1) / Frame::MIC_SAMPLES;
    int peak = 0;
    for (size_t k = a; k < b; ++k) if (std::abs(int(mic[k])) > std::abs(peak)) peak = mic[k];
    f.mic[i] = static_cast<s8>(peak >> 8);
  }
}

bool Log::open_write(const std::string& path) {
  close();
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) return false;
  u8 h[HEADER] = {};
  std::memcpy(h, MAGIC, 4);
  std::memcpy(h + 4, &VERSION, 4);
  std::fwrite(h, 1, HEADER, f_);
  write_ = true; count_ = 0; start_ = 0;
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
  if (ver != 1 && ver != 2 && ver != VERSION) { close(); return false; }
  record_ = ver == 1 ? RECORD_V1 : ver == 2 ? RECORD_V2 : RECORD;
  std::memcpy(&count_, h + 8, 4);
  std::memcpy(&start_, h + 12, 4);
  if (count_ == 0) {                       // recording was cut short: count from the size
    const long here = std::ftell(f_);
    std::fseek(f_, 0, SEEK_END);
    count_ = static_cast<u32>((std::ftell(f_) - here) / record_);
    std::fseek(f_, here, SEEK_SET);
  }
  write_ = false;
  return true;
}

void Log::close() {
  if (!f_) return;
  if (write_) {                            // patch the frame count and start into the header
    std::fseek(f_, 8, SEEK_SET);
    std::fwrite(&count_, 4, 1, f_);
    std::fwrite(&start_, 4, 1, f_);
  }
  std::fclose(f_);
  f_ = nullptr; write_ = false;
}

bool Log::wind_to(u64 frame) {
  if (frame < start_) return false;
  Frame f;
  for (u64 k = start_; k < frame && read(f); ++k) {}
  return true;
}

void Log::write(const Frame& f) {
  u8 r[RECORD] = {};
  std::memcpy(r, &f.buttons, 2);
  r[2] = f.x; r[3] = f.y;
  r[4] = static_cast<u8>((f.down ? 1 : 0) | (f.lid ? 2 : 0) | (f.zl ? 4 : 0) | (f.zr ? 8 : 0));
  r[5] = f.frets;
  std::memcpy(r + 8, f.mic, Frame::MIC_SAMPLES);
  std::memcpy(r + 16, &f.cpad_x, 2);
  std::memcpy(r + 18, &f.cpad_y, 2);
  std::memcpy(r + 20, &f.cstick_x, 2);
  std::memcpy(r + 22, &f.cstick_y, 2);
  std::fwrite(r, 1, RECORD, f_);
  ++count_;
}

bool Log::read(Frame& f) {
  u8 r[RECORD] = {};
  if (!f_ || std::fread(r, 1, record_, f_) != record_) return false;
  f = Frame{};
  std::memcpy(&f.buttons, r, 2);
  f.x = r[2]; f.y = r[3]; f.down = r[4] & 1; f.lid = (r[4] & 2) != 0;
  f.zl = (r[4] & 4) != 0; f.zr = (r[4] & 8) != 0;   // v1/v2 records leave these clear
  f.frets = r[5] & 0xF;
  std::memcpy(f.mic, r + 8, Frame::MIC_SAMPLES);
  if (record_ >= RECORD) {
    std::memcpy(&f.cpad_x, r + 16, 2);
    std::memcpy(&f.cpad_y, r + 18, 2);
    std::memcpy(&f.cstick_x, r + 20, 2);
    std::memcpy(&f.cstick_y, r + 22, 2);
  }
  return true;
}

} // namespace ds::input
