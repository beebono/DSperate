// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Where a slot-2 motion accessory's readings come from: the handheld's own
// accelerometer and gyroscope where it has them (Linux IIO), else a stick,
// whose deflection tilts a console that is otherwise lying flat.
#pragma once
#include "core/slot2/slot2.h"

#include <atomic>
#include <string>
#include <thread>

namespace ds::sdl {

class MotionSource {
public:
  enum class Kind { None, Sensor, Stick };
  ~MotionSource() { close(); }
  // `want`: auto (the sensor if there is one, else the stick), sensor or
  // stick. `matrix`: nine numbers, rows of a rotation from the sensor's axes
  // to the console's, for a sensor the system gives no mount matrix for;
  // empty takes the system's (or the identity).
  void open(const std::string& want, const std::string& matrix);
  void close();
  Kind kind() const { return kind_; }
  const std::string& name() const { return name_; }   // the sensor's, for the log
  // The reading for this frame. `sx`, `sy`: the stick, -1..1, right and up positive.
  slot2::Motion read(float sx, float sy) const;

  // A stick's deflection as a tilt of up to 45 degrees each way.
  static slot2::Motion from_stick(float sx, float sy);
  // The IIO accelerometer to use, or empty: the first device with in_accel_x_raw.
  static std::string find_accel(const std::string& root = "/sys/bus/iio/devices");

private:
  Kind kind_ = Kind::None;
  std::string name_, accel_dir_, gyro_dir_;
  float m_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float accel_scale_ = 0, gyro_scale_ = 0;
  std::thread thread_;
  std::atomic<bool> run_{false};
  std::atomic<float> ax_{0}, ay_{0}, az_{9.80665f}, gz_{0};
  void poll();
};

} // namespace ds::sdl
