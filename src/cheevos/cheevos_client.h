// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The RetroAchievements session (docs/retroachievements-scoping.md, phase 3):
// sign in, load a game's achievement set, evaluate it as the emulator runs,
// and report unlocks. Casual mode only -- hardcore is switched off explicitly
// at creation and there is no way to turn it on from here.
//
// How the threading works, because it is the part that can go wrong quietly:
//
//   * `rc_client` itself, the memory reads, and every rcheevos callback run on
//     the thread that calls frame() -- the emulation thread. Nothing else
//     touches them.
//   * HTTP runs on one worker thread, which never touches rc_client or guest
//     memory. It takes a Request, does the round trip (~270 ms to
//     retroachievements.org from the RG DS), and posts the response back.
//   * frame() drains those responses first, invoking the rcheevos callbacks on
//     the emulation thread, and then calls rc_client_do_frame.
//
// That keeps everything rcheevos sees single-threaded and ordered against
// frames, which is what the one-call-per-frame rule needs
// (tests/cheevos_memory_test.cpp).
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cheevos/cheevos_http.h"
#include "cheevos/cheevos_memory.h"
#include "core/types.h"

namespace ds { class NDS; }

namespace ds::cheevos {

// Something the player should be told about. The frontend turns these into
// toasts; phase 3 prints them.
struct Message {
  enum class Kind : u8 {
    Unlock,      // an achievement was earned
    Info,        // signed in, game loaded, reconnected
    Problem,     // login failed, server error, no achievements for this ROM
  };
  Kind kind = Kind::Info;
  std::string text;     // one line, already player-facing
  std::string detail;   // may be empty
  u32 points = 0;       // an unlock's value; 0 for everything else, and for
                        // the 0-point achievements RetroAchievements has
};

// Where the session currently is. The frontend needs this to decide what to
// draw; it is also the honest answer to "why are there no achievements".
enum class State : u8 {
  Off,          // not started, or no transport on this device
  SignedOut,
  SigningIn,
  SignedIn,
  LoadingGame,
  Playing,      // a set is loaded and being evaluated
  NoSet,        // signed in, but this ROM has no achievements
};

class Client {
public:
  Client();
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Opens the transport and creates the session. False with a reason in `err`
  // when there is no usable libcurl, which is an ordinary outcome on some
  // devices: the caller should report it once and carry on without
  // achievements, never treat it as fatal.
  bool start(std::string& err);
  void shutdown();

  State state() const { return state_; }

  // Encore mode: achievements the player has already earned are activated
  // again, so replaying a game shows them unlocking as it goes. The server
  // does not credit an unlock twice -- nothing is re-awarded -- so this changes
  // what DSperate shows, not what the account holds.
  //
  // rcheevos evaluates it when a game loads and ignores it while one is
  // loaded (rc_client.h:77), so set it before load_game(). Kept here as well
  // as in rc_client so it survives being set before start().
  void set_encore(bool on);
  // Asks rcheevos rather than reporting our own flag back: the two could
  // disagree (encore is only evaluated at game load), and the library's answer
  // is the one that decides what happens.
  bool encore() const;
  const char* transport_name() const;
  // The player-facing reason the session is not usable, when state() is Off.
  const std::string& unavailable_reason() const { return unavailable_; }

  // Sign-in. Both are asynchronous; watch state() and take_messages(). A
  // password login yields a token, which is what gets persisted -- the password
  // is never stored and is cleared from memory as soon as it is sent.
  void sign_in(const std::string& username, const std::string& password);
  void sign_in_with_token(const std::string& username, const std::string& token);
  void sign_out();
  std::string username() const;
  // The token to persist after a successful sign-in, or empty. See
  // save_credentials().
  std::string token() const;

  // Binds the session to a running console and a ROM identity. `hash` comes
  // from rom_hash() (cheevos_hash.h). Asynchronous.
  void load_game(NDS& nds, const std::string& hash);
  void unload_game();
  std::string game_title() const;
  u32 game_id() const;
  // The hash load_game() was given, which is what identifies the dump.
  const std::string& game_hash() const { return hash_; }

  // Once per emulated frame, from the emulation thread, immediately after
  // NDS::run_frame(). Exactly once -- see the header comment and the test.
  void frame();
  // Instead of frame() while emulation is paused, at least once a second, so
  // the session stays alive without evaluating frozen memory.
  void idle();

  std::vector<Message> take_messages();

