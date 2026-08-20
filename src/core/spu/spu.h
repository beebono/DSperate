// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
namespace ds { struct NDS; }
namespace ds::spu {
class Spu {
public:
  explicit Spu(NDS& nds) : nds_(nds) {}
  void reset() {}
private:
  NDS& nds_;
};
}
