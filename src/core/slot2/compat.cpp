// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Which DS games read which GBA games. Gathered from the games' documented
// dual-slot features, with the codes taken from dumps' headers.
#include "core/slot2/compat.h"

#include <cstring>
#include <initializer_list>

namespace ds::slot2 {

namespace {

constexpr const char* ADVANCE_WARS[] = {"AWR", "AW2", nullptr};
constexpr const char* BLEACH[] = {"BLE", nullptr};
constexpr const char* ARIA_OF_SORROW[] = {"A2C", nullptr};
constexpr const char* BAND_BROTHERS[] = {"ZBB", nullptr};   // the Request Selection expansion cartridge
constexpr const char* EYESHIELD_21[] = {"BE4", nullptr};
constexpr const char* FFTA[] = {"AFX", nullptr};
constexpr const char* KIRBY_GBA[] = {"A7K", "B8K", nullptr};
constexpr const char* FROGGER[] = {"BFJ", "AFR", "AFB", nullptr};
constexpr const char* FULLMETAL[] = {"BH2", "BHR", nullptr};
constexpr const char* GOEMON[] = {"BG2", nullptr};
constexpr const char* MINERAL_TOWN[] = {"A4N", "BFG", nullptr};
constexpr const char* KIM_POSSIBLE[] = {"AEY", "BKM", "BQP", nullptr};
constexpr const char* YGGDRA_UNION[] = {"BYU", nullptr};
constexpr const char* BOKTAI[] = {"U3I", "U32", "U33", nullptr};
constexpr const char* MADDEN[] = {"B6M", "BMF", nullptr};
constexpr const char* BN5_DS[] = {"ARE", "AE2", "A3X", "A6B", "B4B", "B4W", "BR4", "BRB", "BRK", "U3I", "U32", "U33", nullptr};
constexpr const char* STAR_FORCE[] = {"ARE", "AE2", "A3X", "A6B", "B4B", "B4W", "BR4", "BRB", "BRK", "BR5", "BR6", "A89", nullptr};
constexpr const char* MEGA_MAN_ZERO[] = {"BZ3", "B4Z", nullptr};
constexpr const char* GYAKUTEN_1[] = {"ASB", nullptr};
constexpr const char* GYAKUTEN_2[] = {"A3G", nullptr};
constexpr const char* GYAKUTEN_3[] = {"A3J", nullptr};
constexpr const char* POKEMON_GEN3[] = {"AXV", "AXP", "BPE", "BPR", "BPG", nullptr};
constexpr const char* PMD_RED[] = {"B24", nullptr};
constexpr const char* SHREK[] = {"B4U", nullptr};
constexpr const char* MYSTERIOS_MENACE[] = {"ASE", nullptr};
constexpr const char* SUPER_ROBOT[] = {"ASR", "AJ9", "A6S", "B6J", "AOG", "B2R", "FSR", nullptr};
constexpr const char* SEGA_GBA[] = {"ACR", "APY", "ASO", "A2N", "B3S", "BSB", nullptr};
constexpr const char* SEGA_GBA_FEEL[] = {"ACR", "APY", "ASO", "A2N", "B3S", "BSB", "A3V", "A5U", nullptr};
constexpr const char* SIMS_2[] = {"B46", nullptr};
constexpr const char* WARIOWARE[] = {"RZW", nullptr};
constexpr const char* YOSHIS_ISLAND[] = {"A3A", nullptr};
constexpr const char* YUGIOH_GX[] = {"BYG", nullptr};

constexpr Linkage TABLE[] = {
  {"AWR", ADVANCE_WARS},       // Advance Wars: Dual Strike
  {"ABL", BLEACH},             // Bleach DS: Souten ni Kakeru Unmei / The Blade of Fate
  {"ACV", ARIA_OF_SORROW},     // Castlevania: Dawn of Sorrow (same region only)
  {"ABB", BAND_BROTHERS},      // Daigassou! Band-Brothers
  {"AE4", EYESHIELD_21},       // Eyeshield 21: Max Devil Power
  {"APR", SEGA_GBA_FEEL},      // Feel the Magic: XY/XX
  {"A6F", FFTA},               // Final Fantasy Tactics A2
  {"AFG", FROGGER},            // Frogger: Helmet Chaos
  {"AHR", FULLMETAL},          // Fullmetal Alchemist: Dual Sympathy
  {"A5E", GOEMON},             // Ganbare Goemon: Toukai Douchuu Ooedo Tengurigaeshi no Maki
  {"ABC", MINERAL_TOWN},       // Harvest Moon DS
  {"AKP", KIM_POSSIBLE},       // Kim Possible: Kimmunicator
  {"CKN", YGGDRA_UNION},       // Knights in the Nightmare
  {"ATK", KIRBY_GBA},          // Kirby: Canvas Curse
  {"ALK", BOKTAI},             // Lunar Knights (the solar sensor too)
  {"AM6", MADDEN},             // Madden NFL 06
  {"A5T", BN5_DS},             // Mega Man Battle Network 5: Double Team DS
  {"A6A", STAR_FORCE},         // Mega Man Star Force: Pegasus
  {"A6B", STAR_FORCE},         // Mega Man Star Force: Leo
  {"A6C", STAR_FORCE},         // Mega Man Star Force: Dragon
  {"ARZ", MEGA_MAN_ZERO},      // Mega Man ZX
  {"AGY", GYAKUTEN_1},         // Phoenix Wright: Ace Attorney
  {"A2G", GYAKUTEN_2},         // Phoenix Wright: Ace Attorney - Justice for All
  {"YG3", GYAKUTEN_3},         // Phoenix Wright: Ace Attorney - Trials and Tribulations
  {"APD", POKEMON_GEN3},       // Pokemon Dash
  {"ADA", POKEMON_GEN3},       // Pokemon Diamond
  {"APA", POKEMON_GEN3},       // Pokemon Pearl
  {"CPU", POKEMON_GEN3},       // Pokemon Platinum
  {"IPK", POKEMON_GEN3},       // Pokemon HeartGold
  {"IPG", POKEMON_GEN3},       // Pokemon SoulSilver
  {"APH", PMD_RED},            // Pokemon Mystery Dungeon: Blue Rescue Team
  {"AS3", SHREK},              // Shrek SuperSlam
  {"AS2", MYSTERIOS_MENACE},   // Spider-Man 2
  {"AS6", SUPER_ROBOT},        // Super Robot Taisen W
  {"YSD", SUPER_ROBOT},        // Super Robot Taisen K
  {"AKA", SEGA_GBA},           // The Rub Rabbits!
  {"ASJ", SIMS_2},             // The Sims 2
  {"AZW", WARIOWARE},          // WarioWare: Touched!
  {"AYI", YOSHIS_ISLAND},      // Yoshi Touch & Go
  {"AYG", YUGIOH_GX},          // Yu-Gi-Oh! Nightmare Troubadour
};

} // namespace

const Linkage* linkage_for(const char ds_code[4]) {
  for (const Linkage& l : TABLE)
    if (std::memcmp(l.ds, ds_code, 3) == 0) return &l;
  return nullptr;
}

// Every region shares the first three letters (USA E, Europe P/X, Japan J, Korea K).
bool uses_guitar_grip(const char ds_code[4]) {
  for (const char* c : {"YGH",    // Guitar Hero: On Tour
                        "CGS",    // Guitar Hero: On Tour - Decades
                        "C6Q",    // Guitar Hero: On Tour - Modern Hits
                        "BGH"})   // Band Hero
    if (std::memcmp(c, ds_code, 3) == 0) return true;
  return false;
}

bool accepts(const Linkage& link, const char gba_code[4]) {
  for (const char* const* c = link.gba; *c; ++c)
    if (std::memcmp(*c, gba_code, 3) == 0) return true;
  return false;
}

} // namespace ds::slot2
