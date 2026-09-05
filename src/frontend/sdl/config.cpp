// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace ds::sdl {

namespace {

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// "section.key" -> {section, key}
void split_key(const std::string& key, std::string& sec, std::string& name) {
  const size_t dot = key.find('.');
  sec = dot == std::string::npos ? "" : key.substr(0, dot);
  name = dot == std::string::npos ? key : key.substr(dot + 1);
}

} // namespace

std::string Config::dir() {
  std::string base;
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) base = x;
  else if (const char* h = std::getenv("HOME"); h && *h) base = std::string(h) + "/.config";
  else base = ".";
  ::mkdir(base.c_str(), 0755);
  const std::string d = base + "/dsperate";
  ::mkdir(d.c_str(), 0755);
  ::mkdir((d + "/games").c_str(), 0755);
  return d;
}

std::string Config::game_path_code(const char code[4]) {
  std::string c;
  for (int i = 0; i < 4; ++i) c += (code[i] >= 0x20 && code[i] < 0x7f && code[i] != '/') ? code[i] : '_';
  return dir() + "/games/" + c + ".ini";
}

std::string Config::game_path_rom(const std::string& rom) {
  std::string base = rom.substr(rom.find_last_of('/') + 1);
  if (const size_t dot = base.find_last_of('.'); dot != std::string::npos && dot > 0) base.resize(dot);
  if (base.empty()) return "";
  return dir() + "/games/" + base + ".ini";
}

bool Config::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line, section;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') { section = trim(line.substr(1, line.size() - 2)); continue; }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    // Trailing comments; a bare `#` inside a value is not expected.
    if (const size_t c = v.find_first_of("#;"); c != std::string::npos) v = trim(v.substr(0, c));
    kv_[section.empty() ? k : section + "." + k] = v;
  }
  return true;
}

std::string Config::str(const std::string& key, const std::string& def) const {
  const auto it = kv_.find(key);
  return it == kv_.end() ? def : it->second;
}

int Config::num(const std::string& key, int def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end() || it->second.empty()) return def;
  return static_cast<int>(std::strtol(it->second.c_str(), nullptr, 0));
}

double Config::real(const std::string& key, double def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end() || it->second.empty()) return def;
  return std::atof(it->second.c_str());
}

bool Config::flag(const std::string& key, bool def) const {
  const auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  const std::string& v = it->second;
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return def;
}

bool Config::store(const std::string& path, const std::string& key, const std::string& value) {
  std::string sec, name;
  split_key(key, sec, name);
  std::vector<std::string> lines;
  {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
  }
  // Find the section, then the key within it; insert after the section's
  // last line if the key is missing; append the section if that is missing.
  size_t sec_start = std::string::npos, sec_end = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string t = trim(lines[i]);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
      if (sec_start != std::string::npos) { sec_end = i; break; }
      if (trim(t.substr(1, t.size() - 2)) == sec) sec_start = i;
    }
  }
  const std::string entry = name + " = " + value;
  if (sec_start == std::string::npos) {
    if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
    lines.push_back("[" + sec + "]");
    lines.push_back(entry);
  } else {
    bool done = false;
    for (size_t i = sec_start + 1; i < sec_end; ++i) {
      const std::string t = trim(lines[i]);
      if (t.empty() || t[0] == '#' || t[0] == ';') continue;
      const size_t eq = t.find('=');
      if (eq != std::string::npos && trim(t.substr(0, eq)) == name) { lines[i] = entry; done = true; break; }
    }
    if (!done) {
      size_t at = sec_end;   // before the blank lines that precede the next section
      while (at > sec_start + 1 && trim(lines[at - 1]).empty()) --at;
      lines.insert(lines.begin() + static_cast<long>(at), entry);
    }
  }
  std::ofstream f(path);
  if (!f) return false;
  for (const std::string& l : lines) f << l << '\n';
  return true;
}

