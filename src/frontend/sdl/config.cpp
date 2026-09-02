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
R"(# DSperate settings. Command-line flags override this file, and two files
# next to it override it for one game: games/<rom name>.ini (the ROM's
# filename without .nds) and games/<GAMECODE>.ini (the four letters printed
# as "game: ... [XXXX]" at start), the filename one winning. A layout picked
# with the hotkey is remembered in the filename one.
# Lines starting with # are comments; delete the # to activate a setting.

[paths]
# bios9 = /path/to/bios9.bin
# bios7 = /path/to/bios7.bin
# firmware = /path/to/firmware.bin
# firmware_override = /path/to/firmware.bin.ovr
#                               # where settings changed inside the firmware's own menu are kept
#                               # (nickname, birthday, favourite colour, message, language).
#                               # Only the changed 256-byte pages are stored, and the firmware
#                               # dump itself is never written to; delete this file to put the
#                               # console back to whatever the dump says. Default: <firmware>.ovr
# games = /path/to/games        # the library the loader cart's game picker lists (.nds and .zip,
#                               # one level, sorted by the ROM header's own title where it has one).
#                               # Only used on a firmware boot with a BootMenu.nds in the slot:
#                               # tapping the card in the DS menu raises this list, and picking a
#                               # game boots it. Unset, the picker says so rather than showing
#                               # an empty box.
# saves = /path/to/saves        # battery saves; default next to the ROM
# states = /path/to/states      # save states and screenshots; default next to the ROM
# cheats = /path/to/usrcheat.dat # Action Replay database; default: beside the ROM,
                                #   then in this config directory. Loading it only
                                #   lists the codes -- none are on until you say so.

[video]
# scale = 2                     # window scale
# fullscreen = false
# layout = vertical             # vertical | horizontal | single | pip | dominant_v | dominant_h
# layout_cycle = vertical,horizontal,single,pip,dominant_v,dominant_h   # what layout_next/prev step through
# screen = top                  # top | bottom: shown alone (single), large (pip) or dominant
# pip_corner = br               # tl | tr | bl | br: where the pip inset sits
# pip_scale = 0.33              # inset size relative to the large screen
# dominant_ratio = 0.5          # the smaller screen's size relative to the dominant one
# dual_window = false           # one window per panel (dual-screen handhelds)
# linear = false                # smooth scaling
# lcd_grid = 0                  # LCD pixel grid strength, 0 (off) .. 1 (software scaling only)
# seam = dark                   # dark: the LCD grid, dimmed by lcd_grid | blend: box-filter seams (sharp-shimmerless;
                                # the one panel pixel/row straddling two DS pixels is their area-weighted blend, the
                                # rest crisp) | blend_linear: the same blended in linear light
# chunky = false                # a cell of chunky_cell panel pixels per group of DS pixels (one seam per cell):
                                # mean (true; the area-weighted box) | extreme (the mean unless the darkest or
                                # brightest pixel stands more than chunky_threshold from it) | mode (dominant
                                # colour, mean when all differ) | tl (top-left pixel) | min | max | false
# chunky_threshold = 180        # extreme only: how far (luma, 0..255) an outlier must stand from the mean to win
# chunky_cell = auto            # panel pixels per cell: auto (smallest of 4..16 dividing the screen; 4 on 640x480 =
                                # 160x120 cells, an area-weighted box of 1.6 DS pixels each) | pair (2x2 DS pixels) | N
# aa = false                    # 3D anti-aliasing (DISP3DCNT bit 4). Opt-in: off, the rasteriser draws
                                # every frame as if the game had it off (no edge coverage, no pixel stack),
                                # which is cheaper and looks like most emulators; true = hardware behaviour
# accel = false                 # GPU renderer
# vsync = true
# fps = false                   # show a frames-per-second counter in the top-right corner of the primary
#                               # screen -- the one shown alone, large or dominant, so it follows `screen`
#                               # and the screen_swap hotkey (top screen when dual_window is on, where both
#                               # panels are shown), moving to the bottom-right when a PiP inset would
#                               # cover it. Counts presented frames, so frameskip and fast forward show
#                               # in it. The `fps` hotkey toggles it; it is unbound by default.

[audio]
# enabled = true
# volume = 100                  # 0..100
# mic = true                    # open the microphone
# mic_dev = plughw:0,0          # ALSA capture device
# mic_gain = 0.25
# mic_gate = 5

[emu]
# jit = true                    # false = interpreter
# quantum = 0                   # 0 = event-bound; 128 = melonDS lockstep
# timing_oc = false             # "Timing OC": no GX FIFO and untimed geometry, DraStic's model. Faster
                                # (Golden Sun -4%), less accurate: games that pace on the FIFO or the swap
                                # wait see different timing (Dragon Ball Origins' intro desyncs). The old
                                # DMA half is gone: untimed DMA measured worse everywhere.
# cpu_oc = false                # "CPU OC": the recompiler prices every data access as main RAM at
                                # translate time instead of looking the region's cost up per access.
                                # Less accurate than timing_oc (timer-race titles drift); measure first.
# fast_load = false             # a cart DMA takes ROM words as fast as it reads them instead of on the
                                # card's clock, with one event per transfer for the done IRQ: fewer
                                # scheduler slices on loading screens and streaming. May introduce
                                # accuracy issues: the DMA's bus stall lands all at once, so games that
                                # race the card (timing loops, mid-transfer polling) can behave differently.
# idle_skip = 1                 # 0 | 1 | all, see README
# autosave = false              # on quit, write a save state to the unlisted "auto" slot
#                               # (<states>/<GAMECODE>.auto.dss). Nothing is written while playing, so
#                               # it costs no frame time; a Ctrl-C or a launcher's SIGTERM is covered,
#                               # a SIGKILL is not. Resume it with --load-state <that path>; it never
#                               # appears in the pause menu's slots and slot_next/prev never reach it.
#                               # Skipped during a replay or a recording, like the save-state hotkey.
# fast_forward = false          # start fast-forwarding (the hotkeys toggle it)
# ff_speed = 0                  # fast-forward cap as a multiple of real time; 0 = unlimited
# ff_skip = 3                   # while fast-forwarding, present one frame in ff_skip+1
# frameskip = 0                 # skip drawing up to N frames in a row (0 = off): a skipped frame runs
                                # the machine and the game exactly as usual, but neither engine draws
                                # its lines and the 3D is not rasterised, so it costs a fraction of a
                                # drawn one. A frame that display-captures is drawn whatever the
                                # setting, since the game reads those pixels back.
# frameskip_mode = adaptive     # adaptive: skip only while the emulator is running behind real time,
                                # up to `frameskip` frames in a row | fixed: always skip that many of
                                # every frameskip+1
# frameskip_capture = false     # INEXACT: skip frames that display-capture as well. The capture is
                                # skipped with the drawing, so its bank keeps the picture it last
                                # captured; a game that reads those pixels back with the CPU sees an
                                # older frame than the hardware would. Without this, a game that
                                # captures every frame (Pokemon B/W, Golden Sun) skips nothing at all.
                                # Skipping always runs in whole display periods (see the README): on a
                                # game that drives its screens on alternate frames `frameskip = 3`
                                # skips six frames of every eight, the same ratio as three of four,
                                # and only the last drawn frame of each block is presented.

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
#                               # bottom screen (left takes the stick away from the d-pad)
# stylus_dpad = none            # a button; while held the d-pad moves the pen (e.g. leftshoulder)
# stylus_button = rightstick    # touches at the pen's position
# stylus_speed = 4.0            # pen pixels per frame at full tilt
# stylus_size = 2               # crosshair scale: arm width and centre dot, pixels
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
