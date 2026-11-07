// SPDX-License-Identifier: GPL-3.0-or-later
// The frontend's translations (frontend/i18n): the .po reader, placeholders,
// locale names, and switching between built-in and user catalogs. Also loads
// every built-in catalog, so a broken po/*.po fails here rather than on a
// device.
#include "frontend/i18n/i18n.h"
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace i18n = ds::i18n;

namespace {

void test_parse() {
  const char* po =
      "# A comment\n"
      "msgid \"\"\n"
      "msgstr \"\"\n"
      "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
      "\"Language: zh_CN\\n\"\n"
      "\n"
      "msgctxt \"language name\"\n"
      "msgid \"ENGLISH\"\n"
      "msgstr \"\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87\"\n"   // 简体中文
      "\n"
      "#: src/frontend/sdl/menu.cpp:200\n"
      "msgid \"SAVE STATE\"\n"
      "msgstr \"\xE4\xBF\x9D\xE5\xAD\x98\xE7\x8A\xB6\xE6\x80\x81\"\n"   // 保存状态
      "\n"
      "msgid \"\"\n"
      "\"SPLIT \"\n"
      "\"ACROSS\"\n"
      "msgstr \"\"\n"
      "\"JOINED\"\n"
      "\n"
      "msgid \"ESCAPES\"\n"
      "msgstr \"A\\\"B\\\\C\\nD\"\n"
      "\n"
      "msgid \"\\001 A\"\n"
      "msgstr \"\\x01\\x41\\1012\"\n"
      "\n"
      "#, fuzzy\n"
      "msgid \"FUZZY\"\n"
      "msgstr \"IGNORED\"\n"
      "\n"
      "msgid \"UNTRANSLATED\"\n"
      "msgstr \"\"\n"
      "\n"
      "msgctxt \"switch\"\n"
      "msgid \"ON\"\n"
      "msgstr \"\xE5\xBC\x80\"\n"                                          // 开
      "\n"
      "msgid \"ON\"\n"
      "msgstr \"PLAIN ON\"\n"
      "\n"
      "msgid \"SLOT < {1} >\"\n"
      "msgstr \"< {1} > SLOT\"\n"
      "\n"
      "msgid \"{1} OF {2}\"\n"
      "msgstr \"{1} ONLY\"\n"                                               // a placeholder lost: dropped
      "\n"
      "msgid \"ONE FILE\"\n"
      "msgid_plural \"{1} FILES\"\n"
      "msgstr[0] \"\"\n"
      "\"FILES\"\n"
      "msgstr[1] \"OTHER\"\n"
      "\n"
      "#~ msgid \"OBSOLETE\"\n"
      "#~ msgstr \"GONE\"\n"
      "msgid \"LAST\"\r\n"
      "msgstr \"WITH CRLF\"\r\n";
  i18n::Catalog c;
  std::string err;
  std::vector<std::string> dropped;
  CHECK(i18n::parse_po(po, c, err, &dropped));
  CHECK(c.name == "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87");
  auto get = [&](const char* ctx, const char* id) -> std::string {
    const auto it = c.entries.find(i18n::Catalog::key(ctx, id));
    return it == c.entries.end() ? "<missing>" : it->second;
  };
  CHECK(get(nullptr, "SAVE STATE") == "\xE4\xBF\x9D\xE5\xAD\x98\xE7\x8A\xB6\xE6\x80\x81");
  CHECK(get(nullptr, "SPLIT ACROSS") == "JOINED");
  CHECK(get(nullptr, "ESCAPES") == "A\"B\\C\nD");
  CHECK(get(nullptr, "\x01 A") == std::string("\x01" "AA2"));   // \001, \x01, \x41, \101 then '2'

  CHECK(get(nullptr, "FUZZY") == "<missing>");
  CHECK(get(nullptr, "UNTRANSLATED") == "<missing>");
  CHECK(get("switch", "ON") == "\xE5\xBC\x80");
  CHECK(get(nullptr, "ON") == "PLAIN ON");
  CHECK(get(nullptr, "SLOT < {1} >") == "< {1} > SLOT");
  CHECK(get(nullptr, "{1} OF {2}") == "<missing>");
  CHECK(dropped.size() == 1 && dropped[0] == "{1} OF {2}");
  CHECK(get(nullptr, "ONE FILE") == "FILES");
  CHECK(get(nullptr, "OBSOLETE") == "<missing>");
  CHECK(get(nullptr, "LAST") == "WITH CRLF");
  CHECK(get(nullptr, "") == "<missing>");   // the header is not an entry

  // Syntax errors say where.
  const char* bad[] = {
    "msgid \"A\"\nmsgstr \"B\n",          // unterminated
    "\"orphan\"\n",                       // a string with no keyword
    "msgid \"A\"\nmsgfoo \"B\"\n",        // unknown keyword
    "msgid \"A\" junk\nmsgstr \"B\"\n",   // text after a string
  };
  for (const char* b : bad) {
    i18n::Catalog x;
    std::string e;
    CHECK(!i18n::parse_po(b, x, e));
    CHECK(e.compare(0, 5, "line ") == 0);
  }
}

void test_placeholders() {
  CHECK(i18n::placeholders("NONE") == 0);
  CHECK(i18n::placeholders("{1} AND {3}") == 0b101);
  CHECK(i18n::placeholders("{0} {x} {10}") == 0);   // only {1}..{9}, each a single digit
  CHECK(i18n::substitute("SLOT < {1} >", {"3"}) == "SLOT < 3 >");
  CHECK(i18n::substitute("{2} BEFORE {1}", {"A", "B"}) == "B BEFORE A");
  CHECK(i18n::substitute("{1}{1}", {"X"}) == "XX");
  CHECK(i18n::substitute("{2} MISSING", {"A"}) == " MISSING");
  CHECK(i18n::substitute("{ LITERAL } {x}", {"A"}) == "{ LITERAL } {x}");
}

void test_normalise() {
  CHECK(i18n::normalise("zh-CN") == "zh_CN");
  CHECK(i18n::normalise("zh-cn") == "zh_CN");
  CHECK(i18n::normalise("ZH_cn.UTF-8") == "zh_CN");
  CHECK(i18n::normalise("de_DE@euro") == "de_DE");
  CHECK(i18n::normalise("zh-hans") == "zh_Hans");
  CHECK(i18n::normalise("fr") == "fr");
  CHECK(i18n::normalise("C") == "");
  CHECK(i18n::normalise("POSIX") == "");
  CHECK(i18n::normalise("C.UTF-8") == "");
  CHECK(i18n::normalise("") == "");

  unsetenv("LANGUAGE"); unsetenv("LC_ALL"); unsetenv("LC_MESSAGES"); unsetenv("LANG");
  CHECK(i18n::from_environment() == "");
  setenv("LANG", "C.UTF-8", 1);
  CHECK(i18n::from_environment() == "");
  setenv("LANG", "de_DE.UTF-8", 1);
  CHECK(i18n::from_environment() == "de_DE");
  setenv("LC_ALL", "fr_FR.UTF-8", 1);
  CHECK(i18n::from_environment() == "fr_FR");
  setenv("LANGUAGE", ":ja:en", 1);   // gettext's list, first wins
  CHECK(i18n::from_environment() == "ja");
  unsetenv("LANGUAGE"); unsetenv("LC_ALL"); unsetenv("LANG");
}

void write(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f);
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}

