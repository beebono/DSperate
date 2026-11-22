// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "motion.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>

namespace ds::sdl {

namespace {
constexpr float kG = 9.80665f;

bool read_text(const std::string& path, char* buf, size_t n) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  const bool ok = std::fgets(buf, static_cast<int>(n), f) != nullptr;
  std::fclose(f);
  return ok;
}
bool read_float(const std::string& path, float& v) {
  char buf[64];
  if (!read_text(path, buf, sizeof buf)) return false;
  char* end = nullptr;
  const float x = std::strtof(buf, &end);
  if (end == buf) return false;
  v = x;
  return true;
}
// Nine numbers, with whatever sits between them (IIO writes "a, b, c; d, e, f; g, h, i").
bool parse_matrix(const char* s, float m[9]) {
  float t[9];
  for (int i = 0; i < 9; ++i) {
    while (*s && !(*s == '-' || *s == '+' || *s == '.' || (*s >= '0' && *s <= '9'))) ++s;
    char* end = nullptr;
    t[i] = std::strtof(s, &end);
    if (end == s) return false;
    s = end;
  }
  for (int i = 0; i < 9; ++i) m[i] = t[i];
  return true;
}
} // namespace

slot2::Motion MotionSource::from_stick(float sx, float sy) {
  const float max_tilt = 0.7853982f;   // 45 degrees
  const auto clamp1 = [](float v) { return v < -1 ? -1.f : v > 1 ? 1.f : v; };
  slot2::Motion m;
  m.ax = kG * std::sin(clamp1(sx) * max_tilt);
  m.ay = kG * std::sin(clamp1(sy) * max_tilt);
  const float rest = kG * kG - m.ax * m.ax - m.ay * m.ay;
  m.az = rest > 0 ? std::sqrt(rest) : 0;
  return m;
}

std::string MotionSource::find_accel(const std::string& root) {
  std::string found;
  if (DIR* d = opendir(root.c_str())) {
    while (const dirent* e = readdir(d)) {
      const std::string dir = root + "/" + e->d_name;
      if (std::string(e->d_name).rfind("iio:device", 0) != 0) continue;
      if (FILE* f = std::fopen((dir + "/in_accel_x_raw").c_str(), "r")) {
        std::fclose(f);
        if (found.empty() || dir < found) found = dir;   // the lowest number, whatever order readdir gives
      }
    }
    closedir(d);
  }
  return found;
}

void MotionSource::open(const std::string& want, const std::string& matrix, bool pad_has) {
  close();
  if (want == "off" || want == "none") return;
  if ((want == "auto" || want == "pad") && pad_has) {
    kind_ = Kind::Pad; name_ = "the controller's sensor";
    for (int i = 0; i < 9; ++i) m_[i] = i % 4 == 0 ? 1.f : 0.f;
    if (!matrix.empty() && !parse_matrix(matrix.c_str(), m_)) std::fprintf(stderr, "motion: slot2.motion_matrix wants nine numbers; ignored\n");
    return;
  }
  if (want != "stick") accel_dir_ = find_accel();
  if (accel_dir_.empty()) {
    if (want == "sensor" || want == "pad") std::fprintf(stderr, "motion: no accelerometer found (%s); the stick tilts instead\n", want == "pad" ? "controller" : "controller or IIO");
    kind_ = Kind::Stick;
    return;
  }
  char buf[128] = "";
  read_text(accel_dir_ + "/name", buf, sizeof buf);
  name_ = buf;
  while (!name_.empty() && (name_.back() == '\n' || name_.back() == ' ')) name_.pop_back();
  if (!read_float(accel_dir_ + "/in_accel_scale", accel_scale_)) accel_scale_ = 1;
  // The matching gyroscope: the device after it, if that is one.
  const size_t colon = accel_dir_.rfind("device");
  if (colon != std::string::npos) {
    const std::string next = accel_dir_.substr(0, colon + 6) + std::to_string(std::atoi(accel_dir_.c_str() + colon + 6) + 1);
    if (read_float(next + "/in_anglvel_scale", gyro_scale_)) gyro_dir_ = next;
  }
  for (int i = 0; i < 9; ++i) m_[i] = i % 4 == 0 ? 1.f : 0.f;
  if (!matrix.empty()) {
    if (!parse_matrix(matrix.c_str(), m_)) std::fprintf(stderr, "motion: slot2.motion_matrix wants nine numbers; ignored\n");
  } else {
    for (const char* f : {"/in_accel_mount_matrix", "/mount_matrix", "/in_mount_matrix"})
      if (read_text(accel_dir_ + f, buf, sizeof buf) && parse_matrix(buf, m_)) break;
  }
  kind_ = Kind::Sensor;
  run_ = true;
  thread_ = std::thread([this] { poll(); });
}

void MotionSource::close() {
  run_ = false;
  if (thread_.joinable()) thread_.join();
  kind_ = Kind::None;
  name_.clear(); accel_dir_.clear(); gyro_dir_.clear();
}

// Off the emulation thread: each read is an I2C transfer. 100 times a second.
void MotionSource::poll() {
  while (run_) {
    float v[3] = {0, 0, 0}, g[3] = {0, 0, 0};
    const bool ok = read_float(accel_dir_ + "/in_accel_x_raw", v[0]) && read_float(accel_dir_ + "/in_accel_y_raw", v[1]) &&
                    read_float(accel_dir_ + "/in_accel_z_raw", v[2]);
    if (ok) {
      for (float& x : v) x *= accel_scale_;
      ax_ = m_[0] * v[0] + m_[1] * v[1] + m_[2] * v[2];
      ay_ = m_[3] * v[0] + m_[4] * v[1] + m_[5] * v[2];
      az_ = m_[6] * v[0] + m_[7] * v[1] + m_[8] * v[2];
    }
    if (!gyro_dir_.empty() && read_float(gyro_dir_ + "/in_anglvel_x_raw", g[0]) && read_float(gyro_dir_ + "/in_anglvel_y_raw", g[1]) &&
        read_float(gyro_dir_ + "/in_anglvel_z_raw", g[2]))
      gz_ = (m_[6] * g[0] + m_[7] * g[1] + m_[8] * g[2]) * gyro_scale_;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

slot2::Motion MotionSource::read(float sx, float sy) const {
  if (kind_ == Kind::Sensor) { slot2::Motion m; m.ax = ax_; m.ay = ay_; m.az = az_; m.gz = gz_; return m; }
  if (kind_ == Kind::Stick) return from_stick(sx, sy);
  return slot2::Motion{};
}

} // namespace ds::sdl
