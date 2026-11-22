// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/settings.h"
#include "frontend/i18n/i18n.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::sdl {
namespace {

using T = Setting::Type;

const Choice kOnOff[]   = {{"false", N_("OFF")}, {"true", N_("ON")}};
const Choice kSkipMode[] = {{"adaptive", N_("ADAPTIVE")}, {"fixed", N_("FIXED")}};
// Panel-typical rates, plus the console's own (auto, the default) and no limit.
const Choice kLimiter[] = {{"auto", N_("CONSOLE (59.8)")}, {"30", "30"}, {"60", "60"}, {"120", "120"},
                           {"144", "144"}, {"240", "240"}, {"off", N_("UNLIMITED")}};
const Choice kTiming[]   = {{"fast", N_("FAST")}, {"exact", N_("EXACT")}, {"turbo", N_("TURBO")}};
const Choice kIntScale[] = {{"off", N_("OFF")}, {"under", N_("UNDER")}, {"over", N_("OVER")}};
const Choice kSeam[]     = {{"dark", N_("DARK")}, {"blend", N_("BLEND")}, {"blend_linear", N_("BLEND LINEAR")}};
const Choice kChunky[]   = {{"false", N_("OFF")}, {"mean", N_("DEFAULT")}, {"extreme", N_("ADAPTIVE")},
                            {"mode", N_("COMMON")}, {"tl", N_("FIRST")}, {"min", N_("DARKEST")}, {"max", N_("LIGHTEST")}};
const Choice kColourProfile[] = {{"off", N_("OFF")}, {"nds", N_("DS")}, {"nds_lite", N_("DS LITE")}};
const Choice kGpuScale[] = {{"1", "1X"}, {"2", "2X"}, {"4", "4X"}};
const Choice kScreen[]   = {{"top", N_("TOP")}, {"bottom", N_("BOTTOM")}};
// GBATEK's order, matching the firmware.
const Choice kColour[]   = {{"0", N_("GREY")}, {"1", N_("BROWN")}, {"2", N_("RED")}, {"3", N_("PINK")},
                            {"4", N_("ORANGE")}, {"5", N_("YELLOW")}, {"6", N_("LIME")}, {"7", N_("GREEN")},
                            {"8", N_("DARK GREEN")}, {"9", N_("TURQUOISE")}, {"10", N_("BLUE")}, {"11", N_("DARK BLUE")},
                            {"12", N_("PURPLE")}, {"13", N_("VIOLET")}, {"14", N_("MAGENTA")}, {"15", N_("DARK PINK")}};
const Choice kMonth[]    = {{"1", N_("JANUARY")}, {"2", N_("FEBRUARY")}, {"3", N_("MARCH")}, {"4", N_("APRIL")},
                            {"5", N_("MAY")}, {"6", N_("JUNE")}, {"7", N_("JULY")}, {"8", N_("AUGUST")},
                            {"9", N_("SEPTEMBER")}, {"10", N_("OCTOBER")}, {"11", N_("NOVEMBER")}, {"12", N_("DECEMBER")}};
// AUTO is --netplay (join if heard, else host); HOST/GUEST fix the role up
// front. Join address stays a command-line flag; discovery finds hosts itself.
const Choice kNetMode[]  = {{"off", N_("OFF")}, {"auto", N_("AUTO")}, {"host", N_("HOST")}, {"guest", N_("GUEST")},
                            {"internet", N_("INTERNET")}};
// A plain-address DNS form also exists in the config but has nowhere in a
// pick row. WIIMMFI is default since Nintendo WFC is gone; HOST resolves to
// nothing unless pointed at a private server.
const Choice kWifiDns[]  = {{"wiimmfi", "WIIMMFI"}, {"host", NC_("wifi.dns", "HOST")}};
const Choice kLanguage[] = {{"0", N_("JAPANESE")}, {"1", N_("ENGLISH")}, {"2", N_("FRENCH")},
                            {"3", N_("GERMAN")}, {"4", N_("ITALIAN")}, {"5", N_("SPANISH")}};
const Choice kCheck[]    = {{"false", "[ ]"}, {"true", "[X]"}};
const Choice kRumble[]   = {{"auto", N_("AUTO")}, {"on", N_("ON")}, {"off", N_("OFF")}};
const Choice kMotion[]   = {{"auto", N_("AUTO")}, {"retail", N_("MOTION PACK")}, {"homebrew", N_("MOTION CARD")}, {"off", N_("OFF")}};
const Choice kMotionSrc[] = {{"auto", N_("AUTO")}, {"pad", N_("CONTROLLER")}, {"sensor", N_("SENSOR")}, {"stick", N_("STICK")}};
const Choice kMotionStick[] = {{"right", N_("RIGHT")}, {"left", N_("LEFT")}};
const Choice kAnalog[]   = {{"auto", N_("AUTO")}, {"off", N_("OFF")}, {"left", N_("LEFT STICK")}, {"right", N_("RIGHT STICK")}, {"dual", N_("TWO STICKS")}};
const Choice kAnalogLayout[] = {{"auto", N_("AUTO")}, {"packed", N_("PACKED")}, {"extended", N_("EXTENDED")}};
const Choice kLayout[]   = {{"vertical", N_("VERTICAL")}, {"horizontal", N_("HORIZONTAL")}, {"single", N_("SINGLE")},
                            // TRANSLATORS: PIP is picture in picture (the small screen over the large one); DOMINANT V and H, a large screen with a small one under or beside it. Match the CYCLE rows and PIP rows.
                            {"pip", N_("PIP")}, {"dominant_v", N_("DOMINANT V")}, {"dominant_h", N_("DOMINANT H")}};
// Filled by set_ui_languages; a slot with an empty value is never offered.
// Every slot starts empty rather than null, so nothing can read a null value.
std::array<Choice, kMaxUiLanguages> kUiLanguages = [] {
  std::array<Choice, kMaxUiLanguages> a{};
  a.fill(Choice{"", ""});
  a[0] = Choice{"auto", N_("AUTO")};
  return a;
}();
// Filled by set_camera_images, as the languages are.
std::array<Choice, kMaxCameraImages> kCameraImages = [] {
  std::array<Choice, kMaxCameraImages> a{};
  a.fill(Choice{"", ""});
  a[0] = Choice{"", N_("TEST PATTERN")};
  return a;
}();
const Choice kCorner[]   = {{"tl", N_("TOP LEFT")}, {"tr", N_("TOP RIGHT")}, {"bl", N_("BOTTOM LEFT")}, {"br", N_("BOTTOM RIGHT")}};

constexpr Setting boolean(const char* k, const char* l, const char* def, u8 f, Dep d, const char* n) {
  return Setting{k, l, T::Bool, kOnOff, 2, 0, 0, 0, nullptr, nullptr, nullptr, def, f, d, n};
}
constexpr Setting check(const char* k, const char* l, const char* def, u8 f, Dep d, const char* n) {
  return Setting{k, l, T::Bool, kCheck, 2, 0, 0, 0, nullptr, nullptr, nullptr, def, f, d, n};
}
constexpr Setting pick(const char* k, const char* l, const Choice* c, u8 nc, const char* def, u8 f, Dep d, const char* n) {
  return Setting{k, l, T::Pick, c, nc, 0, 0, 0, nullptr, nullptr, nullptr, def, f, d, n};
}
constexpr Setting number(const char* k, const char* l, int lo, int hi, int st, const char* def, u8 f, Dep d, const char* n,
                         const char* sv = nullptr, const char* sl = nullptr, const char* sx = nullptr) {
  return Setting{k, l, T::Int, nullptr, 0, lo, hi, st, sv, sl, sx, def, f, d, n};
}
constexpr Setting percent(const char* k, const char* l, int lo, int hi, int st, const char* def, u8 f, Dep d, const char* n,
                          const char* sv = nullptr, const char* sl = nullptr) {
  return Setting{k, l, T::Percent, nullptr, 0, lo, hi, st, sv, sl, nullptr, def, f, d, n};
}
constexpr Setting text(const char* k, const char* l, int maxlen, const char* def, u8 f, const char* n) {
  return Setting{k, l, T::Text, nullptr, 0, maxlen, maxlen, 0, nullptr, nullptr, nullptr, def, f, Dep::None, n};
}
constexpr Setting end() { return Setting{nullptr, nullptr, T::Bool, nullptr, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, Dep::None, nullptr}; }

} // namespace