  // The loaded set, as the player would see it listed. This is what phase 4's
  // achievement page is built on, and it is also how you confirm the server
  // actually recorded an unlock: load the game again and the achievement comes
  // back `unlocked`, because that state came from RetroAchievements rather than
  // from this session.
  struct Achievement {
    std::string title, description, progress;
    u32 id = 0, points = 0;
    bool unlocked = false;      // the *account* holds it -- what the list shows
    bool active = false;        // armed, i.e. it can trigger now. Normally the
                                // opposite of unlocked; in encore mode an
                                // already-earned achievement is both.
    bool unsupported = false;   // a condition reads memory we do not back
  };
  struct Summary {
    u32 total = 0, unlocked = 0, active = 0, unsupported = 0;
    u32 points = 0, points_earned = 0;
  };
  std::vector<Achievement> achievements() const;
  Summary summary() const;

  // Called by rcheevos' completion callbacks, which run on the emulation
  // thread inside frame(). Public only because those callbacks are free
  // functions in the implementation; not for the frontend.
  void on_signed_in(const std::string& display_name);
  void on_sign_in_failed(const std::string& why);
  void on_game_loaded(const std::string& title, u32 id);
  void on_no_set();
  void on_game_failed(const std::string& why);

  // The bodies behind the callbacks rcheevos is given. Typed trampolines with
  // rcheevos' own signatures live in the implementation and forward here, so
  // no function pointer is ever cast. Public for the same reason as the on_*
  // methods above; not for the frontend.
  void enqueue(Request req, void* callback, void* callback_data);
  void handle_event(const void* event);
  u32 read(u32 address, u8* buffer, u32 num_bytes);

private:
  void worker_loop();
  void post(Message::Kind kind, std::string text, std::string detail = {}, u32 points = 0);
  void drain_completions();

  // One HTTP round trip, and the rcheevos callback waiting on it.
  struct Job {
    Request req;
    void* callback = nullptr;        // rc_client_server_callback_t
    void* callback_data = nullptr;
  };
  struct Completion {
    Response res;
    void* callback = nullptr;
    void* callback_data = nullptr;
  };

  void* client_ = nullptr;           // rc_client_t*
  std::unique_ptr<Backend> http_;
  Memory mem_;
  State state_ = State::Off;
  bool encore_ = false;
  std::string hash_;                 // the identity of the dump in the slot
  std::string unavailable_;

  std::thread worker_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::deque<Completion> done_;
  std::vector<Message> messages_;
  bool stop_ = false;
};

// The token sidecar. Beside the config rather than in dsperate.ini, following
// the firmware .ovr precedent, and mode 0600 because it is a credential: the
// token is enough to act as the player on RetroAchievements.
//
// The password is never stored. A stale token must fall back to a password
// sign-in, or a player whose token expires is locked out of their own account
// with no way back from inside the emulator -- drastic-nano's lesson.
struct Credentials {
  std::string username;
  std::string token;
  bool empty() const { return username.empty() || token.empty(); }
};
// `dir` is the config directory (Config::dir()). Both return false with a
// reason in `err`; a missing file is not an error, it is empty credentials.
bool load_credentials(const std::string& dir, Credentials& out, std::string& err);
bool save_credentials(const std::string& dir, const Credentials& in, std::string& err);
void clear_credentials(const std::string& dir);

// Credentials the CFW already holds, because the player signed in through its
// own front end. On ROCKNIX, EmulationStation's sign-in lands in
// /storage/.config/system/configs/system.cfg as
// global.retroachievements.username / .token, and RetroArch keeps the same
// thing as cheevos_username / cheevos_token. Importing it means a player who
// has already signed in on the device does not have to type a password again on
// a machine with no keyboard.
//
// Only the **token** is ever read. Those files also hold the password in clear
// text, and we do not want it: the token is all rc_client needs, it can be
// revoked on its own, and copying somebody's password into a second program is
// strictly worse than not. Nothing here writes to the CFW's file either -- it
// is not ours.
//
// read_cfw_credentials parses one file, accepting both shapes (`key=value` and
// `key = "value"`). import_cfw_credentials walks the known locations and
// reports which one it used in `source`. Both return false when there is
// nothing usable, which is the ordinary case and not an error.
bool read_cfw_credentials(const std::string& path, Credentials& out);
bool import_cfw_credentials(Credentials& out, std::string& source);

} // namespace ds::cheevos
