// DSi SD/MMC host and the eMMC (NAND) device behind it.
//
// A port of melonDS's DSi_SD.cpp: the SDMMC host at 0x04004800-0x040049FF
// with its command/response registers, the two 16-bit data FIFOs and the
// 32-bit one they drain into, the IRQ/card-IRQ masks, and an MMC device on
// port 1 holding the NAND. Port 0 (the SD card slot) is absent and reads as
// "no card"; the SDIO host at 0x04004A00 is absent entirely (the Wi-Fi SDIO
// block is gated off in SCFG_EXT), so that range reads 0.
//
// The guest sees *raw* eMMC sectors here. The NAND's AES-CTR lives in
// software above this host -- melonDS's NANDImage only decrypts for its own
// host-side FAT view, never on the path the guest reads -- so nothing in
// this file encrypts anything.

#ifndef DS_CORE_IO_DSI_SD_H
#define DS_CORE_IO_DSI_SD_H

#include <cstdio>
#include <string>
#include <memory>

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

// A NAND image backed by a real nand.bin (the oracle backer). The image
// carries a 0x40-byte nocash footer holding the eMMC CID and the console ID;
// without it we cannot key anything, so the open fails loudly.
class NandImage {
 public:
  ~NandImage();
  NandImage() = default;
  NandImage(const NandImage&) = delete;
  NandImage& operator=(const NandImage&) = delete;

  bool open(const std::string& path);
  void close();
  bool valid() const { return file_ != nullptr; }

  u64 console_id() const { return console_id_; }
  const u8* emmc_cid() const { return cid_; }
  u64 length() const { return length_; }

  // Raw sector access, as the guest sees it. `addr` is a byte offset.
  void read(u64 addr, u32 len, u8* out);
  void write(u64 addr, u32 len, const u8* in);
  void flush();

  // Access counters, for the melonDS gate's "nand: N block reads/writes" line.
  u64 reads = 0, writes = 0;

 private:
  static void log_access(bool write, u64 addr, u32 len);

 public:

 private:
  std::FILE* file_ = nullptr;
  u64 length_ = 0;
  u8  cid_[16] = {};
  u64 console_id_ = 0;
};

class SdHost;

// The eMMC device on port 1. melonDS's DSi_MMCStorage, NAND-only: the SD
// card variants of each command are the branches we never take.
class MmcStorage {
 public:
  MmcStorage(NDS& nds, SdHost& host, NandImage& nand) : nds_(nds), host_(host), nand_(nand) {}

  void reset();
  void send_cmd(MmcCmd cmd, u32 param);
  void send_acmd(MmcAcmd cmd, u32 param);
  void continue_transfer();

  bool irq = false;
  bool read_only = false;

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
  explicit SdHost(NDS& nds) : nds_(nds) {}

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

  // Scheduler callbacks (Event_DSi_SDMMCTransfer's two function ids).
  static void ev_transfer(NDS& nds, u32 param);
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
  void set_card_irq();
  void update_card_irq(u16 oldmask);
  MmcStorage* device() { return (port_select_ & 1) ? storage_.get() : nullptr; }

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
  NandImage* nand_ = nullptr;
  std::unique_ptr<MmcStorage> storage_;

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
