// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Drives a real RetroAchievements session from the command line, which is how
// phase 3 is exercised before there is any UI
// (docs/retroachievements-scoping.md).
//
//   tools/cheevos_session                         -- transport only
//   tools/cheevos_session <user> <password>       -- sign in
//   tools/cheevos_session <user> <password> <rom> -- sign in and load a ROM's set
//
// A password is taken from DS_CHEEVOS_PASSWORD in preference to argv, since
// argv is visible in ps. DS_CHEEVOS_TOKEN signs in with a token instead.
//
// Note what this proves even with deliberately wrong credentials: if the server
// answers with its own error message rather than a transport failure, then
// dlopen, TLS, the trust store, the User-Agent, rapi's request building and its
// response parsing are all working. That is the whole pipe bar a valid account.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "cheevos/cheevos_client.h"
#include "cheevos/cheevos_hash.h"
#include "core/cart/rom_source.h"
#include "core/nds.h"

using namespace ds;

namespace {

const char* state_name(cheevos::State s) {
  switch (s) {
    case cheevos::State::Off:         return "off";
    case cheevos::State::SignedOut:   return "signed out";
    case cheevos::State::SigningIn:   return "signing in";
    case cheevos::State::SignedIn:    return "signed in";
    case cheevos::State::LoadingGame: return "loading game";
    case cheevos::State::Playing:     return "playing";
    case cheevos::State::NoSet:       return "no achievement set";
  }
  return "?";
}

void show(cheevos::Client& c) {
  for (const cheevos::Message& m : c.take_messages()) {
    const char* tag = m.kind == cheevos::Message::Kind::Unlock ? "UNLOCK"
                    : m.kind == cheevos::Message::Kind::Problem ? "problem" : "info";
    std::printf("  [%s] %s%s%s\n", tag, m.text.c_str(),
                m.detail.empty() ? "" : " -- ", m.detail.c_str());
  }
}

// Stands in for the emulator's loop: one frame() per emulated frame, which is
// also what drains the HTTP completions. Runs until `settled` or the deadline.
template <typename Pred>
bool pump(cheevos::Client& c, Pred settled, int seconds) {
  const int frames = seconds * 60;
  for (int i = 0; i < frames; ++i) {
    c.frame();
    show(c);
    if (settled()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  cheevos::Client client;
  std::string err;
  if (!client.start(err)) {
    std::printf("transport unavailable: %s\n", client.unavailable_reason().c_str());
    std::printf("(this is a normal outcome on a device with no libcurl)\n");
    return 1;
  }
  std::printf("transport: %s\n", client.transport_name());
  std::printf("state:     %s\n", state_name(client.state()));

  const char* env_pass = std::getenv("DS_CHEEVOS_PASSWORD");
  const char* env_token = std::getenv("DS_CHEEVOS_TOKEN");
  const std::string user = argc > 1 ? argv[1] : "";
  const std::string pass = env_pass ? env_pass : (argc > 2 ? argv[2] : "");

  if (user.empty()) {
    std::printf("no user given; transport is up, nothing else to do\n");
    client.shutdown();
    return 0;
  }

  std::printf("signing in as %s...\n", user.c_str());
  if (env_token && *env_token) client.sign_in_with_token(user, env_token);
  else client.sign_in(user, pass);

  pump(client, [&] { return client.state() != cheevos::State::SigningIn; }, 30);
  std::printf("state:     %s\n", state_name(client.state()));
  if (client.state() != cheevos::State::SignedIn) {
    // The interesting failure. A message from the server means the pipe works.
    client.shutdown();
    return 2;
  }
  std::printf("user:      %s\n", client.username().c_str());
  std::printf("token:     %s\n", client.token().empty() ? "(none)" : "(received, would be saved 0600)");

  const char* rom = argc > 3 ? argv[3] : nullptr;
  if (!rom) {
    client.shutdown();
    return 0;
  }

  auto src = cart::RomSource::map_file(rom, err);
  if (!src) {
    std::fprintf(stderr, "%s: %s\n", rom, err.c_str());
    client.shutdown();
    return 1;
  }
  std::string hash;
  if (!cheevos::rom_hash(*src, rom, hash, err)) {
    std::fprintf(stderr, "%s: %s\n", rom, err.c_str());
    client.shutdown();
    return 1;
  }
  std::printf("hash:      %s\n", hash.c_str());

  // A console, so the memory window has something to attach to. Not booted --
  // loading a set does not need the game running, and this tool is not trying
  // to actually play anything.
  NDS nds;
  client.load_game(nds, hash);
  pump(client, [&] {
    const auto s = client.state();
    return s == cheevos::State::Playing || s == cheevos::State::NoSet || s == cheevos::State::SignedIn;
  }, 30);

  std::printf("state:     %s\n", state_name(client.state()));
  if (client.state() == cheevos::State::Playing)
    std::printf("game:      %u  %s\n", client.game_id(), client.game_title().c_str());

  // A few hundred frames against un-booted memory: nothing should unlock, and
  // the point is that do_frame is stable with a real set loaded.
  for (int i = 0; i < 300; ++i) { client.frame(); show(client); }
  client.shutdown();
  return 0;
}
