// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The credential sidecar (docs/retroachievements-scoping.md, phase 3). No
// network: what is tested is that the token is stored the way a credential has
// to be, because the token alone is enough to act as the player on
// RetroAchievements.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "cheevos/cheevos_client.h"

using namespace ds;

namespace {

std::string make_dir() {
  char tmpl[] = "/tmp/dsperate_cheevos_credsXXXXXX";
  const char* d = mkdtemp(tmpl);
  CHECK(d != nullptr);
  return std::string(d);
}

void remove_dir(const std::string& dir) {
  ::unlink((dir + "/cheevos.token").c_str());
  ::unlink((dir + "/cheevos.token.tmp").c_str());
  ::rmdir(dir.c_str());
}

void no_file_is_not_an_error() {
  const std::string dir = make_dir();
  cheevos::Credentials c;
  std::string err;
  // Never signed in. That is a normal state, not a failure, and conflating the
  // two would make a first run look broken.
  CHECK(cheevos::load_credentials(dir, c, err));
  CHECK(err.empty());
  CHECK(c.empty());
  remove_dir(dir);
}

void a_token_round_trips() {
  const std::string dir = make_dir();
  std::string err;
  cheevos::Credentials in;
  in.username = "someplayer";
  in.token = "AbCdEf0123456789";
  CHECK(cheevos::save_credentials(dir, in, err));
  CHECK(err.empty());

  cheevos::Credentials out;
  CHECK(cheevos::load_credentials(dir, out, err));
  CHECK(err.empty());
  CHECK(!out.empty());
  CHECK(out.username == in.username);
  CHECK(out.token == in.token);
  remove_dir(dir);
}

// The token is a credential. 0600 from creation, not chmod'ed afterwards: a
// token another user could read even briefly is a token to treat as leaked.
void the_file_is_owner_only() {
  const std::string dir = make_dir();
  std::string err;
  cheevos::Credentials in{"someplayer", "AbCdEf0123456789"};
  CHECK(cheevos::save_credentials(dir, in, err));

  struct stat st{};
  CHECK(::stat((dir + "/cheevos.token").c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  remove_dir(dir);
}

// Saving twice must leave one good file, not a stale temporary beside it.
void saving_again_replaces_it() {
  const std::string dir = make_dir();
  std::string err;
  CHECK(cheevos::save_credentials(dir, {"first", "token-one"}, err));
  CHECK(cheevos::save_credentials(dir, {"second", "token-two"}, err));

  cheevos::Credentials out;
  CHECK(cheevos::load_credentials(dir, out, err));
  CHECK(out.username == "second");
  CHECK(out.token == "token-two");
  struct stat st{};
  CHECK(::stat((dir + "/cheevos.token.tmp").c_str(), &st) != 0);   // no leftovers
  remove_dir(dir);
}

void signing_out_removes_it() {
  const std::string dir = make_dir();
  std::string err;
  CHECK(cheevos::save_credentials(dir, {"someplayer", "tok"}, err));
  cheevos::clear_credentials(dir);
  cheevos::Credentials out;
  CHECK(cheevos::load_credentials(dir, out, err));
  CHECK(out.empty());
  remove_dir(dir);
}

// The file is line-based, so a newline in either field would silently truncate
// or shift the other one. Refuse rather than write a file we cannot read back.
void a_newline_is_refused() {
  const std::string dir = make_dir();
  std::string err;
  CHECK(!cheevos::save_credentials(dir, {"some\nplayer", "tok"}, err));
  CHECK(!err.empty());
  CHECK(!cheevos::save_credentials(dir, {"someplayer", "to\nk"}, err));
  CHECK(!cheevos::save_credentials(dir, {"", "tok"}, err));
  CHECK(!cheevos::save_credentials(dir, {"someplayer", ""}, err));
  remove_dir(dir);
}

// A half-written file (one line only) must not come back as usable
// credentials, or we would try to sign in with an empty token every launch.
void an_incomplete_file_is_rejected() {
  const std::string dir = make_dir();
  std::FILE* f = std::fopen((dir + "/cheevos.token").c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("someplayer\n", f);
  std::fclose(f);

  cheevos::Credentials out;
  std::string err;
  CHECK(!cheevos::load_credentials(dir, out, err));
  CHECK(!err.empty());
  CHECK(out.empty());
  remove_dir(dir);
}

} // namespace

int main() {
  no_file_is_not_an_error();
  a_token_round_trips();
  the_file_is_owner_only();
  saving_again_replaces_it();
  signing_out_removes_it();
  a_newline_is_refused();
  an_incomplete_file_is_rejected();
  std::printf("cheevos_client: ok\n");
  return 0;
}
