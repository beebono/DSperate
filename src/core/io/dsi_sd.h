// DSi SD/MMC host and the eMMC (NAND) device behind it.
//
// A port of melonDS's DSi_SD.cpp: the SDMMC host at 0x04004800-0x040049FF
// with its command/response registers, the two 16-bit data FIFOs and the
// 32-bit one they drain into, the IRQ/card-IRQ masks, and an MMC device on
// port 1 holding the NAND. Port 0 (the SD card slot) is absent and reads as
// "no card". The same controller, instance 1, is the SDIO host at
// 0x04004A00 with the Atheros Wi-Fi module on its port 0 (melonDS DSi_NWifi).
//
// The guest sees *raw* eMMC sectors here. The NAND's AES-CTR lives in
// software above this host -- melonDS's NANDImage only decrypts for its own
// host-side FAT view, never on the path the guest reads -- so nothing in
// this file encrypts anything.

#ifndef DS_CORE_IO_DSI_SD_H
#define DS_CORE_IO_DSI_SD_H

#include <array>
#include <cstdio>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "core/types.h"

namespace ds {
struct NDS;
}

namespace ds::io {

// MMC commands the DSi's SDK and boot code issue (melonDS MMCCommand).
enum class MmcCmd : u32 {
  Reset = 0, GetOcr = 1, AllGetCid = 2, GetRca = 3, SdioOpCond = 5, Switch = 6,
  Select = 7, SetVoltage = 8, GetCsd = 9, GetCid = 10, StopTransmission = 12,
  GetCsr = 13, SetBlockLength = 16, ReadSingleBlock = 17, ReadMultipleBlocks = 18,
  WriteSingleBlock = 24, WriteMultipleBlocks = 25, IoRwDirect = 52,
  IoRwExtended = 53, AppCommand = 55, DataBlock = 56,
};
enum class MmcAcmd : u32 {
  SetBusWidth = 6, GetSsr = 13, GetWriteBlockCount = 22, SetWriteBlockCount = 23,
  SetOcr = 41, SetCardDetect = 42, GetScr = 51,
};

constexpr u32 MMC_BLOCK_SIZE = 512;

// A NAND image backed by a real nand.bin. The image carries a 0x40-byte
// nocash footer holding the eMMC CID and the console ID; without it we cannot
// key anything, so the open fails loudly.
//
// The dump is opened read-only and every guest write lands in memory, one
// 512-byte sector at a time, overlaid on the file for later reads: a dump is
// a file the user cannot regenerate (docs/dsiware-scoping.md 2.3). What
// persists is pulled back out of the written sectors as files. `write_through`
// is for the melonDS comparisons, which diff the written image on disk (run
// them on a copy).
class NandImage {
 public:
  ~NandImage();
  NandImage() = default;
  NandImage(const NandImage&) = delete;
  NandImage& operator=(const NandImage&) = delete;

  bool open(const std::string& path, bool write_through = false);
  // An image with no file behind it: every sector reads zero until written,
  // and writes are held in memory like a dump's. What a synthesised NAND is
  // built on (dsi_nand_synth.h).
  void create_in_memory(u64 length, const u8 cid[16], u64 console_id);
  void close();
  bool valid() const { return file_ != nullptr || in_memory_; }
  bool in_memory() const { return in_memory_; }
  bool write_through() const { return write_through_; }
  // Sectors written this session and held in memory (empty under write_through).
  const std::unordered_map<u64, std::array<u8, 512>>& written_sectors() const { return written_; }
  // What changed since the image was ready. For a dump that is every written
  // sector; an image built in memory calls mark_baseline() once built (and
  // once its saves are imported), so only the session's own writes count.
  void mark_baseline() { baseline_ = true; changed_.clear(); }
  bool changed(u64 sector) const { return baseline_ ? changed_.count(sector) != 0 : written_.count(sector) != 0; }
  bool any_changed() const { return baseline_ ? !changed_.empty() : !written_.empty(); }

  u64 console_id() const { return console_id_; }
  const u8* emmc_cid() const { return cid_; }
  u64 length() const { return length_; }

  // Raw sector access, as the guest sees it. `addr` is a byte offset.
  void read(u64 addr, u32 len, u8* out);
  void write(u64 addr, u32 len, const u8* in);
  void flush();
  // The same, for the emulator's own filesystem work (NandFs): not counted,
  // not logged, so the guest's access record stays the guest's.
  void peek(u64 addr, u32 len, u8* out);
  void poke(u64 addr, u32 len, const u8* in);

  // Access counters, for the melonDS gate's "nand: N block reads/writes" line.
  u64 reads = 0, writes = 0;
  // A byte range whose first guest read is remembered: how a synthesised NAND
  // tells that a title went looking in /sys (for the system font it lacks).
  void watch_reads(u64 from, u64 to) { watch_from_ = from; watch_to_ = to; watch_hit_ = false; }
  bool watch_hit() const { return watch_hit_; }

 private:
  static void log_access(bool write, u64 addr, u32 len);

 public:

 private:
  void read_file(u64 addr, u32 len, u8* out);

