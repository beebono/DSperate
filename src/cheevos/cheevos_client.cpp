// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_client.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/nds.h"
#include "rc_client.h"

namespace ds::cheevos {
namespace {

bool verbose() {
  static const bool on = [] {
    const char* e = std::getenv("DS_CHEEVOS_VERBOSE");
    return e && std::atoi(e) != 0;
  }();
  return on;
}

#define CLOG(...) do { if (verbose()) std::fprintf(stderr, "cheevos: " __VA_ARGS__); } while (0)

const char* token_name = "/cheevos.token";

// The callbacks rcheevos is handed, with exactly its signatures, each one
// finding the session through rc_client_get_userdata and forwarding. Written
// out rather than casting Client's members to these types: a cast between
// function pointer types is not something to rely on, and these cost nothing.
Client* session_of(const rc_client_t* c) {
  return static_cast<Client*>(rc_client_get_userdata(c));
}

uint32_t rc_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* c) {
  Client* self = session_of(c);
  if (!self) { std::memset(buffer, 0, num_bytes); return 0; }
  return self->read(address, buffer, num_bytes);
}

void rc_server_call(const rc_api_request_t* request, rc_client_server_callback_t callback,
                    void* callback_data, rc_client_t* c) {
  Client* self = session_of(c);
  if (!self || !request) return;
  Request req;
  req.url = request->url ? request->url : "";
  req.body = request->post_data ? request->post_data : "";
  req.content_type = request->content_type ? request->content_type : "";
  self->enqueue(std::move(req), reinterpret_cast<void*>(callback), callback_data);
}

void rc_event_handler(const rc_client_event_t* event, rc_client_t* c) {
  Client* self = session_of(c);
  if (self) self->handle_event(event);
}

void rc_log(const char* message, const rc_client_t* c) {
  (void)c;
  std::fprintf(stderr, "cheevos: rc: %s\n", message ? message : "");
}

} // namespace

// ---------------------------------------------------------------------------
// Credentials

bool load_credentials(const std::string& dir, Credentials& out, std::string& err) {
  out = Credentials{};
  err.clear();
  const std::string path = dir + token_name;
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return true;   // not signed in yet; not a failure

  char line[512];
  std::string fields[2];
  for (int i = 0; i < 2 && std::fgets(line, sizeof line, f); ++i) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    fields[i] = s;
  }
  std::fclose(f);
  out.username = fields[0];
  out.token = fields[1];
  if (out.empty()) {
    out = Credentials{};
    err = "the saved credentials are incomplete";
    return false;
  }
  return true;
}

bool save_credentials(const std::string& dir, const Credentials& in, std::string& err) {
  err.clear();
  if (in.empty()) { err = "nothing to save"; return false; }
  // A newline in either field would corrupt the file, and neither can legally
  // contain one.
  if (in.username.find('\n') != std::string::npos || in.token.find('\n') != std::string::npos) {
    err = "credentials contain a newline";
    return false;
  }

  // Written 0600 from the start, not chmod'ed afterwards: a token readable by
  // another user, even briefly, is a token to treat as compromised. O_EXCL on a
  // temporary then rename keeps the live file whole if we are interrupted.
  const std::string path = dir + token_name;
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) { err = std::strerror(errno); return false; }
  const std::string body = in.username + "\n" + in.token + "\n";
  const ssize_t n = ::write(fd, body.data(), body.size());
  const bool ok = n == static_cast<ssize_t>(body.size());
  ::close(fd);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    err = std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

void clear_credentials(const std::string& dir) {
  const std::string path = dir + token_name;
  ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// Session

Client::Client() = default;
Client::~Client() { shutdown(); }

const char* Client::transport_name() const { return http_ ? http_->name() : "none"; }

void Client::post(Message::Kind kind, std::string text, std::string detail) {
  std::lock_guard<std::mutex> lk(mu_);
  messages_.push_back(Message{kind, std::move(text), std::move(detail)});
}

std::vector<Message> Client::take_messages() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Message> out;
  out.swap(messages_);
  return out;
}

bool Client::start(std::string& err) {
  if (client_) return true;
  err.clear();

  http_ = make_curl_backend(err);
  if (!http_) {
    // Expected on a device whose CFW ships no libcurl, and on the -static
    // tiers, which cannot dlopen at all. Report once, carry on without.
    unavailable_ = "no HTTP support on this device (" + err + ")";
    state_ = State::Off;
    return false;
  }

  rc_client_t* c = rc_client_create(&rc_read_memory, &rc_server_call);
  if (!c) {
    err = "rc_client_create failed";
    unavailable_ = err;
    http_.reset();
    return false;
  }
  client_ = c;
  rc_client_set_userdata(c, this);

  // CASUAL MODE. rc_client defaults hardcore ON (rc_client.c:173), so this is
  // the line that makes this emulator's unlocks honest: DSperate permits save
  // states and cheats, so it must never claim a hardcore unlock. Submitting one
  // would put bad data on another person's account, which is the only failure
  // here with consequences outside our own build. Nothing sets it back.
  rc_client_set_hardcore_enabled(c, 0);

  // Memory is read only from inside do_frame/idle, which is how we can promise
  // that guest memory is touched on the emulation thread and nowhere else.
  rc_client_set_allow_background_memory_reads(c, 0);

  if (verbose()) rc_client_enable_logging(c, RC_CLIENT_LOG_LEVEL_VERBOSE, &rc_log);
  rc_client_set_event_handler(c, &rc_event_handler);

  worker_ = std::thread([this] { worker_loop(); });
  state_ = State::SignedOut;
  CLOG("session up, transport %s, user agent %s\n", transport_name(), user_agent());
  return true;
}