const Setting kEmuSettings[] = {
  // Always the global file (the host refuses a per-game copy): the language
  // is set before a game's own file is read.
  pick("ui.language", N_("MENU LANGUAGE"), kUiLanguages.data(), kMaxUiLanguages, "auto", FlagLive | FlagVerbatim, Dep::None,
       N_("THE LANGUAGE OF THESE MENUS. AUTO FOLLOWS THE SYSTEM'S")),
  number("emu.frameskip", N_("FRAMESKIP"), 0, 3, 1, "0", FlagLive, Dep::NetSession,
         N_("DRAW FEWER FRAMES. THE GAME STILL RUNS IN FULL")),
  pick("emu.frameskip_mode", N_("FRAMESKIP MODE"), kSkipMode, 2, "adaptive", FlagLive, Dep::FrameskipMode,
       N_("ADAPTIVE SKIPS ONLY WHILE BEHIND REAL TIME")),
  pick("emu.limiter", N_("FRAME LIMITER"), kLimiter, 7, "auto", FlagLive, Dep::NetSession,
       N_("THE RATE THE GAME IS HELD TO. CONSOLE IS THE ONE THE GAME WAS WRITTEN FOR")),
  // Whole percent in the file ("100"), so not a Percent row (0..1 fraction).
  number("emu.speed", N_("GAME SPEED"), 25, 400, 5, "100", FlagLive, Dep::NetSession,
         N_("HOW FAST THE GAME RUNS AGAINST THE LIMITER"), nullptr, nullptr, "%"),
  // Multiple of real time (a floor), not of the limiter, so it never slows
  // the game below this even on a fast limiter.
  number("emu.ff_speed", N_("FAST FORWARD SPEED"), 2, 16, 1, "0", FlagLive, Dep::NetSession,
         N_("AT LEAST THIS MANY TIMES REAL TIME WHILE FAST FORWARD IS HELD"), "0", N_("UNLIMITED"), "X"),
  number("emu.ff_skip", N_("FAST FORWARD SKIP"), 0, 9, 1, "3", FlagLive, Dep::NetSession,
         N_("WHILE FAST FORWARDING, SHOW ONE FRAME IN THIS MANY PLUS ONE")),
  // AUTO raises the buffer only when the machine keeps up yet still ran dry
  // (a hitch, not a machine too slow to keep up at all).
  number("audio.buffer_size", N_("AUDIO BUFFER"), 20, 200, 10, "auto", FlagLive, Dep::None,
         N_("SOUND HELD AHEAD. LOWER IS LESS DELAY, LESS SLACK BEFORE A LATE FRAME IS HEARD"),
         "auto", N_("AUTO"), " MS"),
  // Turbo is not the console's speed: the host warns once a session when it is picked here (main.cpp).
  pick("emu.timing", N_("CPU TIMING"), kTiming, 3, "fast", FlagRestart, Dep::None,
       N_("EXACT IS THE CONSOLE'S, AND SLOWER TO RUN. TURBO GIVES GAMES MORE CPU: SMOOTHER, MAY BREAK SOME")),
  boolean("emu.autosave", N_("AUTOSAVE ON QUIT"), "false", FlagLive, Dep::None,
          N_("SAVE A STATE WHEN THE EMULATOR EXITS, TO RESUME FROM")),
  boolean("emu.autoload", N_("AUTOLOAD ON START"), "false", FlagRestart, Dep::None,
          N_("WHEN A GAME STARTS, RESUME FROM ITS AUTOSAVED STATE IF THERE IS ONE")),
  boolean("emu.dsi_enhanced", N_("DSI ENHANCED GAMES"), "false", FlagRestart, Dep::DsiFirmware,
          N_("RUN DSI-ENHANCED GAME CARDS AS A DSI. NEEDS THE DSI BIOS, AND RUNS HEAVIER")),
  boolean("emu.dsi_hide_installed", N_("HIDE NAND DSIWARE"), "false", FlagRestart, Dep::None,
          N_("DSI MENU: HIDE THE NAND DUMP'S OWN TITLES. THE DUMP IS NOT CHANGED")),
  boolean("emu.dsi_nand_shortcuts", N_("DSI NAND LINKS"), "false", FlagLive, Dep::ShortcutsPath,
          N_("GAME LIST ENTRIES THAT START THE NAND'S DSIWARE WITHOUT THE DSI MENU")),
  // Always this game's own file: "don't ask again" wrote it there.
  boolean("slot2.ask", N_("ASK FOR GBA GAME"), "true", FlagRestart, Dep::Slot2Link,
          N_("BEFORE THIS GAME STARTS, OFFER THE GBA GAMES IT READS. KEPT FOR THIS GAME")),
  // TRANSLATORS: Nintendo's slot-2 accessory: its official name in your language where it has one.
  pick("slot2.rumble", N_("RUMBLE PAK"), kRumble, 3, "auto", FlagRestart, Dep::None,
       N_("IN SLOT 2 WHEN NO GBA GAME IS. AUTO: WHEN THE CONTROLLER CAN RUMBLE")),
  number("slot2.rumble_strength", N_("RUMBLE STRENGTH"), 10, 100, 10, "100", FlagLive, Dep::RumblePak,
         N_("HOW HARD THE MOTOR RUNS WHEN THE GAME RUMBLES"), nullptr, nullptr, "%"),
  // TRANSLATORS: the slot-2 motion accessory (Tony Hawk's Motion came with one).
  pick("slot2.motion", N_("MOTION PACK"), kMotion, 4, "auto", FlagRestart, Dep::None,
       N_("A TILT SENSOR IN SLOT 2. AUTO: FOR THE GAME THAT NEEDS IT")),
  pick("slot2.motion_source", N_("TILT FROM"), kMotionSrc, 4, "auto", FlagRestart, Dep::None,
       N_("THE CONTROLLER'S OR HANDHELD'S OWN SENSOR WHERE THERE IS ONE, ELSE A STICK")),
  pick("slot2.motion_stick", N_("TILT STICK"), kMotionStick, 2, "right", FlagLive, Dep::None,
       N_("THE STICK THAT TILTS WHEN THERE IS NO SENSOR. THE PEN LETS GO OF IT MEANWHILE")),
  number("slot2.solar", N_("SOLAR SENSOR"), 0, 10, 1, "5", FlagLive, Dep::SolarSensor,
         N_("THE SUNLIGHT A BOKTAI CARTRIDGE SEES: 0 DARK, 10 FULL SUN")),
  pick("net.mode", N_("NETWORK FEATURES"), kNetMode, 5, "off", FlagLive, Dep::Net,
       N_("LOCAL WIRELESS, OR INTERNET. AUTO JOINS A SESSION, ELSE HOSTS ONE")),
  pick("wifi.dns", "DNS", kWifiDns, 2, "wiimmfi", FlagRestart, Dep::NetInternet,
       N_("WHERE THE GAME LOOKS UP ITS SERVERS. NINTENDO'S ARE GONE; WIIMMFI REPLACES THEM")),
  end(),
};

