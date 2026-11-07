# Translating DSperate

DSperate's menus, notices and messages can be translated. A translation is one
[gettext](https://www.gnu.org/software/gettext/manual/html_node/PO-Files.html)
`.po` file, so any PO editor works (Poedit, Lokalize, Weblate, a text editor).

## Files

| File | What it is |
| --- | --- |
| `po/dsperate.pot` | The template: every string the program shows. Generated, never edited by hand. |
| `po/<code>.po` | One language. Every `.po` here is built into the program. |

`<code>` is the gettext form: language, then region if it matters, joined by
an underscore. Simplified Chinese is `zh_CN`, Brazilian Portuguese `pt_BR`,
French with no region `fr`. The config also accepts `zh-CN`.

## Starting a language

```sh
msginit --no-translator --locale=zh_CN --input=po/dsperate.pot --output=po/zh_CN.po
```

Or copy `po/dsperate.pot` to `po/zh_CN.po` and fill in the header's
`Language: zh_CN`.

Translate the first entry, `ENGLISH` in the context `language name`, as the
language's own name as its speakers write it (`简体中文`). That is what the
MENU LANGUAGE setting lists it as.

## Rules for the strings

- **The English is the key.** Leave `msgid` alone and write the translation in
  `msgstr`. An empty `msgstr` shows the English, so a partial translation is
  fine.
- **Keep every `{1}`, `{2}`... placeholder.** The program fills them in (a slot
  number, a count, a game's name). They can move to wherever your grammar puts
  them (`"SLOT < {1} >"` can become `"< {1} > 号存档"`), but none can be dropped
  or added. A string whose placeholders don't match is ignored and shows in
  English, with a warning on the console.
- **A placeholder can be a button**: a face-button icon with a controller, a DS
  letter (A, X...) on the keyboard. The `#.` note above such a string says so;
  keep each one beside the word it labels (`{1} DONE`).
- **Case does not matter.** The menu font has capitals only and folds lower
  case onto them, so write the case your language normally uses.
- **Length.** The menu is a fixed-width panel. Long rows scroll when selected
  and other text may be cut short, so a translation near the English length
  reads best. A CJK character is about a third wider than a Latin letter.
- **Context.** A comment starting `#.` above a string explains where it is
  shown. A `msgctxt` line marks a string whose English means something else
  elsewhere (`HOST` in the context `wifi.dns` is the system's own DNS, not
  hosting a session): translate each as its context says.
- **Lines marked `#, fuzzy`** are treated as untranslated. Check them and remove
  the mark.

These stay in English and are not in the template: command-line help, console
logs, config keys and values, game titles, cheat names and RetroAchievements
text (that comes from the server).

## Trying it without building

Put the file in a `lang` folder next to the config file, and DSperate loads it
on the next start:

| Device | Folder |
| --- | --- |
| Most Linux systems | `~/.config/dsperate/lang/zh_CN.po` |
| ROCKNIX | `/storage/.config/dsperate/lang/zh_CN.po` |
| With `--config F` | `lang/` next to `F` |

Then pick the language under Options → Emulation → MENU LANGUAGE, or set it in
the config:

```ini
[ui]
language = zh_CN
```

A file there with a built-in language's code replaces the built-in one, so a
fix can be tried on an installed copy. A file that fails to load is named on
the console with the line it stopped at.

## Keeping a translation up to date

When the program's strings change, the template is regenerated and each
translation brought up to date with it:

```sh
python3 tools/i18n_extract.py                     # rewrites po/dsperate.pot
msgmerge --update --no-fuzzy-matching po/zh_CN.po po/dsperate.pot
```

New strings then appear untranslated, and strings that no longer exist turn
into `#~` comments, which can be deleted.

## Sending it

Open a pull request that adds or updates `po/<code>.po`. `ctest -R i18n` loads
every built-in translation and fails on a broken one.

## For developers

Code that shows text marks it for translation:

| In the code | Use |
| --- | --- |
| `tr("SAVE STATE")` | A string, translated where it is drawn. |
| `trf("SLOT < {1} >", {std::to_string(n)})` | A format. Never build a sentence from pieces, or a translation cannot reorder it. |
| `trc("switch", "ON")` | A string that needs a context to translate. |
| `N_("SAVE STATE")`, `NC_("switch", "ON")` | A table entry: marked, not translated where it stands; `tr()` it where it is drawn. A settings choice marked `NC_` takes the setting's key as its context (`NC_("wifi.dns", "HOST")`). |
| `trf("{1} DONE", {"\x03"})` | A face-button icon goes in as an argument, never in the string: gettext reserves `\004`. |
| `// TRANSLATORS: ...` | On the line above a call: a note in the template. |

Run `python3 tools/i18n_extract.py` after changing any of them and commit
`po/dsperate.pot`; CI fails if it is out of date. The mechanism is
`src/frontend/i18n/i18n.h`.
