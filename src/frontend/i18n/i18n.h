// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The frontend's translations. The English text is the key: tr("SAVE STATE")
// returns the current language's string, or the English one when there is no
// translation, so a missing or half-done language never breaks a page.
//
// Catalogs are gettext .po files: every po/<code>.po in the source tree is
// built in, and a <code>.po in the user's lang directory (set_user_dir; the
// SDL frontend uses <config dir>/lang) adds a language or replaces a built-in
// one without a rebuild. Only the parts of the format DSperate uses are read:
// msgctxt, msgid, msgstr, continuation lines, the usual escapes; fuzzy and
// untranslated entries count as missing. docs/translating.md is the guide.
//
// Formats use numbered placeholders, {1} to {9}, which a translation may
// reorder: trf("SLOT < {1} >", {std::to_string(n)}). An entry whose
// placeholders differ from its msgid's is dropped as the catalog loads.
//
// Each catalog names itself by translating "ENGLISH" in the context
// "language name" ("简体中文"), which is what the language picker lists.
#pragma once
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

// Marks a literal for translation without translating it where it stands (a
// table entry, translated with tr() where it is drawn). tools/i18n_extract.py
// collects N_, NC_, tr, trc and trf literals into po/dsperate.pot.
#define N_(s) s
#define NC_(ctx, s) s

namespace ds::i18n {

// The current language's string for `english`, else `english` itself. The
// pointer stays valid for the life of the process, across language changes.
const char* tr(const char* english);
// The same, for a string that needs a context to translate unambiguously
// ("ON" as a switch vs "ON" as a count's unit).
const char* trc(const char* context, const char* english);
// tr(), then {1}..{9} replaced by `args`.
std::string trf(const char* english, std::initializer_list<std::string> args);

struct Language {
  std::string code;   // "zh_CN"; "en" for the built-in English
  std::string name;   // in itself: "简体中文"; "ENGLISH" if the catalog does not say
  bool user = false;  // from the user's lang directory rather than built in
};
// English first, then every catalog found, by code. A user file with a
// built-in's code is listed once, as the user's.
std::vector<Language> languages();
// Switches language: a code ("zh_CN", "zh-CN", "zh"), "auto" for the
// environment's (LANGUAGE, LC_ALL, LC_MESSAGES, LANG), or "en"/"" for English.
// A region falls back to its language (zh_TW -> zh) and a bare language to
// its first region (zh -> zh_CN). False, leaving English, when nothing
// matches. Safe against concurrent tr() calls.
bool set_language(const std::string& code);
// The code in force: "en" when untranslated.
std::string language();
// Where user catalogs live; read on the next languages()/set_language().
void set_user_dir(const std::string& dir);

// ---- below: the pieces, for tests and tools ----

struct Catalog {
  std::unordered_map<std::string, std::string> entries;   // key(): translation
  std::string name;
  static std::string key(const char* context, const char* english);
};
// Parses a .po file's text. False and `error` ("line 12: ...") on a syntax
// error. Entries whose placeholders don't match their msgid are left out and
// their msgids listed in `dropped`.
bool parse_po(const std::string& text, Catalog& out, std::string& error,
              std::vector<std::string>* dropped = nullptr);
// "zh-cn", "zh_CN.UTF-8", "zh_CN@x" -> "zh_CN"; "C", "POSIX", "" -> "".
std::string normalise(const std::string& code);
// The first usable code from the environment, normalised, or "".
std::string from_environment();
// {1}..{9} in `pattern` replaced by args (a missing one by nothing); any other
// brace is literal.
std::string substitute(const std::string& pattern, std::initializer_list<std::string> args);
// Which {1}..{9} a string uses, as a bit mask (bit n-1 for {n}).
unsigned placeholders(const std::string& s);

}  // namespace ds::i18n