const Setting kAnalogSettings[] = {
  pick("pad.analog", N_("ANALOG PATCH"), kAnalog, 5, "auto", FlagLive, Dep::None,
       N_("NEEDS A CPAD CHEAT ON. AUTO USES CONTROLS AS AVAILABLE")),
  number("pad.analog_deadzone", N_("ANALOG DEADZONE"), 0, 30000, 500, "6000", FlagLive, Dep::None,
         N_("HOW FAR A STICK MUST MOVE BEFORE AN ANALOG PATCH SEES IT")),
  number("pad.analog_sensitivity_left", N_("LEFT SENSITIVITY"), 10, 300, 10, "100", FlagLive, Dep::None,
         N_("UNDER 100 CAPS THE TOP SPEED; OVER 100 GETS THERE WITH LESS TRAVEL"),
         nullptr, nullptr, "%"),
  number("pad.analog_sensitivity_right", N_("RIGHT SENSITIVITY"), 10, 300, 10, "100", FlagLive, Dep::None,
         N_("UNDER 100 CAPS THE TOP SPEED; OVER 100 GETS THERE WITH LESS TRAVEL"),
         nullptr, nullptr, "%"),
  boolean("pad.analog_invert_y", N_("ANALOG INVERT Y"), "false", FlagLive, Dep::None,
          N_("FLIP THE STICKS' UP AND DOWN FOR AN ANALOG PATCH")),
  pick("pad.analog_layout", N_("ANALOG LAYOUT"), kAnalogLayout, 3, "auto", FlagLive, Dep::None,
       N_("HOW THE PATCH WANTS STICK DATA ARRANGED. AUTO READS THE PATCH")),
  end(),
};

