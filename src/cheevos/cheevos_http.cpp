// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_http.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <sys/stat.h>

namespace ds::cheevos {
namespace {

// libcurl's ABI, declared rather than included. We never build against a curl
// package -- the point of dlopen is that there is nothing to find at build time
// -- so the handful of entry points and option numbers we use are written out
// here.
//
// The option numbers are safe to hard-code: curl assigns each one a permanent
// number (type base + index) and has never renumbered one, because every binary
// ever linked against it would break. The values below were read out of
// curl 8.5.0's include/curl/curl.h, and the type bases are in the same file:
// LONG 0, OBJECTPOINT 10000, FUNCTIONPOINT 20000.
using CURL = void;
constexpr long CURL_GLOBAL_ALL = 3;
constexpr int  CURLE_OK = 0;

enum CurlOpt : int {
  CURLOPT_WRITEDATA        = 10001,   // CBPOINT 1
  CURLOPT_URL              = 10002,   // STRINGPOINT 2
  CURLOPT_WRITEFUNCTION    = 20011,   // FUNCTIONPOINT 11
  CURLOPT_POSTFIELDS       = 10015,   // OBJECTPOINT 15
  CURLOPT_USERAGENT        = 10018,   // STRINGPOINT 18
  CURLOPT_HTTPHEADER       = 10023,   // SLISTPOINT 23
  CURLOPT_POST             = 47,      // LONG 47
  CURLOPT_FOLLOWLOCATION   = 52,      // LONG 52
  CURLOPT_POSTFIELDSIZE    = 60,      // LONG 60
  CURLOPT_CAINFO           = 10065,   // STRINGPOINT 65
  CURLOPT_NOSIGNAL         = 99,      // LONG 99
  CURLOPT_ACCEPT_ENCODING  = 10102,   // STRINGPOINT 102
  CURLOPT_TIMEOUT_MS       = 155,     // LONG 155
  CURLOPT_CONNECTTIMEOUT_MS = 156,    // LONG 156
};
constexpr int CURLINFO_RESPONSE_CODE = 0x200000 + 2;   // CURLINFO_LONG + 2

struct curl_slist;

struct Api {
  int   (*global_init)(long);
  CURL* (*easy_init)();
  int   (*easy_setopt)(CURL*, int, ...);
  int   (*easy_perform)(CURL*);
  int   (*easy_getinfo)(CURL*, int, ...);
  void  (*easy_cleanup)(CURL*);
  const char* (*easy_strerror)(int);
  curl_slist* (*slist_append)(curl_slist*, const char*);
  void  (*slist_free_all)(curl_slist*);
};

// Trust stores, for a libcurl whose compiled-in path does not exist here. That
// is the normal case for one a packager dropped into the emulator's own lib
// directory: it was built against some other filesystem. Only consulted when
// the usual system stores are absent, so a CFW that has its own keeps using it.
const char* const kCaPaths[] = {
  "/etc/ssl/certs/ca-certificates.crt",
  "/etc/pki/tls/certs/ca-bundle.crt",
  "/etc/ssl/cert.pem",
  "/mnt/SDCARD/spruce/etc/ca-certificates.crt",
};

bool exists(const char* path) {
  struct stat st{};
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Growth-bounded sink. A RetroAchievements response is a few KB; a patch (the
// achievement set for a game) can be a few hundred. The cap is generous and
// exists so a confused or hostile response cannot grow without limit on a
// 512 MB handheld.
constexpr size_t MAX_BODY = 8u << 20;

struct Sink {
  std::string* out;
  bool overflowed = false;
};

size_t write_cb(char* data, size_t size, size_t nmemb, void* user) {
  Sink* s = static_cast<Sink*>(user);
  const size_t n = size * nmemb;
  if (s->out->size() + n > MAX_BODY) { s->overflowed = true; return 0; }   // 0 aborts the transfer
  s->out->append(data, n);
  return n;
}

class CurlBackend final : public Backend {
public:
  CurlBackend(void* handle, const Api& api, const char* from)
      : handle_(handle), api_(api), name_(std::string("libcurl (") + from + ")") {
    // Resolved once: where the trust store is is a property of the device, not
    // of a request.
    if (const char* ca = std::getenv("DS_CHEEVOS_CAINFO")) {
      if (*ca) { ca_ = ca; return; }
    }
    for (const char* p : kCaPaths) {
      if (exists(p)) { ca_ = p; break; }
    }
  }
  ~CurlBackend() override {
    // The library is deliberately left open. curl_global_cleanup is not called
    // either: OpenSSL and curl both register process-wide state, and tearing it
    // down while a worker may still be unwinding is a crash-at-exit waiting to
    // happen. A leaked handle at process end costs nothing.
    (void)handle_;
  }

  const char* name() const override { return name_.c_str(); }

  Response perform(const Request& req, const char* user_agent) override {
    Response res;
    CURL* c = api_.easy_init();
    if (!c) { res.error = "curl_easy_init failed"; return res; }

    Sink sink{&res.body};
    api_.easy_setopt(c, CURLOPT_URL, req.url.c_str());
    api_.easy_setopt(c, CURLOPT_WRITEFUNCTION, &write_cb);
    api_.easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    api_.easy_setopt(c, CURLOPT_USERAGENT, user_agent);
    api_.easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    api_.easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");     // whatever this curl supports
    // No signals: curl's default alarm-based DNS timeout is not thread-safe,
    // and this runs on a worker. The *_MS timeouts below work without it.
    api_.easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    api_.easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    api_.easy_setopt(c, CURLOPT_TIMEOUT_MS, 30000L);
    // Set only when a store was actually found; otherwise curl uses the one it
    // was built against, which is right wherever the CFW built its own.
    if (!ca_.empty()) api_.easy_setopt(c, CURLOPT_CAINFO, ca_.c_str());

    curl_slist* headers = nullptr;
    if (!req.body.empty()) {
      api_.easy_setopt(c, CURLOPT_POST, 1L);
      api_.easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
      api_.easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
      if (!req.content_type.empty()) {
        const std::string h = "Content-Type: " + req.content_type;
        headers = api_.slist_append(headers, h.c_str());
      }
      // curl would otherwise announce Expect: 100-continue on a large body and
      // wait a second for a server that never answers it.
      headers = api_.slist_append(headers, "Expect:");
      if (headers) api_.easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    }

    const int rc = api_.easy_perform(c);
    if (rc == CURLE_OK) {
      long status = 0;
      api_.easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
      res.status = static_cast<int>(status);
    } else if (sink.overflowed) {
      res.error = "response larger than 8 MB";
    } else {
      // status stays 0, which is what tells rcheevos to retry this later.
      const char* m = api_.easy_strerror ? api_.easy_strerror(rc) : nullptr;
      res.error = m ? m : ("curl error " + std::to_string(rc));
    }

    if (headers) api_.slist_free_all(headers);
    api_.easy_cleanup(c);
    return res;
  }

private:
  void* handle_;
  Api api_;
  std::string name_, ca_;
};

} // namespace

std::unique_ptr<Backend> make_curl_backend(std::string& err) {
  err.clear();

  // The soname, and nothing else. Deliberately no list of places a library
  // might be hiding: on a CFW the launcher decides what this process can see.
  // spruce's dsperate_functions.sh exports
  // LD_LIBRARY_PATH="$EMU_DIR/lib64:$LD_LIBRARY_PATH", so a libcurl.so.4
  // dropped into the emulator's own lib directory is found by this line with
  // no help from us. Hunting through other applications' bundles would work
  // until any of them moved, and would be us borrowing a library nobody
  // offered.
  //
  // DS_CHEEVOS_LIBCURL is the escape hatch for a firmware that keeps it
  // somewhere the loader cannot see.
  const char* explicit_path = std::getenv("DS_CHEEVOS_LIBCURL");
  if (explicit_path && !*explicit_path) explicit_path = nullptr;
  const char* what = explicit_path ? explicit_path : "libcurl.so.4";
  void* h = dlopen(what, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char* e = dlerror();
    err = e ? e : "libcurl.so.4 not found";
    return nullptr;
  }

  Api api{};
  struct Sym { const char* name; void** slot; };
  const Sym syms[] = {
    {"curl_global_init",   reinterpret_cast<void**>(&api.global_init)},
    {"curl_easy_init",     reinterpret_cast<void**>(&api.easy_init)},
    {"curl_easy_setopt",   reinterpret_cast<void**>(&api.easy_setopt)},
    {"curl_easy_perform",  reinterpret_cast<void**>(&api.easy_perform)},
    {"curl_easy_getinfo",  reinterpret_cast<void**>(&api.easy_getinfo)},
    {"curl_easy_cleanup",  reinterpret_cast<void**>(&api.easy_cleanup)},
    {"curl_easy_strerror", reinterpret_cast<void**>(&api.easy_strerror)},
    {"curl_slist_append",  reinterpret_cast<void**>(&api.slist_append)},
    {"curl_slist_free_all", reinterpret_cast<void**>(&api.slist_free_all)},
  };
  for (const Sym& s : syms) {
    *s.slot = dlsym(h, s.name);
    if (!*s.slot) {
      err = std::string("libcurl.so.4 has no ") + s.name;
      dlclose(h);
      return nullptr;
    }
  }

  // Once, before any easy handle exists. curl does this lazily and
  // non-thread-safely if we do not, and the first request is on a worker.
  if (api.global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
    err = "curl_global_init failed";
    dlclose(h);
    return nullptr;
  }
  return std::make_unique<CurlBackend>(h, api, what);
}

} // namespace ds::cheevos
