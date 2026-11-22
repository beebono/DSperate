// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The still a DSi's cameras look at, from a picture file (JPEG, PNG or BMP,
// decoded by the vendored stb_image). Included by one file of each frontend.
#pragma once
#include "core/io/dsi_camera.h"
#include "core/nds.h"

#include <cstdio>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ds::frontend {

// Puts `path`'s picture in front of both cameras; an empty path, or a file
// that will not decode, puts the colour bars there. False (and `err`) only
// for a file that was named and could not be used.
inline bool set_camera_image(NDS& nds, const std::string& path, std::string& err) {
  if (path.empty()) { nds.io.cam.set_image(nullptr); return true; }
  int w = 0, h = 0, n = 0;
  // A picture's size is the file's word: nothing a camera would give is past 8192 a side.
  if (!stbi_info(path.c_str(), &w, &h, &n) || w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    err = path + ": not a JPEG, PNG or BMP picture this can read";
    nds.io.cam.set_image(nullptr);
    return false;
  }
  unsigned char* rgb = stbi_load(path.c_str(), &w, &h, &n, 3);
  if (!rgb) { err = path + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "cannot be decoded"); nds.io.cam.set_image(nullptr); return false; }
  std::vector<u32> frame(io::DsiCamera::W * io::DsiCamera::H / 2);
  io::DsiCamera::frame_from_rgb(rgb, w, h, frame.data());
  stbi_image_free(rgb);
  nds.io.cam.set_image(frame.data());
  return true;
}

} // namespace ds::frontend