const Setting kVideoSettings[] = {
  pick("video.integer_scale", N_("INTEGER SCALE"), kIntScale, 3, "off", FlagDeferred, Dep::None,
       N_("WHOLE PANEL PIXELS PER DS PIXEL. UNDER LETTERBOXES, OVER CROPS")),
  boolean("video.linear", N_("BILINEAR"), "false", FlagDeferred, Dep::PanelEffects,
          N_("SMOOTH SCALING. OVERRIDES THE GRID, SEAMS AND CHUNKY")),
  percent("video.lcd_grid", N_("LCD GRID"), 0, 100, 10, "0", FlagDeferred, Dep::GridSeam,
          N_("A DARK SEAM AROUND EVERY DS PIXEL, LIKE THE ORIGINAL SCREEN")),
  pick("video.seam", N_("SEAM"), kSeam, 3, "dark", FlagDeferred, Dep::GridSeam,
       N_("DARK DRAWS THE GRID. BLEND SOFTENS ONLY THE STRADDLING PIXEL")),
  pick("video.chunky", N_("CHUNKY"), kChunky, 7, "false", FlagDeferred, Dep::Chunky,
       N_("DRAW BLOCKS OF DS PIXELS AS ONE FLAT CELL, FOR PANELS AT ODD SCALES")),
  number("video.chunky_cell", N_("CHUNKY CELL"), 2, 8, 1, "auto", FlagDeferred, Dep::ChunkyCell,
         N_("PANEL PIXELS PER CELL"), "auto", N_("AUTO")),
  pick("video.colour", N_("COLOUR"), kColourProfile, 3, "off", FlagLive, Dep::None,
       N_("ANOTHER SCREEN'S COLOURS: THE ORIGINAL DS, OR THE BRIGHTER DS LITE")),
  boolean("video.aa", N_("ANTI-ALIASING"), "false", FlagLive, Dep::None,
          N_("DS ACCURATE IN CPU RENDER, 4X MSAA ON GPU (VULKAN ONLY)")),
  boolean("video.gpu3d", "GPU 3D", "false", FlagLive, Dep::None,
          N_("DRAW THE 3D LAYER ON THE GPU WHEN POSSIBLE, OTHERWISE CPU IS USED")),
  pick("video.gpu3d_scale", N_("GPU 3D SCALE"), kGpuScale, 3, "1", FlagLive, Dep::Gpu3dScale,
       N_("THE MOST THE GPU DRAWS 3D AT, TIMES THE DS'S (VULKAN ONLY). A SMALLER VIEW TAKES LESS. 2D STAYS AS IT IS")),
  boolean("video.fps", N_("FPS COUNTER"), "false", FlagLive, Dep::None,
          N_("FRAMES PER SECOND IN THE CORNER OF THE SCREEN")),
  boolean("video.fullscreen", N_("FULLSCREEN"), "false", FlagDeferred, Dep::Windowed, nullptr),
  end(),
};