// Built-in catalogs all load, and user files add and replace languages.
void test_switching() {
  // Every built-in catalog parses (a syntax error would leave it out) and names itself.
  const auto builtin = i18n::languages();
  CHECK(!builtin.empty() && builtin[0].code == "en");
  for (const auto& l : builtin) CHECK(!l.user);

  char tmpl[] = "/tmp/dsperate_i18n_XXXXXX";
  const char* d = mkdtemp(tmpl);
  CHECK(d);
  const std::string dir = d;
  write(dir + "/fr.po",
        "msgctxt \"language name\"\nmsgid \"ENGLISH\"\nmsgstr \"FRAN\xC3\x87" "AIS\"\n\n"
        "msgid \"SAVE STATE\"\nmsgstr \"SAUVEGARDER\"\n\n"
        "msgid \"SLOT < {1} >\"\nmsgstr \"EMPLACEMENT < {1} >\"\n");
  write(dir + "/de-DE.po", "msgid \"SAVE STATE\"\nmsgstr \"SPEICHERN\"\n");   // no name: listed by code
  write(dir + "/broken.po", "msgid \"A\nmsgstr \"B\"\n");                      // fails to parse: not listed
  write(dir + "/notes.txt", "not a catalog");
  i18n::set_user_dir(dir);

  const auto langs = i18n::languages();
  auto find = [&](const char* code) -> const i18n::Language* {
    for (const auto& l : langs) if (l.code == code) return &l;
    return nullptr;
  };
  CHECK(find("fr") && find("fr")->user && find("fr")->name == "FRAN\xC3\x87" "AIS");
  CHECK(find("de_DE") && find("de_DE")->name == "de_DE");
  CHECK(!find("broken"));

  CHECK(std::strcmp(i18n::tr("SAVE STATE"), "SAVE STATE") == 0);   // English until asked
  CHECK(i18n::language() == "en");

  CHECK(i18n::set_language("fr"));
  CHECK(i18n::language() == "fr");
  const char* kept = i18n::tr("SAVE STATE");
  CHECK(std::strcmp(kept, "SAUVEGARDER") == 0);
  CHECK(std::strcmp(i18n::tr("NOT IN THE CATALOG"), "NOT IN THE CATALOG") == 0);
  CHECK(i18n::trf("SLOT < {1} >", {"4"}) == "EMPLACEMENT < 4 >");
  CHECK(i18n::trf("{1} UNTRANSLATED", {"X"}) == "X UNTRANSLATED");

  CHECK(i18n::set_language("fr-CA"));   // a region falls back to its language
  CHECK(i18n::language() == "fr");
  CHECK(i18n::set_language("de"));      // a language picks its region
  CHECK(i18n::language() == "de_DE");
  CHECK(std::strcmp(i18n::tr("SAVE STATE"), "SPEICHERN") == 0);
  CHECK(std::strcmp(kept, "SAUVEGARDER") == 0);   // a pointer from before the switch still reads

  setenv("LANG", "fr_FR.UTF-8", 1);
  CHECK(i18n::set_language("auto"));
  CHECK(i18n::language() == "fr");
  unsetenv("LANG");
  CHECK(i18n::set_language("auto"));    // nothing in the environment: English
  CHECK(i18n::language() == "en");

  CHECK(!i18n::set_language("xx"));     // unknown: English, and says so
  CHECK(i18n::language() == "en");
  CHECK(std::strcmp(i18n::tr("SAVE STATE"), "SAVE STATE") == 0);
  CHECK(i18n::set_language("en_GB"));   // English in any region needs no catalog
  CHECK(i18n::set_language(""));

  // A user file replaces the built-in of the same code.
  for (const auto& l : builtin) {
    if (l.code == "en") continue;
    write(dir + "/" + l.code + ".po", "msgid \"SAVE STATE\"\nmsgstr \"USER COPY\"\n");
    CHECK(i18n::set_language(l.code));
    CHECK(std::strcmp(i18n::tr("SAVE STATE"), "USER COPY") == 0);
    break;
  }

  i18n::set_user_dir("");
  CHECK(i18n::set_language("en"));
  std::system(("rm -rf '" + dir + "'").c_str());
}

}  // namespace

int main() {
  test_parse();
  test_placeholders();
  test_normalise();
  test_switching();
  std::puts("i18n: ok");
  return 0;
}