void Config::write_default(const std::string& path, bool force) {
  if (!force && std::ifstream(path)) return;
  std::ofstream f(path);
  if (!f) return;
  f <<
R"(# DSperate settings. Command-line flags override this file. Two files next
# to it override it for one game: games/<rom name>.ini (the ROM's filename
# without .nds) and games/<GAMECODE>.ini (the four letters printed as
# "game: ... [XXXX]" at start); the filename one wins, and is where a layout
# picked with the hotkey is remembered.
# Every line is a comment showing the default; delete the # to change one.

[paths]
# bios9 = /path/to/bios9.bin
# bios7 = /path/to/bios7.bin
# firmware = /path/to/firmware.bin
# firmware_override = /path/to/firmware.bin.ovr
                                # settings changed inside the firmware's own menu (nickname, colour,
                                # language...) are kept here, never in the dump. Delete it to reset.
                                # Default: <firmware>.ovr
# games = /path/to/games        # the library the loader card lists on a firmware boot (.nds and .zip,
                                # one level). Unset, the picker says so.
# saves = /path/to/saves        # battery saves. Default: next to the ROM
# states = /path/to/states      # save states (and the autosave's PNG). Default: next to the ROM
# screenshots = /path/to/shots  # the screenshot hotkey. Default: the states directory
# cache = /path/to/cache        # where zipped games are unpacked when they cannot be unpacked beside
                                # the zip (a read-only card). Default: <zip dir>/.dsperate/. Kept
                                # between runs (see [cart]); never /tmp, which is RAM on a handheld
# cheats = /path/to/usrcheat.dat # Action Replay database. Default: beside the ROM, then in this
                                #   directory. Loading it lists the codes; none are on until enabled

[loader]
# The card the DS menu shows on a firmware boot. Built in; a BootMenu.nds next
# to this file replaces it (tools/mkcart.py makes one with your own icon).
# title = Game Menu             # first banner line
# subtitle = Dariragan! Dagozuban!   # second line; empty for one line
# card = true                   # false boots the firmware with the slot empty

[video]
# scale = 2                     # window size, in DS pixels
# fullscreen = false
# layout = vertical             # vertical | horizontal | single | pip | dominant_v | dominant_h
# layout_cycle = vertical,horizontal,single,pip,dominant_v,dominant_h   # what the layout hotkeys step through
# screen = top                  # top | bottom: the screen shown alone (single), large (pip) or dominant
# pip_corner = br               # tl | tr | bl | br: where the pip inset sits
# pip_scale = 0.33              # inset size relative to the large screen
# pip_alpha = 1.0               # inset opacity at rest, 0..1
# pip_touch_hold = 60           # frames the inset stays opaque after the bottom screen was touched; 0 = never fades
# dominant_ratio = 0.5          # the smaller screen's size relative to the dominant one
# dual_window = false           # one window per panel (dual-screen handhelds)
# linear = false                # bilinear filter instead of nearest (on the A30 display-engine tier: the
                                # driver's own scaler filter). Overrides lcd_grid, seam and chunky
# lcd_grid = 0                  # LCD pixel grid: one dark seam per DS pixel, strength 0 (off) .. 1 (black)
# seam = dark                   # dark: the grid above | blend: only the panel pixel straddling two DS
                                # pixels is blended ("sharp shimmerless") | blend_linear: the same in
                                # linear light
# chunky = false                # draw blocks of DS pixels as one flat cell, for panels at odd scales:
                                # mean (or true) | extreme | mode | tl | min | max | false
# chunky_threshold = 180        # extreme only: luma distance (0..255) an outlier needs to win over the mean
# chunky_cell = auto            # panel pixels per cell: auto (4..16, whatever divides the screen) | pair
                                # (2x2 DS pixels) | N (or the nearest size below N that divides the screen)
# aa = false                    # 3D edge anti-aliasing as the hardware does it. Off is cheaper and is
                                # what most emulators show
# disp = auto                   # present through the display engine's hardware scaler (Miyoo A30 and
#                               # other Allwinner boards): auto (wherever /dev/disp answers) | true | false
# fbdev = auto                  # present straight through /dev/fb0 (the H700 handhelds' mali-only SDL2):
#                               # auto (when SDL has no display but fb0 answers) | true | false
# vsync = true
# fps = false                   # frames-per-second counter in a corner of the primary screen. Counts
#                               # presented frames, so frameskip and fast forward show. The fps hotkey
#                               # toggles it (unbound by default)

[audio]
# enabled = true
# volume = 100                  # 0..100
# driver = pipewire             # SDL audio backend tried first (pipewire, alsa, pulseaudio...); empty =
#                               # SDL's choice; SDL_AUDIODRIVER overrides
# native_rate = true            # open the device at its own rate and resample here (false: 32768 Hz,
#                               # the sound daemon resamples)
# mic = true                    # open the microphone
# mic_dev = plughw:0,0          # ALSA capture device
# mic_gain = 0.25
# mic_gate = 5

[cart]
# cache_mb = 2048               # unpacked zipped games kept per directory, in MB; the least recently
#                               # launched go first when a new one would not fit. 0 = keep all
# cache = keep                  # keep: unpacked games stay for the next launch | session: deleted at
#                               # exit, for a card with no room to spare

[emu]
# realtime = rr                 # rr | fifo | off: real-time scheduling for the emulator's threads
#                               # (needs root or an rtprio limit)
# rt_priority = 5
# jit = true                    # false = interpreter (much slower; for comparison)
# quantum = 0                   # CPU interleave: 0 = event-bound (fastest) | 128 = melonDS lockstep
#
# cpu_oc, timing_oc and fast_load trade accuracy for speed. They are off by
# default and no game needs them; they exist to squeeze a slow device. If a
# game misbehaves (hangs, desyncs, glitches), turn these off first.
# cpu_oc = false                # "CPU OC": the recompiler prices every memory access as main RAM
                                # instead of by region. Less accurate than timing_oc: timer-race
                                # titles drift. Usually a sizable FPS increase.
# timing_oc = false             # "Timing OC": drop the GX FIFO and geometry timing (DraStic's model).
                                # A few percent faster on 3D-heavy games; USE WITH CAUTION! games that
                                # pace on the FIFO or the swap WILL break. HARD. Turn THIS off first if
                                # something breaks.
# fast_load = false             # cart DMA reads ROM at full speed instead of on the card's clock.
                                # Faster loading screens; games that race the card can misbehave
# idle_skip = 1                 # skip a CPU busy-wait: 0 = never | 1 = only the GXSTAT swap poll |
                                # all = every proven poll loop
# autosave = false              # on quit, save a state to the hidden "auto" slot
                                # (<states>/<GAMECODE>.auto.dss). Covers Ctrl-C and a launcher's
                                # SIGTERM, not SIGKILL. Resume with --load-state <that path>; the slot
                                # never shows in the menu. Skipped during a replay or recording
# autosave_png = false          # a picture with the auto state, laid out as the window shows it:
                                # true = <states>/<GAMECODE>.auto.png | a path = that file (for a
                                # launcher's game switcher). Taken from the emulator's own frame, so
                                # it works on panels a screen grabber cannot read
# fast_forward = false          # start fast-forwarding (the hotkeys toggle it)
# ff_speed = 0                  # fast-forward cap as a multiple of real time; 0 = unlimited
# ff_skip = 3                   # while fast-forwarding, present one frame in ff_skip+1
# frameskip = 0                 # skip drawing up to N frames in a row (0 = off). The game runs
                                # exactly as usual, only the drawing is skipped. A frame the game
                                # display-captures is always drawn: it reads those pixels back
# frameskip_mode = adaptive     # adaptive: skip only while behind real time, up to `frameskip` |
                                # fixed: always skip that many of every frameskip+1
# frameskip_capture = false     # INEXACT: skip display-capture frames too. The game then reads back
                                # an older frame than the hardware would. Without this, a game that
                                # captures every frame (Pokemon B/W, Golden Sun) never skips. Skipping
                                # runs in whole display periods: on a game that draws its screens on
                                # alternate frames, frameskip = 3 skips six of every eight

# DS buttons: a b x y l r start select up down left right.
# Keyboard values are SDL key names ("x", "Return", "Right Shift", "F5").
[keys]
# a = x
# b = z
# x = s
# y = a
# l = q
# r = w
# start = Return
# select = Right Shift
# up = Up
# down = Down
# left = Left
# right = Right

# Controller values are SDL controller button names (a b x y back guide
# start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft
# dpright) or axes (+leftx -lefty +righttrigger ...). SDL names buttons by
# position, so the DS's A is the pad's "b".
[pad]
# a = b
# b = a
# x = y
# y = x
# l = leftshoulder
# r = rightshoulder
# start = start
# select = back
# up = dpup
# down = dpdown
# left = dpleft
# right = dpright
# stick_dpad = true             # left stick also works the d-pad
# stick_deadzone = 12000
# stylus_axis = right           # right | left | none: the stick that moves the pen over the
                                # bottom screen (left takes the stick away from the d-pad)
# stylus_dpad = none            # a button; while held the d-pad moves the pen (e.g. leftshoulder)
# stylus_button = rightstick    # touches at the pen's position; mod+<it> is free for a hotkey
# stylus_speed = 4.0            # pen pixels per frame at full tilt
# stylus_size = 2               # crosshair scale: arm width and centre dot, DS pixels (scaled up by the
                                # view's reduction when the bottom screen is the PiP inset / dominant secondary)
# stylus_hide = 90              # frames idle before the crosshair hides (0 = never shown)

# Hotkeys: quit pause fast_forward (held) fast_forward_toggle save_state
# ("pause" opens the pause menu: it stops the machine and draws save state,
# load state and the slot list over the held frame, driven by the DS buttons)
# load_state slot_next slot_prev volume_up volume_down mute layout_next
# layout_prev screen_swap pip_corner_next fullscreen screenshot lid mic (held) fps. A value is a key name, or
# "mod+name" meaning the modifier must be held with it; "none" unbinds.
[hotkeys]
# modifier = none               # keyboard modifier, e.g. "Left Ctrl"
# quit = Escape
# pause = p
# fast_forward = Tab
# fast_forward_toggle = none
# save_state = F5
# load_state = F7
# slot_next = F3
# slot_prev = F2
# volume_up = =
# volume_down = -
# mute = 0
# layout_next = F4
# layout_prev = F10
# screen_swap = F6              # which screen is alone / large / dominant (or first)
# pip_corner_next = F8
# fullscreen = f
# screenshot = F9
# lid = l
# mic = m
# fps = none                    # the on-screen frames-per-second counter

# The same on the controller. The modifier is the pad's mode/home button
# ("guide"); if it doubles as a DS button it is withheld from the game
# while held and delivered as a tap when released alone. A chord
# "mod+start+back" needs both buttons (SDL calls the DS Select "back").
[padhotkeys]
# modifier = guide
# quit = mod+start+back
# pause = mod+start
# fast_forward = mod++righttrigger
# save_state = mod+rightshoulder
# load_state = mod+leftshoulder
# slot_next = mod+dpright
# slot_prev = mod+dpleft
# volume_up = none
# volume_down = none
# layout_next = mod+back
# layout_prev = mod+x
# screen_swap = mod+y
# pip_corner_next = none
# screenshot = none
# mic = leftstick
# fps = none
)";
}

} // namespace ds::sdl
