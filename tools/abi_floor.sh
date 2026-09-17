#!/bin/bash
# Assert the runtime floor of a built binary: the highest symbol versions it
# imports, and so the oldest system it will load on.
#
#   tools/abi_floor.sh <binary> <max GLIBC> [max GLIBCXX|none] [readelf]
#
# Why this is a gate and not a note: the floor is a property nothing in the
# build declares. It moves when a single call reaches a newer symbol -- one
# std::pow put the whole aarch64 binary at GLIBC_2.29 -- and nothing fails,
# nothing warns; the tarball just stops loading on some devices and the report
# arrives weeks later as "it doesn't start". Checking it here names the commit
# that moved it.
#
# The ceilings are deliberate, not observations to be raised when they bite:
#   aarch64 portable (focal container)  2.18 / none   spruceOS et al
#   aarch64 standard (current runner)   2.38 / 3.4.30 dArkOS ships GCC 12.4,
#                                                     whose libstdc++ tops out
#                                                     at GLIBCXX_3.4.30
# "none" means the binary must import no GLIBCXX at all, i.e. libstdc++ is
# linked statically.
set -euo pipefail
BIN=${1:?usage: abi_floor.sh <binary> <max GLIBC> [max GLIBCXX|none] [readelf]}
MAX_GLIBC=${2:?need a maximum GLIBC version, e.g. 2.18}
MAX_GLIBCXX=${3:-none}
READELF=${4:-readelf}

syms() { "$READELF" -W --dyn-syms "$BIN" | grep -oE "[A-Za-z_0-9.]+@$1_[0-9.]+" || true; }
# grep finds nothing when a binary imports no GLIBCXX at all (a static
# libstdc++ -- the expected case for the portable tarball), and under pipefail
# that would abort the script instead of reporting "none".
maxver() { { grep -oE "$1_[0-9.]+" || true; } | sed "s/$1_//" | sort -uV | tail -1; }

fail=0
got_glibc=$(syms GLIBC | maxver GLIBC)
: "${got_glibc:=none}"
echo "$BIN"
echo "  max GLIBC:   ${got_glibc}  (ceiling ${MAX_GLIBC})"
if [ "$got_glibc" != none ] &&
   [ "$(printf '%s\n%s\n' "$MAX_GLIBC" "$got_glibc" | sort -V | tail -1)" != "$MAX_GLIBC" ]; then
  echo "  FAIL: needs glibc newer than the ceiling. Imports above it:"
  syms GLIBC | while read -r s; do
    v=${s##*@GLIBC_}
    [ "$(printf '%s\n%s\n' "$MAX_GLIBC" "$v" | sort -V | tail -1)" = "$MAX_GLIBC" ] || echo "    $s"
  done | sort -u
  fail=1
fi

got_cxx=$(syms GLIBCXX | maxver GLIBCXX)
: "${got_cxx:=none}"
echo "  max GLIBCXX: ${got_cxx}  (ceiling ${MAX_GLIBCXX})"
if [ "$MAX_GLIBCXX" = none ]; then
  [ "$got_cxx" = none ] || { echo "  FAIL: expected a static libstdc++, found GLIBCXX imports"; fail=1; }
elif [ "$got_cxx" != none ] &&
     [ "$(printf '%s\n%s\n' "$MAX_GLIBCXX" "$got_cxx" | sort -V | tail -1)" != "$MAX_GLIBCXX" ]; then
  echo "  FAIL: needs a libstdc++ newer than the ceiling."
  fail=1
fi

[ $fail = 0 ] || { echo "  (if this is intended, move the ceiling in the workflow and say which devices it drops)"; exit 1; }
echo "  ok"
