// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/i18n/i18n.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace ds::i18n {

// Every po/*.po of the source tree, linked in (CMake writes builtin_po.cpp).
namespace detail {
struct BuiltinPo { const char* code; const char* begin; const char* end; };
extern const BuiltinPo kBuiltinPo[];
extern const unsigned kBuiltinPoCount;
}  // namespace detail

namespace {

constexpr const char* kNameContext = "language name";
constexpr const char* kNameId = "ENGLISH";

// ---- the .po reader ---------------------------------------------------------

// The quoted string at s[i] (s[i] is the opening quote); i ends past the
// closing quote. False on an unterminated string.
bool read_quoted(const std::string& s, size_t& i, std::string& out) {
  ++i;
  while (i < s.size()) {
    const char c = s[i++];
    if (c == '"') return true;
    if (c != '\\') { out += c; continue; }
    if (i >= s.size()) return false;
    const char e = s[i++];
    switch (e) {
    case 'n': out += '\n'; break;
    case 't': out += '\t'; break;
    case 'r': out += '\r'; break;
    case 'a': out += '\a'; break;
    case 'x': {   // \xHH, up to two digits
      int v = 0, k = 0;
      for (; k < 2 && i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i])); ++k, ++i)
        v = v * 16 + (std::isdigit(static_cast<unsigned char>(s[i])) ? s[i] - '0' : (std::tolower(static_cast<unsigned char>(s[i])) - 'a' + 10));
      if (k == 0) return false;
      out += static_cast<char>(v);
      break;
    }
    default:
      if (e >= '0' && e <= '7') {   // \ooo, up to three digits (the extractor writes control bytes so)
        int v = e - '0';
        for (int k = 1; k < 3 && i < s.size() && s[i] >= '0' && s[i] <= '7'; ++k, ++i) v = v * 8 + (s[i] - '0');
        out += static_cast<char>(v);
      } else {
        out += e;   // \" \\ and anything else: the character itself
      }
      break;
    }
  }
  return false;
}

struct Entry {
  bool has_ctx = false, fuzzy = false, plural = false;
  std::string ctx, id, str;
  bool any() const { return has_ctx || !id.empty() || !str.empty() || plural; }
};

}  // namespace

std::string Catalog::key(const char* context, const char* english) {
  if (!context) return english;
  std::string k = context;
  k += '\x04';   // gettext's own separator
  k += english;
  return k;
}

unsigned placeholders(const std::string& s) {
  unsigned m = 0;
  for (size_t i = 0; i + 2 < s.size(); ++i)
    if (s[i] == '{' && s[i + 1] >= '1' && s[i + 1] <= '9' && s[i + 2] == '}') m |= 1u << (s[i + 1] - '1');
  return m;
}

std::string substitute(const std::string& p, std::initializer_list<std::string> args) {
  std::string out;
  out.reserve(p.size());
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i] == '{' && i + 2 < p.size() && p[i + 1] >= '1' && p[i + 1] <= '9' && p[i + 2] == '}') {
      const size_t n = static_cast<size_t>(p[i + 1] - '1');
      if (n < args.size()) out += *(args.begin() + n);
      i += 2;
      continue;
    }
    out += p[i];
  }
  return out;
}

