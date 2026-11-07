// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Reads the tables tools/make_dsi_font.py writes (GBATEK "DSi SD/MMC Firmware
// Font File"): a 0x80-byte signature, a 0x20-byte header whose byte 4 is the
// entry count, then 0x40-byte entries (name, packed size, file offset,
// unpacked size, SHA-1), each a Nitro font (NFTR) in the DSi's backwards LZ.
// Only what that generator emits is relied on, though every offset is
// bounds-checked so a bad table fails to a '?' rather than a crash.
#include "frontend/sdl/ui_font.h"
#include "core/io/dsi_nand_synth.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

namespace ds::sdl::uifont {
namespace {

u32 rd16(const std::vector<u8>& b, size_t at) { return at + 2 <= b.size() ? b[at] | b[at + 1] << 8 : 0; }
u32 rd32(const std::vector<u8>& b, size_t at) {
  return at + 4 <= b.size() ? b[at] | b[at + 1] << 8 | b[at + 2] << 16 | static_cast<u32>(b[at + 3]) << 24 : 0;
}

// The DSi's backwards LZ: the tail of the packed data decodes from the end
// towards the front. A footer gives the packed length and the growth. Empty
// on anything malformed.
std::vector<u8> blz_decode(const u8* data, size_t n) {
  if (n < 8) return {};
  const u32 inc = data[n - 4] | data[n - 3] << 8 | data[n - 2] << 16 | static_cast<u32>(data[n - 1]) << 24;
  if (inc == 0) return std::vector<u8>(data, data + n);
  const size_t hdr = data[n - 5];
  const size_t enc = data[n - 8] | data[n - 7] << 8 | data[n - 6] << 16;
  if (enc > n || hdr > enc || hdr < 8) return {};
  std::vector<u8> raw(n + inc);
  std::copy(data, data + (n - enc), raw.begin());
  size_t pak = n - hdr, r = raw.size();
  const size_t pak_end = n - enc, r_end = n - enc;
  u32 mask = 0, flags = 0;
  while (r > r_end) {
    mask >>= 1;
    if (!mask) {
      if (pak <= pak_end) return {};
      flags = data[--pak];
      mask = 0x80;
    }
    if (!(flags & mask)) {
      if (pak <= pak_end) return {};
      raw[--r] = data[--pak];
    } else {
      if (pak < pak_end + 2) return {};
      pak -= 2;
      const u32 pos = data[pak + 1] << 8 | data[pak];
      const size_t len = std::min<size_t>((pos >> 12) + 3, r - r_end);
      const size_t back = (pos & 0xFFF) + 3;
      if (r + back > raw.size()) return {};   // the first byte copied is raw[r - 1 + back]
      for (size_t k = 0; k < len; ++k) { --r; raw[r] = raw[r + back]; }
    }
  }
  return raw;
}

struct Font {
  std::vector<u8> nftr;
  size_t tiles = 0, widths = 0;   // offsets into nftr
  u32 width_first = 0, width_last = 0;
  int cell_w = 0, cell_h = 0, tile_bytes = 0, base = 0;
  std::vector<std::pair<u16, u16>> map;   // (code point, tile), sorted
  bool ok = false;
};

// Parses an unpacked NFTR: FINF's pointers to CGLP, CWDH and the CMAP chain.
// The pointers are to each block's data, 8 bytes past its magic.
bool parse(Font& f) {
  const std::vector<u8>& b = f.nftr;
  if (b.size() < 0x30 || b[0] != 'R' || b[1] != 'T' || b[2] != 'F' || b[3] != 'N') return false;
  const size_t cglp = rd32(b, 0x20), cwdh = rd32(b, 0x24);
  size_t cmap = rd32(b, 0x28);
  if (cglp + 8 > b.size() || cwdh + 8 > b.size() || cwdh < cglp + 16) return false;
  f.cell_w = b[cglp]; f.cell_h = b[cglp + 1];
  f.tile_bytes = static_cast<int>(rd16(b, cglp + 2));
  f.base = b[cglp + 4];
  if (b[cglp + 6] != 2 || f.cell_w == 0 || f.cell_h == 0 || f.tile_bytes < (f.cell_w * f.cell_h * 2 + 7) / 8) return false;
  f.tiles = cglp + 8;
  f.width_first = rd16(b, cwdh);
  f.width_last = rd16(b, cwdh + 2);
  f.widths = cwdh + 8;
  const size_t ntiles = (cwdh - 8 - f.tiles) / static_cast<size_t>(f.tile_bytes);
  if (f.widths + 3 * (f.width_last + 1 - f.width_first) > b.size()) return false;
  for (int guard = 0; cmap != 0 && guard < 4096; ++guard) {
    if (cmap + 12 > b.size()) return false;
    const u32 first = rd16(b, cmap), last = rd16(b, cmap + 2), type = rd16(b, cmap + 4);
    const size_t body = cmap + 12;
    if (type == 0) {
      const u32 tile = rd16(b, body);
      for (u32 c = first; c <= last; ++c) f.map.emplace_back(static_cast<u16>(c), static_cast<u16>(tile + c - first));
    } else if (type == 1) {
      for (u32 c = first; c <= last; ++c) {
        const u32 tile = rd16(b, body + 2 * (c - first));
        if (tile != 0xFFFF) f.map.emplace_back(static_cast<u16>(c), static_cast<u16>(tile));
      }
    } else if (type == 2) {
      const u32 n = rd16(b, body);
      if (body + 2 + 4 * static_cast<size_t>(n) > b.size()) return false;
      for (u32 k = 0; k < n; ++k)
        f.map.emplace_back(static_cast<u16>(rd16(b, body + 2 + 4 * k)), static_cast<u16>(rd16(b, body + 4 + 4 * k)));
    }
    cmap = rd32(b, cmap + 8);
  }
  f.map.erase(std::remove_if(f.map.begin(), f.map.end(), [&](const auto& e) {
    return e.second >= ntiles || e.second < f.width_first || e.second > f.width_last;
  }), f.map.end());
  std::sort(f.map.begin(), f.map.end());
  f.map.erase(std::unique(f.map.begin(), f.map.end(), [](const auto& a, const auto& c) { return a.first == c.first; }), f.map.end());
  return !f.map.empty();
}

// Font `which` (0-2) of the table for `region` (1 normal, 4 China, 5 Korea):
// the table's non-empty entries, in order.
Font unpack(u8 region, int which) {
  Font f;
  const io::FontBlob t = io::builtin_dsi_font_view(region);
  if (t.size < 0xA0) return f;
  const u32 count = t.data[0x84];
  int seen = 0;
  for (u32 e = 0; e < count; ++e) {
    const size_t at = 0xA0 + 0x40 * static_cast<size_t>(e);
    if (at + 0x40 > t.size) break;
    const u8* p = t.data + at;
    const u32 packed = p[0x20] | p[0x21] << 8 | p[0x22] << 16 | static_cast<u32>(p[0x23]) << 24;
    const u32 off = p[0x24] | p[0x25] << 8 | p[0x26] << 16 | static_cast<u32>(p[0x27]) << 24;
    const u32 raw = p[0x28] | p[0x29] << 8 | p[0x2A] << 16 | static_cast<u32>(p[0x2B]) << 24;
    if (packed == 0) continue;
    if (seen++ != which) continue;
    if (static_cast<size_t>(off) + packed > t.size) return f;
    f.nftr = blz_decode(t.data + off, packed);
    if (f.nftr.size() != raw) { f.nftr.clear(); return f; }
    f.ok = parse(f);
    if (!f.ok) f.nftr.clear();
    return f;
  }
  return f;
}

constexpr u8 kRegions[3] = {1, 4, 5};   // searched in this order

const Font& font(int table, Size size) {
  static std::array<Font, 9> fonts;
  static std::array<std::once_flag, 9> once;
  const int k = table * 3 + static_cast<int>(size);
  std::call_once(once[k], [&] { fonts[k] = unpack(kRegions[table], static_cast<int>(size)); });
  return fonts[k];
}

}  // namespace

bool find(u32 cp, Size size, Glyph& out) {
  if (cp > 0xFFFF) return false;   // the fonts are UCS-2
  for (int t = 0; t < 3; ++t) {
    const Font& f = font(t, size);
    if (!f.ok) continue;
    const auto it = std::lower_bound(f.map.begin(), f.map.end(), std::pair<u16, u16>{static_cast<u16>(cp), 0});
    if (it == f.map.end() || it->first != cp) continue;
    const size_t tile = it->second;
    const u8* w = f.nftr.data() + f.widths + 3 * (tile - f.width_first);
    out.tile = f.nftr.data() + f.tiles + tile * static_cast<size_t>(f.tile_bytes);
    out.cell_w = f.cell_w; out.cell_h = f.cell_h; out.base = f.base;
    out.left = static_cast<s8>(w[0]); out.ink = w[1]; out.adv = w[2];
    return true;
  }
  return false;
}

}  // namespace ds::sdl::uifont