  std::FILE* file_ = nullptr;
  bool in_memory_ = false;
  u64  watch_from_ = 0, watch_to_ = 0;
  bool watch_hit_ = false;
  bool write_through_ = false;
  u64 length_ = 0;
  u8  cid_[16] = {};
  u64 console_id_ = 0;
  std::unordered_map<u64, std::array<u8, 512>> written_;   // sector index -> contents
  std::unordered_set<u64> changed_;                         // written since mark_baseline()
  bool baseline_ = false;
};

class SdHost;
class NWifi;

// A device on one of a host's two ports (melonDS DSi_SDDevice).
class SdDevice {
 public:
  virtual ~SdDevice() = default;
  virtual void reset() = 0;
  virtual void send_cmd(MmcCmd cmd, u32 param) = 0;
  virtual void send_acmd(MmcAcmd cmd, u32 param) = 0;
  virtual void continue_transfer() = 0;
  bool irq = false;
  bool read_only = false;
};

// The eMMC device on port 1. melonDS's DSi_MMCStorage, NAND-only: the SD
// card variants of each command are the branches we never take.
class MmcStorage : public SdDevice {
 public:
  MmcStorage(NDS& nds, SdHost& host, NandImage& nand) : nds_(nds), host_(host), nand_(nand) {}

  void reset() override;
  void send_cmd(MmcCmd cmd, u32 param) override;
  void send_acmd(MmcAcmd cmd, u32 param) override;
  void continue_transfer() override;

  template <class S> void sync_state(S& s);

 private:
  void set_state(u32 state) { csr_ &= ~(0xFu << 9); csr_ |= state << 9; }
  u32  read_block(u64 addr);
  u32  write_block(u64 addr);

  NDS& nds_;
  SdHost& host_;
  NandImage& nand_;

  u8  cid_[16] = {};
  u8  csd_[16] = {};
  u32 csr_ = 0, ocr_ = 0, rca_ = 0;
  u8  scr_[8] = {};
  u8  ssr_[64] = {};
  u32 block_size_ = MMC_BLOCK_SIZE;
  u64 rw_address_ = 0;
  MmcCmd rw_command_ = MmcCmd::Reset;
};

class SdHost {
 public:
  // num 0: the SDMMC host (0x04004800, NAND on port 1); 1: the SDIO host
  // (0x04004A00, Wi-Fi on port 0). melonDS DSi_SDHost::Num.
  SdHost(NDS& nds, u32 num);
  ~SdHost();
  NWifi* nwifi() { return wifi_.get(); }
  void set_card_irq();   // a device's IRQ line changed (melonDS SetCardIRQ)
  u32 num() const { return num_; }

  void reset();
  void attach_nand(NandImage* nand);
  bool has_nand() const { return storage_ != nullptr; }

  u16  read(u32 addr);
  void write(u32 addr, u16 val);
  u16  read_fifo16();
  void write_fifo16(u16 val);
  u32  read_fifo32();
  void write_fifo32(u32 val);

  // Called by the device.
  void send_response(u32 val, bool last);
  u32  data_rx(const u8* data, u32 len);
  u32  data_tx(u8* data, u32 len);
  u32  transferrable_len(u32 len) const;

  // Scheduler callbacks (Event_DSi_SDMMCTransfer / SDIOTransfer's two function ids).
  static void ev_transfer_mmc(NDS& nds, u32 param);
  static void ev_transfer_sdio(NDS& nds, u32 param);
  void schedule_transfer(u32 which);

  template <class S> void sync_state(S& s);

 private:
  void finish_rx();
  void finish_tx();
  void check_rx();
  void check_tx();
  void update_fifo32();
  void check_swap_fifo();
  void update_data32_irq();
  void set_irq(u32 irq);
  void update_irq(u32 oldmask);
  void update_card_irq(u16 oldmask);
  SdDevice* device() { return (port_select_ & 1) ? port1() : port0(); }
  SdDevice* port0();                                                  // SD card slot (host 0, empty) / Wi-Fi (host 1)
  SdDevice* port1() { return num_ == 0 ? storage_.get() : nullptr; }  // the eMMC on host 0
  u32 irq2_main() const;
  u32 irq2_data1() const;

  // melonDS's FIFO<u16,0x100> / FIFO<u32,0x80>, as plain ring buffers.
  template <typename T, u32 N>
  struct Fifo {
    T buf[N] = {};
    u32 read_pos = 0, write_pos = 0, level = 0;
    void clear() { read_pos = write_pos = level = 0; }
    bool empty() const { return level == 0; }
    bool full() const { return level == N; }
    void write(T v) { if (level == N) return; buf[write_pos] = v; write_pos = (write_pos + 1) % N; level++; }
    T read() { if (!level) return T{}; T v = buf[read_pos]; read_pos = (read_pos + 1) % N; level--; return v; }
  };

  NDS& nds_;
  u32 num_ = 0;
  NandImage* nand_ = nullptr;
  std::unique_ptr<MmcStorage> storage_;
  std::unique_ptr<NWifi> wifi_;

  u16 port_select_ = 0, soft_reset_ = 0, sd_clock_ = 0, sd_option_ = 0;
  u32 irq_status_ = 0, irq_mask_ = 0;
  u16 card_irq_status_ = 0, card_irq_mask_ = 0, card_irq_ctl_ = 0;
  u16 data_ctl_ = 0, data32_irq_ = 0;
  u32 data_mode_ = 0;
  u16 block_count16_ = 0, block_count32_ = 0, block_count_internal_ = 0;
  u16 block_len16_ = 0, block_len32_ = 0;
  u16 stop_action_ = 0;
  u16 command_ = 0;
  u32 param_ = 0;
  u16 response_buffer_[8] = {};
  u32 cur_fifo_ = 0;
  bool tx_req_ = false;
  Fifo<u16, 0x100> data_fifo_[2];
  Fifo<u32, 0x80> data_fifo32_;

  friend class MmcStorage;
};

}  // namespace ds::io

#endif  // DS_CORE_IO_DSI_SD_H