bool parse_po(const std::string& text, Catalog& out, std::string& error, std::vector<std::string>* dropped) {
  Entry e;
  bool pending_fuzzy = false;
  std::string* target = nullptr;   // what a continuation line appends to
  bool str_seen = false;           // the entry has had its msgstr: the next keyword starts a new one
  int str_index = -1;              // msgstr[N]: only N == 0 is kept
  std::string skipped;             // msgid_plural, msgstr[1..]: read, not kept

  auto flush = [&] {
    if (e.any() && !e.fuzzy && !e.str.empty() && !e.id.empty()) {
      if (placeholders(e.id) != placeholders(e.str)) {
        if (dropped) dropped->push_back(e.id);
      } else {
        if (e.has_ctx && e.ctx == kNameContext && e.id == kNameId) out.name = e.str;
        out.entries[Catalog::key(e.has_ctx ? e.ctx.c_str() : nullptr, e.id.c_str())] = e.str;
      }
    }
    e = Entry{};
    target = nullptr;
    str_seen = false;
    str_index = -1;
  };

  std::istringstream in(text);
  std::string line;
  int n = 0;
  auto fail = [&](const char* what) {
    error = "line " + std::to_string(n) + ": " + what;
    return false;
  };
  while (std::getline(in, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) continue;
    if (line[i] == '#') {
      // "#, fuzzy, c-format": flags for the entry that follows. "#~" is an
      // obsolete entry, and like every other comment, ignored.
      if (line.compare(i, 2, "#,") == 0 && line.find("fuzzy", i) != std::string::npos) {
        if (str_seen) flush();
        pending_fuzzy = true;
      }
      continue;
    }
    std::string* dest = nullptr;
    if (line[i] == '"') {
      if (!target) return fail("a string with no keyword before it");
      dest = target;
    } else {
      const size_t kw_end = line.find_first_of(" \t\"", i);
      const std::string kw = line.substr(i, kw_end == std::string::npos ? std::string::npos : kw_end - i);
      if (kw == "msgctxt") {
        if (str_seen || e.any()) flush();
        e.has_ctx = true; dest = &e.ctx;
      } else if (kw == "msgid") {
        if (str_seen || (e.any() && !(e.has_ctx && e.id.empty() && e.str.empty()))) flush();
        dest = &e.id;
      } else if (kw == "msgid_plural") {
        e.plural = true;
        dest = &skipped;   // the plural's English is not a key
      } else if (kw == "msgstr" || kw.compare(0, 7, "msgstr[") == 0) {
        str_index = kw == "msgstr" ? 0 : std::atoi(kw.c_str() + 7);
        str_seen = true;
        dest = str_index == 0 ? &e.str : &skipped;
      } else {
        return fail("unknown keyword");
      }
      if (pending_fuzzy) { e.fuzzy = true; pending_fuzzy = false; }
      i = kw_end == std::string::npos ? line.size() : line.find('"', kw_end);
      if (i == std::string::npos) return fail("a keyword with no string");
      target = dest;
    }
    std::string s;
    if (!read_quoted(line, i, s)) return fail("an unterminated string");
    if (line.find_first_not_of(" \t", i) != std::string::npos) return fail("text after a string");
    *dest += s;
  }
  flush();
  return true;
}

std::string normalise(const std::string& code) {
  std::string c = code.substr(0, code.find_first_of(".@"));
  if (c.empty() || c == "C" || c == "POSIX") return "";
  for (char& ch : c) if (ch == '-') ch = '_';
  const size_t us = c.find('_');
  for (size_t k = 0; k < c.size() && k < us; ++k) c[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(c[k])));
  if (us != std::string::npos) {
    std::string region = c.substr(us + 1);
    for (size_t k = 0; k < region.size(); ++k) {
      const unsigned char ch = static_cast<unsigned char>(region[k]);
      // A script subtag ("Hans") is title case, a region ("CN") upper case.
      region[k] = static_cast<char>(region.size() == 4 && k > 0 ? std::tolower(ch) : std::toupper(ch));
    }
    c = c.substr(0, us + 1) + region;
  }
  return c;
}