const Setting kLayoutSettings[] = {
  pick("video.layout", N_("LAYOUT"), kLayout, 6, "vertical", FlagDeferred, Dep::OneWindow,
       N_("HOW THE TWO SCREENS SHARE THE WINDOW")),
  pick("video.screen", N_("MAIN SCREEN"), kScreen, 2, "top", FlagDeferred, Dep::None,
       N_("THE SCREEN SHOWN ALONE, LARGE OR FIRST")),
  number("video.screen_gap", N_("SCREEN GAP"), 0, 128, 4, "0", FlagDeferred, Dep::OneWindow,
         N_("PANEL PIXELS BETWEEN THE TWO SCREENS WHEN THEY ARE STACKED OR SIDE BY SIDE. AUTO PUSHES THEM TO THE EDGES"), "auto", N_("AUTO")),
  pick("video.pip_corner", N_("PIP CORNER"), kCorner, 4, "br", FlagDeferred, Dep::Pip,
       N_("WHERE THE SMALL SCREEN SITS")),
  percent("video.pip_scale", N_("PIP SIZE"), 10, 90, 5, "0.33", FlagDeferred, Dep::Pip,
          N_("HOW BIG THE SMALL SCREEN IS AGAINST THE LARGE ONE")),
  percent("video.pip_alpha", N_("PIP OPACITY"), 0, 100, 10, "1", FlagDeferred, Dep::Pip,
          N_("HOW SOLID THE SMALL SCREEN IS AT REST")),
  number("video.pip_touch_hold", N_("PIP TOUCH HOLD"), 10, 300, 10, "60", FlagLive, Dep::PipTouchHold,
         N_("FRAMES THE SMALL SCREEN STAYS SOLID AFTER IT IS TOUCHED"), "0", N_("NEVER FADE")),
  percent("video.dominant_ratio", N_("DOMINANT RATIO"), 10, 90, 5, "auto", FlagDeferred, Dep::Dominant,
          N_("THE SMALLER SCREEN'S SIZE. AUTO FITS WHOLE PIXELS"), "auto", N_("AUTO")),
  percent("video.dominant_threshold", N_("DOMINANT THRESHOLD"), 10, 99, 5, "0.25", FlagDeferred, Dep::DominantThreshold,
          N_("THE SMALLEST SECONDARY AUTO WILL ACCEPT")),
  check("video.layout_cycle.vertical", N_("CYCLE VERTICAL"), "true", FlagLive, Dep::OneWindow,
        N_("THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS")),
  check("video.layout_cycle.horizontal", N_("CYCLE HORIZONTAL"), "true", FlagLive, Dep::OneWindow,
        N_("THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS")),
  check("video.layout_cycle.single", N_("CYCLE SINGLE"), "true", FlagLive, Dep::OneWindow,
        N_("THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS")),
  check("video.layout_cycle.pip", N_("CYCLE PIP"), "true", FlagLive, Dep::OneWindow,
        N_("THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS")),
  check("video.layout_cycle.dominant_v", N_("CYCLE DOMINANT V"), "true", FlagLive, Dep::OneWindow,
        N_("THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS")),
  check("video.layout_cycle.dominant_h", N_("CYCLE DOMINANT H"), "true", FlagLive, Dep::OneWindow,
        N_("THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS. ONE ALWAYS STAYS")),
  end(),
};

