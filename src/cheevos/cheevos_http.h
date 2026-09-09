// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The HTTP transport RetroAchievements is reached through
// (docs/retroachievements-scoping.md, phase 3). rcheevos has no socket code at
// all -- it hands us a URL and a body and wants a status and a response -- and
// DSperate had no networking before this, so this is all of it.
//
// There is one backend: libcurl, opened with dlopen rather than linked. The
// reasoning is `wl_dyn.h`'s, and it applies more strongly here because
// achievements are optional: a device without libcurl must lose this feature
// and nothing else, never fail to start. The CFW provides the library and the
// CA store -- ROCKNIX ships curl 8.21 over OpenSSL 3 and a real trust bundle,
// and validates RetroAchievements' chain with no help from us -- so nothing is
// vendored and no certificates ship.
//
// It is an interface with one implementation on purpose. The two `-static`
// handheld tiers cannot dlopen at all, and reaching them later means either
// linking libcurl in or shelling out to a `curl` binary (which is what
// drastic-nano ships, so it is known to work). Both are a new Backend and
// nothing else.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace ds::cheevos {

struct Request {
  std::string url;
  std::string body;           // empty for GET, form-encoded for POST
  std::string content_type;   // only meaningful with a body
};

struct Response {
  // The HTTP status, or 0 if the request never got one (DNS, connect, TLS).
  // rcheevos treats 0 as a retryable transport failure and requeues, which is
  // what we want for a handheld that wandered out of Wi-Fi range.
  int status = 0;
  std::string body;
  std::string error;          // transport-level reason when status == 0
};

// A blocking HTTP client. Call sites are on the worker thread only.
class Backend {
public:
  virtual ~Backend() = default;
  virtual Response perform(const Request& req, const char* user_agent) = 0;
  virtual const char* name() const = 0;
};

// libcurl via dlopen. Null with a reason in `err` when the library is missing
// or too old to have what we call -- which is a normal outcome on some devices,
// not an error to shout about.
std::unique_ptr<Backend> make_curl_backend(std::string& err);

} // namespace ds::cheevos