void Client::shutdown() {
  if (worker_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }
  if (client_) {
    // Anything still queued is dropped rather than completed: rc_client is
    // about to go away, and calling back into it mid-teardown is how you get a
    // crash at exit. rc_client_destroy releases its own pending state.
    rc_client_destroy(static_cast<rc_client_t*>(client_));
    client_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    jobs_.clear();
    done_.clear();
  }
  http_.reset();
  state_ = State::Off;
}

// ---------------------------------------------------------------------------
// The worker

void Client::worker_loop() {
  // main.cpp may have put the whole process under SCHED_RR (emu.realtime), and
  // this thread inherits it. That is wrong for this thread in a way the project
  // has already been bitten by: rt-scheduling-closes-the-tail found the frame
  // tail was preemption by unrelated threads, so a thread that blocks on a
  // socket for 270 ms must not hold a real-time priority. Drop to SCHED_OTHER
  // and then below everything else.
#if defined(__linux__)
  sched_param sp{};
  sp.sched_priority = 0;
  if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp) != 0 && verbose())
    std::perror("cheevos: worker SCHED_OTHER");
  // Thread-level nice on Linux; ignored if the kernel disallows it.
  if (nice(10) == -1 && errno != 0 && verbose()) std::perror("cheevos: worker nice");
#endif

  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }

    CLOG("-> %s%s\n", job.req.url.c_str(), job.req.body.empty() ? "" : " (POST)");
    Response res = http_->perform(job.req, user_agent());
    CLOG("<- status %d, %zu bytes%s%s\n", res.status, res.body.size(),
         res.error.empty() ? "" : ", ", res.error.c_str());

    {
      std::lock_guard<std::mutex> lk(mu_);
      done_.push_back(Completion{std::move(res), job.callback, job.callback_data});
    }
  }
}