// [user]: what a game sees as the console's owner. Restart-only: baked into
// generated firmware at boot; unused with a real dump.
const Setting kUserSettings[] = {
  text("user.nickname", N_("NICKNAME"), 10, "DSperate", FlagRestart,
       N_("WHAT GAMES CALL YOU")),
  text("user.message", N_("MESSAGE"), 26, "", FlagRestart,
       N_("THE GREETING THE DS MENU SHOWS")),
  pick("user.colour", N_("FAVOURITE COLOUR"), kColour, 16, "0", FlagRestart, Dep::None,
       N_("SOME GAMES COLOUR THEMSELVES WITH IT")),
  pick("user.birthday_month", N_("BIRTHDAY MONTH"), kMonth, 12, "1", FlagRestart, Dep::None,
       N_("GAMES THAT WISH YOU A HAPPY BIRTHDAY USE THIS")),
  number("user.birthday_day", N_("BIRTHDAY DAY"), 1, 31, 1, "1", FlagRestart, Dep::None,
         N_("NOT CHECKED AGAINST THE MONTH, AS THE CONSOLE DOES NOT EITHER")),
  pick("user.language", N_("LANGUAGE"), kLanguage, 6, "1", FlagRestart, Dep::None,
       N_("THE LANGUAGE MULTI-LANGUAGE GAMES START IN")),
  pick("emu.dsi_camera_image", N_("DSI CAMERA PICTURE"), kCameraImages.data(), kMaxCameraImages, "", FlagLive | FlagVerbatim, Dep::DsiFirmware,
       N_("WHAT THE DSI'S CAMERAS SEE: A PICTURE FROM PATHS.DSI_PHOTOS, OR COLOUR BARS")),
  end(),
};

void set_camera_images(const std::vector<std::string>& names) {
  static std::vector<std::string> kept;
  kept = names;
  for (int i = 1; i < kMaxCameraImages; ++i) kCameraImages[i] = Choice{"", ""};
  int n = 1;
  for (const std::string& name : kept) {
    if (n == kMaxCameraImages) break;
    kCameraImages[n++] = Choice{name.c_str(), name.c_str()};
  }
}

void set_ui_languages(const std::vector<UiLanguage>& languages) {
  // Owned here so the table's pointers stay valid; set once, at startup.
  static std::vector<UiLanguage> kept;
  kept = languages;
  for (int i = 1; i < kMaxUiLanguages; ++i) kUiLanguages[i] = Choice{"", ""};
  int n = 1;
  for (const UiLanguage& l : kept) {
    if (n == kMaxUiLanguages) break;
    kUiLanguages[n++] = Choice{l.code.c_str(), l.name.c_str()};
  }
}

