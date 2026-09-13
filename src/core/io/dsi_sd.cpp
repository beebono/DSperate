#include "core/io/dsi_sd.h"
#include "core/io/dsi_nwifi.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>

#include "core/nds.h"
#include "core/io/io.h"
#include "core/sched/scheduler.h"
#include "core/dma/ndma.h"
#include "core/state/state.h"

namespace ds::io {

namespace {
// melonDS's Transfer_TX / Transfer_RX event function ids, folded into the
// one event's parameter.
constexpr u32 TRANSFER_TX = 0, TRANSFER_RX = 1;
// melonDS schedules the transfer 512 system (ARM7) cycles out; our scheduler
// counts ARM9 cycles, so the delay doubles (the same conversion the DSi RTC
// grid event makes).
constexpr u64 TRANSFER_DELAY = 512 * 2;
}  // namespace

// ---- NandImage ---------------------------------------------------------------

NandImage::~NandImage() { close(); }

void NandImage::close() {
  if (file_) { std::fflush(file_); std::fclose(file_); file_ = nullptr; }
  in_memory_ = false;
  baseline_ = false; changed_.clear();
  length_ = 0; console_id_ = 0; std::memset(cid_, 0, sizeof(cid_));
  written_.clear();
}

void NandImage::create_in_memory(u64 length, const u8 cid[16], u64 console_id) {
  close();
  in_memory_ = true;
  length_ = length;
  std::memcpy(cid_, cid, sizeof(cid_));
  console_id_ = console_id;
}

bool NandImage::open(const std::string& path, bool write_through) {
  close();
  std::FILE* f = std::fopen(path.c_str(), write_through ? "r+b" : "rb");
  if (!f) { std::fprintf(stderr, "[nand] cannot open %s\n", path.c_str()); return false; }
  std::fseek(f, 0, SEEK_END);
  const long len = std::ftell(f);
  if (len <= 0x40) { std::fprintf(stderr, "[nand] %s is too small\n", path.c_str()); std::fclose(f); return false; }

  // The nocash footer: normally the last 0x40 bytes, with a second copy at
  // 0x000FF800 for images cut off by external tools (GBATEK, DSi SD/MMC
  // images). melonDS NANDImage::NANDImage.
  static const char kRef[16] = {'D','S','i',' ','e','M','M','C',' ','C','I','D','/','C','P','U'};
  char footer[16];
  std::fseek(f, -0x40, SEEK_END);
  if (std::fread(footer, 1, sizeof(footer), f) != sizeof(footer) || std::memcmp(footer, kRef, sizeof(kRef)) != 0) {
    std::fseek(f, 0x000FF800, SEEK_SET);
    if (std::fread(footer, 1, sizeof(footer), f) != sizeof(footer) || std::memcmp(footer, kRef, sizeof(kRef)) != 0) {
      std::fprintf(stderr, "[nand] %s has no nocash footer (no eMMC CID / console ID)\n", path.c_str());
      std::fclose(f);
      return false;
    }
  }
  if (std::fread(cid_, 1, sizeof(cid_), f) != sizeof(cid_) ||
      std::fread(&console_id_, 1, sizeof(console_id_), f) != sizeof(console_id_)) {
    std::fprintf(stderr, "[nand] %s: truncated footer\n", path.c_str());
    std::fclose(f);
    return false;
  }

  file_ = f;
  write_through_ = write_through;
  length_ = static_cast<u64>(len);
  return true;
}

void NandImage::read_file(u64 addr, u32 len, u8* out) {
  if (!file_) { std::memset(out, 0, len); return; }
  std::fseek(file_, static_cast<long>(addr), SEEK_SET);
  const size_t got = std::fread(out, 1, len, file_);
  if (got < len) std::memset(out + got, 0, len - got);
}

void NandImage::read(u64 addr, u32 len, u8* out) {
  reads++;
  log_access(false, addr, len);
  if (addr < watch_to_ && addr + len > watch_from_) watch_hit_ = true;
  peek(addr, len, out);
}

void NandImage::write(u64 addr, u32 len, const u8* in) {
  writes++;
  log_access(true, addr, len);
  poke(addr, len, in);
}

void NandImage::peek(u64 addr, u32 len, u8* out) {
  if (!valid()) { std::memset(out, 0, len); return; }
  read_file(addr, len, out);
  if (written_.empty()) return;
  // Overlay the sectors written this session. Reads are whole blocks from the
  // SD host and 16-byte pieces from the boot2 loader; both go through here.
  for (u64 s = addr / MMC_BLOCK_SIZE, end = (addr + len + MMC_BLOCK_SIZE - 1) / MMC_BLOCK_SIZE; s < end; ++s) {
    const auto it = written_.find(s);
    if (it == written_.end()) continue;
    const u64 sec = s * MMC_BLOCK_SIZE;
    const u64 from = std::max(sec, addr), to = std::min(sec + MMC_BLOCK_SIZE, addr + len);
    std::memcpy(out + (from - addr), it->second.data() + (from - sec), static_cast<size_t>(to - from));
  }
}

void NandImage::poke(u64 addr, u32 len, const u8* in) {
  if (!valid()) return;
  if (write_through_ && file_) {
    std::fseek(file_, static_cast<long>(addr), SEEK_SET);
    std::fwrite(in, 1, len, file_);
    return;
  }
  for (u64 s = addr / MMC_BLOCK_SIZE, end = (addr + len + MMC_BLOCK_SIZE - 1) / MMC_BLOCK_SIZE; s < end; ++s) {
    const u64 sec = s * MMC_BLOCK_SIZE;
    if (baseline_) changed_.insert(s);
    auto [it, fresh] = written_.try_emplace(s);
    // A write that covers only part of a sector keeps the rest of it.
    if (fresh && (addr > sec || addr + len < sec + MMC_BLOCK_SIZE)) read_file(sec, MMC_BLOCK_SIZE, it->second.data());
    const u64 from = std::max(sec, addr), to = std::min(sec + MMC_BLOCK_SIZE, addr + len);
    std::memcpy(it->second.data() + (from - sec), in + (from - addr), static_cast<size_t>(to - from));
  }
}

void NandImage::flush() { if (file_) std::fflush(file_); }

// DS_NAND_LOG=<file> writes the block log in trace_melonds's format, so
// tools/dsi_nand.py map reads ours and the oracle's alike.
void NandImage::log_access(bool write, u64 addr, u32 len) {
  static std::FILE* log = [] {
    const char* path = getenv("DS_NAND_LOG");
    return path ? std::fopen(path, "w") : nullptr;
  }();
  if (log) { std::fprintf(log, "%c %llx %u\n", write ? 'w' : 'r', (unsigned long long)addr, len); std::fflush(log); }
}

// ---- SdHost ------------------------------------------------------------------

SdHost::SdHost(NDS& nds, u32 num) : nds_(nds), num_(num) {
  if (num_ == 1) wifi_ = std::make_unique<NWifi>(nds, *this);
}
SdHost::~SdHost() = default;
SdDevice* SdHost::port0() { return wifi_.get(); }

void SdHost::attach_nand(NandImage* nand) {
  nand_ = nand;
  storage_.reset();
  if (nand && nand->valid()) storage_ = std::make_unique<MmcStorage>(nds_, *this, *nand);
}

u32 SdHost::irq2_main() const { return num_ ? IRQ2_SDIO : IRQ2_SDMMC; }
u32 SdHost::irq2_data1() const { return num_ ? IRQ2_SDIO_DATA1 : IRQ2_SD_DATA1; }

void SdHost::reset() {
  port_select_ = num_ ? 0x0100 : 0x0200;      // melonDS's reset values (CHECKME there too)
  soft_reset_ = 0x0007;
  sd_clock_ = 0;
  sd_option_ = 0;

  command_ = 0;
  param_ = 0;
  std::memset(response_buffer_, 0, sizeof(response_buffer_));

  data_fifo_[0].clear();
  data_fifo_[1].clear();
  cur_fifo_ = 0;
  data_fifo32_.clear();

  irq_status_ = 0;
  irq_mask_ = 0x8B7F031D;

  card_irq_status_ = 0;
  card_irq_mask_ = 0xC007;
  card_irq_ctl_ = 0;

  data_ctl_ = 0;
  data32_irq_ = 0;
  data_mode_ = 0;
  block_count16_ = block_count32_ = block_count_internal_ = 0;
  block_len16_ = block_len32_ = 0;
  stop_action_ = 0;
  tx_req_ = false;

  if (storage_) storage_->reset();
  if (wifi_) wifi_->reset();
}

// melonDS's ScheduleEvent drops the request when the event is already live,
// keeping the first timestamp and function; ours would overwrite it, which
// silently loses the pending completion (and can swap RX for TX) when two
// blocks land inside one delay. Match melonDS and refuse.
void SdHost::schedule_transfer(u32 which) {
  const EventId ev = num_ ? EventId::Sdio : EventId::SdMmc;
  if (nds_.sched.armed(ev)) return;
  nds_.sched.schedule(ev, nds_.sched.now() + TRANSFER_DELAY, num_ ? ev_transfer_sdio : ev_transfer_mmc, which);
}

void SdHost::ev_transfer_mmc(NDS& nds, u32 param) {
  if (param == TRANSFER_RX) nds.io.sd.finish_rx();
  else nds.io.sd.finish_tx();
}
void SdHost::ev_transfer_sdio(NDS& nds, u32 param) {
  if (param == TRANSFER_RX) nds.io.sdio.finish_rx();
  else nds.io.sdio.finish_tx();
}

void SdHost::update_data32_irq() {
  if (data_mode_ == 0) return;

  u32 oldflags = ((data32_irq_ >> 8) & 0x1) | (((~data32_irq_) >> 8) & 0x2);
  oldflags &= (data32_irq_ >> 11);

  data32_irq_ &= ~0x0300;
  if (data_fifo32_.level >= (block_len32_ >> 2)) data32_irq_ |= 1 << 8;
  if (!data_fifo32_.empty())                     data32_irq_ |= 1 << 9;

  u32 newflags = ((data32_irq_ >> 8) & 0x1) | (((~data32_irq_) >> 8) & 0x2);
  newflags &= (data32_irq_ >> 11);

  if (oldflags == 0 && newflags != 0) nds_.io.request_irq2(irq2_main());
}

void SdHost::set_irq(u32 irq) {
  const u32 oldflags = irq_status_ & ~irq_mask_;
  irq_status_ |= 1u << irq;
  const u32 newflags = irq_status_ & ~irq_mask_;
  if (oldflags == 0 && newflags != 0) nds_.io.request_irq2(irq2_main());
}

void SdHost::update_irq(u32 oldmask) {
  const u32 oldflags = irq_status_ & ~oldmask;
  const u32 newflags = irq_status_ & ~irq_mask_;
  if (oldflags == 0 && newflags != 0) nds_.io.request_irq2(irq2_main());
}

void SdHost::set_card_irq() {
  if (!(card_irq_ctl_ & 1)) return;

  const u16 oldflags = card_irq_status_ & ~card_irq_mask_;
  SdDevice* dev = device();
  if (dev && dev->irq) card_irq_status_ |=  1;
  else                 card_irq_status_ &= ~1;
  const u16 newflags = card_irq_status_ & ~card_irq_mask_;

  if (oldflags == 0 && newflags != 0) {
    nds_.io.request_irq2(irq2_main());
    nds_.io.request_irq2(irq2_data1());
  }
}

void SdHost::update_card_irq(u16 oldmask) {
  const u16 oldflags = card_irq_status_ & ~oldmask;
  const u16 newflags = card_irq_status_ & ~card_irq_mask_;
  if (oldflags == 0 && newflags != 0) {
    nds_.io.request_irq2(irq2_main());
    nds_.io.request_irq2(irq2_data1());
  }
}

void SdHost::send_response(u32 val, bool last) {
  std::memmove(&response_buffer_[2], &response_buffer_[0], 6 * sizeof(u16));
  std::memcpy(&response_buffer_[0], &val, sizeof(u32));
  if (last) set_irq(0);
}

void SdHost::finish_rx() {
  check_swap_fifo();
  if (data_mode_ == 1) update_fifo32();
  else set_irq(24);
}

u32 SdHost::data_rx(const u8* data, u32 len) {
  if (len != block_len16_) len = block_len16_;

  const u32 f = cur_fifo_ ^ 1;
  for (u32 i = 0; i < len; i += 2) {
    u16 v;
    std::memcpy(&v, &data[i], sizeof(v));
    data_fifo_[f].write(v);
  }

  // The delay is load-bearing, not cosmetic: DSi boot2 sends a command and
  // then polls IRQ0, and an instant IRQ24 would let the handler clear IRQ0
  // before the send-command routine starts looking (melonDS's note).
  schedule_transfer(TRANSFER_RX);
  return len;
}

void SdHost::finish_tx() {
  SdDevice* dev = device();
  if (block_count_internal_ == 0) {
    if (stop_action_ & (1 << 8)) { if (dev) dev->send_cmd(MmcCmd::StopTransmission, 0); }
    set_irq(2);
    tx_req_ = false;
  } else {
    if (dev) dev->continue_transfer();
  }
}

u32 SdHost::data_tx(u8* data, u32 len) {
  tx_req_ = true;
  const u32 f = cur_fifo_;

  if (data_mode_ == 1) {
    if ((data_fifo32_.level << 2) < len) {
      if (data_fifo32_.empty()) {
        set_irq(25);
        nds_.ndma.check(Cpu::ARM7, num_ ? 0x29 : 0x28);
      }
      return 0;
    }
    // Drain FIFO32 into FIFO16.
    for (;;) {
      const u32 cf = cur_fifo_;
      if ((data_fifo_[cf].level << 1) >= block_len16_) break;
      if (data_fifo32_.empty()) break;
      const u32 val = data_fifo32_.read();
      data_fifo_[cf].write(static_cast<u16>(val & 0xFFFF));
      data_fifo_[cf].write(static_cast<u16>(val >> 16));
    }
    update_data32_irq();
    if (block_count32_ > 1) block_count32_--;
  } else {
    if ((data_fifo_[f].level << 1) < len) {
      if (data_fifo_[f].empty()) set_irq(25);
      return 0;
    }
  }

  for (u32 i = 0; i < len; i += 2) {
    const u16 v = data_fifo_[f].read();
    std::memcpy(&data[i], &v, sizeof(v));
  }

  cur_fifo_ ^= 1;
  block_count_internal_--;

  schedule_transfer(TRANSFER_TX);
  return len;
}

u32 SdHost::transferrable_len(u32 len) const {
  if (len > block_len16_) len = block_len16_;
  return len;
}

void SdHost::check_rx() {
  SdDevice* dev = device();
  check_swap_fifo();

  if (block_count_internal_ <= 1) {
    if (stop_action_ & (1 << 8)) { if (dev) dev->send_cmd(MmcCmd::StopTransmission, 0); }
    set_irq(2);
  } else {
    block_count_internal_--;
    if (dev) dev->continue_transfer();
  }
}

void SdHost::check_tx() {
  if (!tx_req_) return;

  if (data_mode_ == 1) {
    if ((data_fifo32_.level << 2) < block_len32_) return;
  } else {
    if ((data_fifo_[cur_fifo_].level << 1) < block_len16_) return;
  }

  SdDevice* dev = device();
  if (dev) dev->continue_transfer();
}

static bool sd_trace() { static const bool on = getenv("DS_DEBUG_SD") != nullptr; return on; }

u16 SdHost::read(u32 addr) {
  if (sd_trace()) std::fprintf(stderr, "[sd] r %03X\n", addr & 0x1FF);
  switch (addr & 0x1FF) {
  case 0x000: return command_;
  case 0x002: return port_select_ & 0x030F;
  case 0x004: return static_cast<u16>(param_ & 0xFFFF);
  case 0x006: return static_cast<u16>(param_ >> 16);

  case 0x008: return stop_action_;
  case 0x00A: return block_count16_;

  case 0x00C: case 0x00E: case 0x010: case 0x012:
  case 0x014: case 0x016: case 0x018: case 0x01A:
    return response_buffer_[((addr & 0x1FF) - 0x00C) >> 1];

  case 0x01C: {
    u16 ret = static_cast<u16>(irq_status_ & (0x031D | (num_ ? 2 : 0)));
    // Card presence. Host 0: port 0, the SD card slot, which is empty here, so
    // the "inserted" and "writable" bits stay clear, as melonDS reports it.
    // Host 1: the Wi-Fi module is soldered on -- always inserted.
    if (num_) ret |= 0x00A0;
    return ret;
  }
  case 0x01E: return static_cast<u16>((irq_status_ >> 16) & 0x8B7F);
  case 0x020: return static_cast<u16>(irq_mask_ & 0x031D);
  case 0x022: return static_cast<u16>((irq_mask_ >> 16) & 0x8B7F);

  case 0x024: return sd_clock_;
  case 0x026: return block_len16_;
  case 0x028: return sd_option_;

  case 0x02C: return 0;
  case 0x02E: return 0;

  case 0x030: return read_fifo16();

  case 0x034: return card_irq_ctl_;
  case 0x036: return card_irq_status_;
  case 0x038: return card_irq_mask_;

  case 0x0D8: return data_ctl_;
  case 0x0E0: return soft_reset_;
  case 0x0F6: return 0;   // MMC write protect, always 0

  case 0x100: return data32_irq_;
  case 0x102: return 0;
  case 0x104: return block_len32_;
  case 0x108: return block_count32_;
  case 0x106: return 0;
  case 0x10A: return 0;
  default: break;
  }
  return 0;
}

u16 SdHost::read_fifo16() {
  const u32 f = cur_fifo_;
  if (data_fifo_[f].empty()) return 0;   // hardware wraps around; melonDS returns 0

  const u16 ret = data_fifo_[f].read();
  if (data_fifo_[f].empty()) check_rx();
  return ret;
}

u32 SdHost::read_fifo32() {
  if (data_mode_ != 1) return 0;
  if (data_fifo32_.empty()) return 0;

  const u32 ret = data_fifo32_.read();
  if (data_fifo32_.empty()) check_rx();
  update_data32_irq();
  return ret;
}

void SdHost::write(u32 addr, u16 val) {
  if (sd_trace()) std::fprintf(stderr, "[sd] w %03X = %04X\n", addr & 0x1FF, val);
  switch (addr & 0x1FF) {
  case 0x000: {
    command_ = val;
    const u8 cmd = command_ & 0x3F;
    SdDevice* dev = device();
    if (!dev) return;
    // Command type 1 is "ACMD", which on hardware sends an APP_CMD prefix of
    // its own -- but DSi boot2 sends APP_CMD manually *and* sets the type, so
    // melonDS treats both types the same and lets the CSR's APP_CMD bit do
    // the routing.
    const u32 type = (command_ >> 6) & 3;
    if (type <= 1) dev->send_cmd(static_cast<MmcCmd>(cmd), param_);
    return;
  }

  case 0x002: port_select_ = (val & 0x040F) | (port_select_ & 0x0300); return;
  case 0x004: param_ = (param_ & 0xFFFF0000) | val; return;
  case 0x006: param_ = (param_ & 0x0000FFFF) | (static_cast<u32>(val) << 16); return;

  case 0x008: stop_action_ = val & 0x0101; return;
  case 0x00A: block_count16_ = val; block_count_internal_ = val; return;

  case 0x01C: irq_status_ &= (val | 0xFFFF0000); return;
  case 0x01E: irq_status_ &= ((static_cast<u32>(val) << 16) | 0xFFFF); return;
  case 0x020: {
    const u32 oldmask = irq_mask_;
    irq_mask_ = (irq_mask_ & 0x8B7F0000) | (val & 0x031D);
    update_irq(oldmask);
    return;
  }
  case 0x022: {
    const u32 oldmask = irq_mask_;
    irq_mask_ = (irq_mask_ & 0x0000031D) | ((static_cast<u32>(val) & 0x8B7F) << 16);
    update_irq(oldmask);
    return;
  }

  case 0x024: sd_clock_ = val & 0x03FF; return;
  case 0x026:
    block_len16_ = val & 0x03FF;
    if (block_len16_ > MMC_BLOCK_SIZE) block_len16_ = MMC_BLOCK_SIZE;
    return;
  case 0x028: sd_option_ = val & 0xC1FF; return;

  case 0x030: write_fifo16(val); return;

  case 0x034: card_irq_ctl_ = val & 0x0305; set_card_irq(); return;
  case 0x036: card_irq_status_ &= val; return;
  case 0x038: {
    const u16 oldmask = card_irq_mask_;
    card_irq_mask_ = val & 0xC007;
    update_card_irq(oldmask);
    return;
  }

  case 0x0D8:
    data_ctl_ = val & 0x0022;
    data_mode_ = ((data_ctl_ >> 1) & 1) & ((data32_irq_ >> 1) & 1);
    return;

  case 0x0E0:
    if ((soft_reset_ & 1) && !(val & 1)) {
      stop_action_ = 0;
      std::memset(response_buffer_, 0, sizeof(response_buffer_));
      irq_status_ = 0;
      sd_clock_ &= ~0x0500;
      sd_option_ = 0x40EE;
      if (storage_) storage_->reset();
      if (wifi_) wifi_->reset();
    }
    soft_reset_ = 0x0006 | (val & 1);
    return;

  case 0x100:
    data32_irq_ = (val & 0x1802) | (data32_irq_ & 0x0300);
    if (val & (1 << 10)) data_fifo32_.clear();
    data_mode_ = ((data_ctl_ >> 1) & 1) & ((data32_irq_ >> 1) & 1);
    return;
  case 0x102: return;
  case 0x104: block_len32_ = val & 0x03FF; return;
  case 0x108: block_count32_ = val; return;
  case 0x106: return;
  case 0x10A: return;
  default: break;
  }
}

void SdHost::write_fifo16(u16 val) {
  const u32 f = cur_fifo_;
  if (data_fifo_[f].full()) return;
  data_fifo_[f].write(val);
  check_tx();
}

void SdHost::write_fifo32(u32 val) {
  if (data_mode_ != 1) return;
  if (data_fifo32_.full()) return;
  data_fifo32_.write(val);
  check_tx();
  update_data32_irq();
}

void SdHost::update_fifo32() {
  if (data_mode_ != 1) return;

  for (;;) {
    const u32 f = cur_fifo_;
    if ((data_fifo32_.level << 2) >= block_len32_) break;
    if (data_fifo_[f].empty()) break;
    u32 val = data_fifo_[f].read();
    val |= static_cast<u32>(data_fifo_[f].read()) << 16;
    data_fifo32_.write(val);
  }

  update_data32_irq();

  if ((data_fifo32_.level << 2) >= block_len32_) nds_.ndma.check(Cpu::ARM7, num_ ? 0x29 : 0x28);
}

void SdHost::check_swap_fifo() {
  const u32 f = cur_fifo_;
  const bool cur_empty = (data_mode_ == 1) ? data_fifo32_.empty() : data_fifo_[f].empty();
  if (cur_empty && ((data_fifo_[f ^ 1].level << 1) >= block_len16_)) cur_fifo_ ^= 1;
}

// ---- MmcStorage --------------------------------------------------------------

void MmcStorage::reset() {
  std::memcpy(cid_, nand_.emmc_cid(), sizeof(cid_));

  csr_ = 0x00000100;
  ocr_ = 0x80FF8000;

  static const u8 kCsdTemplate[16] = {0x40, 0x40, 0x96, 0xE9, 0x7F, 0xDB, 0xF6, 0xDF,
                                      0x01, 0x59, 0x0F, 0x2A, 0x01, 0x26, 0x90, 0x00};
  std::memcpy(csd_, kCsdTemplate, sizeof(csd_));

  std::memset(scr_, 0, sizeof(scr_));
  const u32 scr0 = 0x012A0000;
  std::memcpy(&scr_[0], &scr0, sizeof(scr0));
  std::memset(ssr_, 0, sizeof(ssr_));

  block_size_ = MMC_BLOCK_SIZE;
  rw_address_ = 0;
  rw_command_ = MmcCmd::Reset;
}

void MmcStorage::send_cmd(MmcCmd cmd, u32 param) {
  if (csr_ & (1 << 5)) {
    csr_ &= ~(1u << 5);
    send_acmd(static_cast<MmcAcmd>(cmd), param);
    return;
  }

  auto word = [&](const u8* p, u32 off) { u32 v; std::memcpy(&v, p + off, sizeof(v)); return v; };

  switch (cmd) {
  case MmcCmd::Reset:
    host_.send_response(csr_, true);
    return;

  case MmcCmd::GetOcr:
    // The eMMC is not high-capacity addressed: bit 30 never sets.
    param &= ~(1u << 30);
    ocr_ &= 0xBF000000;
    ocr_ |= param & 0x40FFFFFF;
    host_.send_response(ocr_, true);
    set_state(0x01);
    return;

  case MmcCmd::AllGetCid:
  case MmcCmd::GetCid:
    host_.send_response(word(cid_, 12), false);
    host_.send_response(word(cid_, 8), false);
    host_.send_response(word(cid_, 4), false);
    host_.send_response(word(cid_, 0), true);
    if (cmd == MmcCmd::AllGetCid) set_state(0x02);
    return;

  case MmcCmd::GetRca:
    rca_ = param >> 16;
    host_.send_response(csr_ | 0x10000, true);
    return;

  case MmcCmd::Switch:
    host_.send_response(csr_, true);
    return;

  case MmcCmd::Select:
    host_.send_response(csr_, true);
    return;

  case MmcCmd::SetVoltage:
    host_.send_response(param, true);
    return;

  case MmcCmd::GetCsd:
    host_.send_response(word(csd_, 12), false);
    host_.send_response(word(csd_, 8), false);
    host_.send_response(word(csd_, 4), false);
    host_.send_response(word(csd_, 0), true);
    return;

  case MmcCmd::StopTransmission:
    set_state(0x04);
    nand_.flush();
    rw_command_ = MmcCmd::Reset;
    host_.send_response(csr_, true);
    return;

  case MmcCmd::GetCsr:
    host_.send_response(csr_, true);
    return;

  case MmcCmd::SetBlockLength:
    block_size_ = param;
    if (block_size_ > MMC_BLOCK_SIZE) block_size_ = MMC_BLOCK_SIZE;
    set_state(0x04);
    host_.send_response(csr_, true);
    return;

  case MmcCmd::ReadSingleBlock:
  case MmcCmd::ReadMultipleBlocks:
    rw_address_ = param;
    if (ocr_ & (1 << 30)) { rw_address_ <<= 9; block_size_ = MMC_BLOCK_SIZE; }
    rw_command_ = cmd;
    host_.send_response(csr_, true);
    rw_address_ += read_block(rw_address_);
    set_state(0x05);
    return;

  case MmcCmd::WriteSingleBlock:
  case MmcCmd::WriteMultipleBlocks:
    rw_address_ = param;
    if (ocr_ & (1 << 30)) { rw_address_ <<= 9; block_size_ = MMC_BLOCK_SIZE; }
    rw_command_ = cmd;
    host_.send_response(csr_, true);
    rw_address_ += write_block(rw_address_);
    set_state(0x04);
    return;

  case MmcCmd::AppCommand:
    csr_ |= 1 << 5;
    host_.send_response(csr_, true);
    return;

  default:
    break;
  }
  std::fprintf(stderr, "[mmc] unknown CMD %u %08X\n", static_cast<u32>(cmd), param);
}

void MmcStorage::send_acmd(MmcAcmd cmd, u32 param) {
  switch (cmd) {
  case MmcAcmd::SetBusWidth:
    host_.send_response(csr_, true);
    return;

  case MmcAcmd::GetSsr:
    host_.send_response(csr_, true);
    host_.data_rx(ssr_, 64);
    return;

  case MmcAcmd::SetOcr:
    // boot2 hardcodes 0x40100000 and branches on whether bit 30 took; on the
    // eMMC it does not.
    param &= ~(1u << 30);
    ocr_ &= 0xBF000000;
    ocr_ |= param & 0x40FFFFFF;
    host_.send_response(ocr_, true);
    set_state(0x01);
    return;

  case MmcAcmd::SetCardDetect:
    host_.send_response(csr_, true);
    return;

  case MmcAcmd::GetScr:
    host_.send_response(csr_, true);
    host_.data_rx(scr_, 8);
    return;

  default:
    break;
  }
  std::fprintf(stderr, "[mmc] unknown ACMD %u %08X\n", static_cast<u32>(cmd), param);
}

void MmcStorage::continue_transfer() {
  if (rw_command_ == MmcCmd::Reset) return;

  u32 len = 0;
  switch (rw_command_) {
  case MmcCmd::ReadSingleBlock:
    rw_command_ = MmcCmd::Reset;
    [[fallthrough]];
  case MmcCmd::ReadMultipleBlocks:
    len = read_block(rw_address_);
    break;

  case MmcCmd::WriteSingleBlock:
    rw_command_ = MmcCmd::Reset;
    [[fallthrough]];
  case MmcCmd::WriteMultipleBlocks:
    len = write_block(rw_address_);
    break;

  default:
    break;
  }
  rw_address_ += len;
}

u32 MmcStorage::read_block(u64 addr) {
  u32 len = host_.transferrable_len(block_size_);
  u8 data[MMC_BLOCK_SIZE];
  // melonDS indexes the block buffer by addr&0x1FF; every transfer the DSi
  // makes is sector-aligned, so this is 0, but clamp rather than smash the
  // stack if a title ever asks for something else.
  if ((addr & 0x1FF) + len > MMC_BLOCK_SIZE) len = MMC_BLOCK_SIZE - (addr & 0x1FF);
  nand_.read(addr, len, &data[addr & 0x1FF]);
  return host_.data_rx(&data[addr & 0x1FF], len);
}

u32 MmcStorage::write_block(u64 addr) {
  u32 len = host_.transferrable_len(block_size_);
  u8 data[MMC_BLOCK_SIZE];
  if ((addr & 0x1FF) + len > MMC_BLOCK_SIZE) len = MMC_BLOCK_SIZE - (addr & 0x1FF);
  len = host_.data_tx(&data[addr & 0x1FF], len);
  if (len && !read_only) nand_.write(addr, len, &data[addr & 0x1FF]);
  return len;
}

// ---- save state ---------------------------------------------------------------
// Registers and FIFOs only. The NAND's contents are the backing file, which a
// state does not carry; a dirty-sector journal is phase 3's save work.

template <class S> void MmcStorage::sync_state(S& s) {
  s.fields(cid_, csd_, csr_, ocr_, rca_, scr_, ssr_, block_size_, rw_address_, rw_command_, irq, read_only);
}
template void MmcStorage::sync_state<state::Writer>(state::Writer&);
template void MmcStorage::sync_state<state::Reader>(state::Reader&);

template <class S> void SdHost::sync_state(S& s) {
  s.fields(port_select_, soft_reset_, sd_clock_, sd_option_, irq_status_, irq_mask_,
           card_irq_status_, card_irq_mask_, card_irq_ctl_, data_ctl_, data32_irq_, data_mode_,
           block_count16_, block_count32_, block_count_internal_, block_len16_, block_len32_,
           stop_action_, command_, param_, response_buffer_, cur_fifo_, tx_req_,
           data_fifo_[0].buf, data_fifo_[0].read_pos, data_fifo_[0].write_pos, data_fifo_[0].level,
           data_fifo_[1].buf, data_fifo_[1].read_pos, data_fifo_[1].write_pos, data_fifo_[1].level,
           data_fifo32_.buf, data_fifo32_.read_pos, data_fifo32_.write_pos, data_fifo32_.level);
  if (storage_) storage_->sync_state(s);
  if (wifi_) wifi_->sync_state(s);
}
template void SdHost::sync_state<state::Writer>(state::Writer&);
template void SdHost::sync_state<state::Reader>(state::Reader&);

}  // namespace ds::io