void Client::enqueue(Request req, void* callback, void* callback_data) {
  Job job;
  job.req = std::move(req);
  job.callback = callback;
  job.callback_data = callback_data;
  {
    std::lock_guard<std::mutex> lk(mu_);
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void Client::drain_completions() {
  for (;;) {
    Completion done;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (done_.empty()) return;
      done = std::move(done_.front());
      done_.pop_front();
    }
    // On the emulation thread, which is the whole point: every rcheevos
    // callback, and therefore everything that touches rc_client, runs here.
    rc_api_server_response_t res{};
    res.body = done.res.body.c_str();
    res.body_length = done.res.body.size();
    // 0 when the request never reached a server. rcheevos reads that as a
    // retryable transport failure and requeues the unlock, which is what a
    // handheld that left Wi-Fi range needs.
    res.http_status_code = done.res.status;
    auto cb = reinterpret_cast<rc_client_server_callback_t>(done.callback);
    if (cb) cb(&res, done.callback_data);
  }
}

// ---------------------------------------------------------------------------
// Per-frame

void Client::frame() {
  if (!client_) return;
  drain_completions();
  rc_client_do_frame(static_cast<rc_client_t*>(client_));
}

void Client::idle() {
  if (!client_) return;
  drain_completions();
  rc_client_idle(static_cast<rc_client_t*>(client_));
}

u32 Client::read(u32 address, u8* buffer, u32 num_bytes) {
  return mem_.read(address, buffer, num_bytes);
}

// ---------------------------------------------------------------------------
// Sign-in

namespace {

void login_done(int result, const char* error_message, rc_client_t* c, void* userdata) {
  (void)userdata;
  Client* self = static_cast<Client*>(rc_client_get_userdata(c));
  if (!self) return;
  if (result == RC_OK) {
    const rc_client_user_t* u = rc_client_get_user_info(c);
    self->on_signed_in(u && u->display_name ? u->display_name : "");
  } else {
    self->on_sign_in_failed(error_message ? error_message : "sign-in failed");
  }
}

void load_done(int result, const char* error_message, rc_client_t* c, void* userdata) {
  (void)userdata;
  Client* self = static_cast<Client*>(rc_client_get_userdata(c));
  if (!self) return;
  if (result == RC_OK) {
    const rc_client_game_t* g = rc_client_get_game_info(c);
    self->on_game_loaded(g && g->title ? g->title : "", g ? g->id : 0);
  } else if (result == RC_NO_GAME_LOADED) {
    // The ROM hashed fine, RetroAchievements simply has no set for this dump.
    // An ordinary outcome, and one the player has to be able to see -- it is
    // otherwise indistinguishable from the feature being broken.
    self->on_no_set();
  } else {
    self->on_game_failed(error_message ? error_message : "could not load achievements");
  }
}

} // namespace

void Client::sign_in(const std::string& username, const std::string& password) {
  if (!client_) return;
  state_ = State::SigningIn;
  rc_client_begin_login_with_password(static_cast<rc_client_t*>(client_), username.c_str(),
                                      password.c_str(), &login_done, nullptr);
}

void Client::sign_in_with_token(const std::string& username, const std::string& token) {
  if (!client_) return;
  state_ = State::SigningIn;
  rc_client_begin_login_with_token(static_cast<rc_client_t*>(client_), username.c_str(),
                                   token.c_str(), &login_done, nullptr);
}

void Client::sign_out() {
  if (!client_) return;
  rc_client_logout(static_cast<rc_client_t*>(client_));
  state_ = State::SignedOut;
  post(Message::Kind::Info, "Signed out of RetroAchievements");
}

std::string Client::username() const {
  if (!client_) return {};
  const rc_client_user_t* u = rc_client_get_user_info(static_cast<rc_client_t*>(client_));
  return u && u->username ? u->username : std::string{};
}

std::string Client::token() const {
  if (!client_) return {};
  const rc_client_user_t* u = rc_client_get_user_info(static_cast<rc_client_t*>(client_));
  return u && u->token ? u->token : std::string{};
}

// ---------------------------------------------------------------------------
// Game

void Client::load_game(NDS& nds, const std::string& hash) {
  if (!client_) return;
  std::string err;
  if (!mem_.attach(nds, err)) {
    post(Message::Kind::Problem, "Achievements unavailable", err);
    return;
  }
  state_ = State::LoadingGame;
  rc_client_begin_load_game(static_cast<rc_client_t*>(client_), hash.c_str(), &load_done, nullptr);
}

void Client::unload_game() {
  if (!client_) return;
  rc_client_unload_game(static_cast<rc_client_t*>(client_));
  if (state_ == State::Playing || state_ == State::LoadingGame || state_ == State::NoSet)
    state_ = State::SignedIn;
}

std::string Client::game_title() const {
  if (!client_) return {};
  const rc_client_game_t* g = rc_client_get_game_info(static_cast<rc_client_t*>(client_));
  return g && g->title ? g->title : std::string{};
}

u32 Client::game_id() const {
  if (!client_) return 0;
  const rc_client_game_t* g = rc_client_get_game_info(static_cast<rc_client_t*>(client_));
  return g ? g->id : 0;
}

// ---------------------------------------------------------------------------
// Outcomes

void Client::on_signed_in(const std::string& display_name) {
  state_ = State::SignedIn;
  post(Message::Kind::Info, "Signed in to RetroAchievements",
       display_name.empty() ? std::string{} : display_name);
  CLOG("signed in as %s\n", display_name.c_str());
}

void Client::on_sign_in_failed(const std::string& why) {
  state_ = State::SignedOut;
  post(Message::Kind::Problem, "RetroAchievements sign-in failed", why);
}

void Client::on_game_loaded(const std::string& title, u32 id) {
  state_ = State::Playing;
  post(Message::Kind::Info, title.empty() ? "Achievements loaded" : title,
       "achievements active");
  CLOG("game %u loaded: %s\n", id, title.c_str());
}

void Client::on_no_set() {
  state_ = State::NoSet;
  post(Message::Kind::Problem, "No achievements for this game",
       "RetroAchievements does not have a set for this ROM");
}

void Client::on_game_failed(const std::string& why) {
  state_ = State::SignedIn;
  post(Message::Kind::Problem, "Could not load achievements", why);
}

// ---------------------------------------------------------------------------
// Events from rcheevos

void Client::handle_event(const void* event_ptr) {
  const rc_client_event_t* e = static_cast<const rc_client_event_t*>(event_ptr);
  if (!e) return;
  Client* self = this;

  switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      if (e->achievement) {
        self->post(Message::Kind::Unlock,
                   e->achievement->title ? e->achievement->title : "Achievement unlocked",
                   e->achievement->description ? e->achievement->description : "");
      }
      break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
      self->post(Message::Kind::Info, "All achievements earned", self->game_title());
      break;
    case RC_CLIENT_EVENT_SUBSET_COMPLETED:
      self->post(Message::Kind::Info, "Subset completed");
      break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
      self->post(Message::Kind::Problem, "RetroAchievements error",
                 e->server_error && e->server_error->error_message ? e->server_error->error_message : "");
      break;
    case RC_CLIENT_EVENT_DISCONNECTED:
      // Unlocks are being held, not lost. Saying so is the difference between
      // a player trusting the feature and not.
      self->post(Message::Kind::Problem, "RetroAchievements offline", "unlocks will be sent when the connection returns");
      break;
    case RC_CLIENT_EVENT_RECONNECTED:
      self->post(Message::Kind::Info, "RetroAchievements reconnected", "pending unlocks sent");
      break;
    case RC_CLIENT_EVENT_RESET:
      // Only raised by enabling hardcore, which this build never does.
      CLOG("ignoring a reset event; hardcore is not supported\n");
      break;
    default:
      break;   // indicators and leaderboards are phase 4 and later
  }
}

} // namespace ds::cheevos