int settings_count(const Setting* table) {
  int n = 0;
  while (table[n].key) ++n;
  return n;
}

namespace {

bool truthy(const std::string& v) {
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Percent rows keep a 0..1 double in the file; round to the nearest whole
// percent so display and re-stepped values agree.
int percent_of(const std::string& v) {
  return static_cast<int>(std::lround(std::atof(v.c_str()) * 100.0));
}
std::string percent_str(int p) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%.2f", p / 100.0);
  std::string s = buf;   // trim trailing zeros: "0.50" -> "0.5"
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s;
}

int choice_index(const Setting& s, const std::string& v) {
  // video.aa's older mode words are on (the frontend's aa_on).
  if (s.type == T::Bool && !std::strcmp(s.key, "video.aa") && (v == "enhanced" || v == "accurate" || v == "smooth")) return 1;
  for (int i = 0; i < s.nchoices; ++i) {
    if (v == s.choices[i].value) return i;
    if (s.type == T::Bool && truthy(v) == truthy(s.choices[i].value)) return i;   // tolerate hand-edited variants
  }
  return -1;
}

} // namespace

std::string default_value(const Setting& s) {
  if (s.def) return s.def;
  switch (s.type) {
  case T::Bool:
  case T::Pick:    return s.choices[0].value;
  case T::Int:
  case T::Percent: return s.sentinel_value ? s.sentinel_value : std::to_string(s.lo);
  case T::Text:    return "";
  }
  return "";
}

std::string display_value(const Setting& s, const std::string& value) {
  // Unset text shows the firmware's default, not "--"; only an explicitly emptied field reads as empty.
  if (s.type == T::Text) {
    const std::string v = value.empty() ? default_value(s) : value;
    return v.empty() ? "--" : v;
  }
  const std::string v = value.empty() ? default_value(s) : value;
  switch (s.type) {
  case T::Text: break;   // handled above
  case T::Bool:
  case T::Pick: {
    const int i = choice_index(s, v);
    if (i < 0) return v;   // unknown value: show as-is, not silently remapped
    const char* label = s.choices[i].label;
    // Language names and file names are their own; only the row's first
    // choice (AUTO, TEST PATTERN) is translated.
    if ((s.flags & FlagVerbatim) && i != 0) return label;
    // A choice marked NC_(key, ...) has the setting's key as its context.
    const char* in_context = i18n::trc(s.key, label);
    return in_context != label ? in_context : i18n::tr(label);
  }
  case T::Int:
    if (s.sentinel_value && v == s.sentinel_value) return i18n::tr(s.sentinel_label);
    return s.suffix ? v + s.suffix : v;
  case T::Percent: {
    if (s.sentinel_value && v == s.sentinel_value) return i18n::tr(s.sentinel_label);
    return std::to_string(percent_of(v)) + "%";
  }
  }
  return v;
}

std::string step_value(const Setting& s, const std::string& value, int dir, const SettingsHost& host) {
  if (s.type == T::Text) return value;   // stepped a character at a time, not as a whole
  const std::string v = value.empty() ? default_value(s) : value;
  if (s.type == T::Bool || s.type == T::Pick) {
    int i = choice_index(s, v);
    if (i < 0) i = 0;
    // Wraps (lists are short); skips choices the tier disallows; gives up if none are allowed.
    for (int n = 0; n < s.nchoices; ++n) {
      i = (i + (dir > 0 ? 1 : s.nchoices - 1)) % s.nchoices;
      if (*s.choices[i].value && host.value_allowed(s, s.choices[i].value)) return s.choices[i].value;
    }
    return v;
  }
  const bool at_sentinel = s.sentinel_value && v == s.sentinel_value;
  if (at_sentinel) {   // sentinel sits one step below lo: down is a no-op, up lands on lo
    if (dir <= 0) return v;
    return s.type == T::Percent ? percent_str(s.lo) : std::to_string(s.lo);
  }
  const int cur = s.type == T::Percent ? percent_of(v) : std::atoi(v.c_str());
  int next = cur + dir * s.step;
  if (next < s.lo) {
    if (s.sentinel_value) return s.sentinel_value;
    next = s.lo;
  }
  if (next > s.hi) next = s.hi;
  return s.type == T::Percent ? percent_str(next) : std::to_string(next);
}

} // namespace ds::sdl