std::string from_environment() {
  if (const char* l = std::getenv("LANGUAGE"); l && *l) {
    std::string list = l;
    size_t a = 0;
    while (a <= list.size()) {
      const size_t b = list.find(':', a);
      const std::string c = normalise(list.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (!c.empty()) return c;
      if (b == std::string::npos) break;
      a = b + 1;
    }
  }
  for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"})
    if (const char* v = std::getenv(var); v && *v) {
      const std::string c = normalise(v);
      if (!c.empty()) return c;
    }
  return "";
}

// ---- the languages ------------------------------------------------------------

namespace {

struct Source {
  std::string code;
  bool user = false;
  std::string path;                        // user files
  const char* begin = nullptr, *end = nullptr;   // built-in ones
};

std::mutex g_mu;
std::string g_user_dir;
std::vector<std::unique_ptr<Catalog>> g_keep;          // never freed: tr()'s pointers outlive a switch
std::map<std::string, const Catalog*> g_loaded;       // by source ("b:zh_CN" / "u:<path>"); null: failed
std::atomic<const Catalog*> g_current{nullptr};
std::string g_current_code = "en";

std::string lang_of(const std::string& code) { return code.substr(0, code.find('_')); }

// Built-in, then the user's: a user file replaces a built-in of the same code.
std::vector<Source> sources() {
  std::map<std::string, Source> by_code;
  for (unsigned k = 0; k < detail::kBuiltinPoCount; ++k) {
    const detail::BuiltinPo& b = detail::kBuiltinPo[k];
    Source s; s.code = normalise(b.code); s.begin = b.begin; s.end = b.end;
    if (!s.code.empty()) by_code[s.code] = s;
  }
  if (!g_user_dir.empty())
    if (DIR* d = ::opendir(g_user_dir.c_str())) {
      while (const dirent* ent = ::readdir(d)) {
        const std::string f = ent->d_name;
        if (f.size() <= 3 || f.compare(f.size() - 3, 3, ".po") != 0) continue;
        Source s; s.code = normalise(f.substr(0, f.size() - 3)); s.user = true; s.path = g_user_dir + "/" + f;
        if (!s.code.empty()) by_code[s.code] = s;
      }
      ::closedir(d);
    }
  std::vector<Source> out;
  for (auto& [code, s] : by_code) out.push_back(s);
  return out;
}

const Catalog* load(const Source& s) {
  const std::string id = s.user ? "u:" + s.path : "b:" + s.code;
  if (auto it = g_loaded.find(id); it != g_loaded.end()) return it->second;
  std::string text;
  if (s.user) {
    std::ifstream f(s.path, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    text = ss.str();
  } else {
    text.assign(s.begin, s.end);
  }
  auto cat = std::make_unique<Catalog>();
  std::string err;
  std::vector<std::string> dropped;
  const char* where = s.user ? s.path.c_str() : s.code.c_str();
  if (!parse_po(text, *cat, err, &dropped)) {
    std::fprintf(stderr, "lang: %s: %s; not loaded\n", where, err.c_str());
    g_loaded[id] = nullptr;
    return nullptr;
  }
  for (const std::string& d : dropped)
    std::fprintf(stderr, "lang: %s: \"%s\": its translation's {n} placeholders differ; left in English\n", where, d.c_str());
  const Catalog* p = cat.get();
  g_keep.push_back(std::move(cat));
  g_loaded[id] = p;
  return p;
}

}  // namespace

const char* tr(const char* english) {
  const Catalog* c = g_current.load(std::memory_order_acquire);
  if (!c) return english;
  const auto it = c->entries.find(english);
  return it == c->entries.end() ? english : it->second.c_str();
}

const char* trc(const char* context, const char* english) {
  const Catalog* c = g_current.load(std::memory_order_acquire);
  if (!c) return english;
  const auto it = c->entries.find(Catalog::key(context, english));
  return it == c->entries.end() ? english : it->second.c_str();
}

std::string trf(const char* english, std::initializer_list<std::string> args) {
  return substitute(tr(english), args);
}

std::vector<Language> languages() {
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<Language> out{{"en", kNameId, false}};
  for (const Source& s : sources())
    if (const Catalog* c = load(s)) out.push_back({s.code, c->name.empty() ? s.code : c->name, s.user});
  return out;
}

bool set_language(const std::string& code) {
  std::lock_guard<std::mutex> lock(g_mu);
  const std::string want = code == "auto" ? from_environment() : normalise(code);
  const std::vector<Source> srcs = sources();
  const Source* pick = nullptr;
  if (!want.empty()) {
    for (const Source& s : srcs) if (s.code == want) { pick = &s; break; }                      // zh_CN
    if (!pick) for (const Source& s : srcs) if (s.code == lang_of(want)) { pick = &s; break; }   // zh_TW -> zh
    if (!pick) for (const Source& s : srcs) if (lang_of(s.code) == lang_of(want)) { pick = &s; break; }   // zh -> zh_CN
  }
  const Catalog* c = pick ? load(*pick) : nullptr;
  g_current.store(c, std::memory_order_release);
  g_current_code = c ? pick->code : "en";
  return c != nullptr || want.empty() || lang_of(want) == "en";
}

std::string language() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_current_code;
}

void set_user_dir(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_user_dir = dir;
}

}  // namespace ds::i18n
