#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the wireframe test ROM. Usage: build.sh [OUT.nds] (default: wiretest.nds beside this script).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/wiretest.nds}
tmp=$(mktemp -d)
CC=${CC:-arm-linux-gnueabihf-gcc}
OBJCOPY=${OBJCOPY:-arm-linux-gnueabihf-objcopy}
FLAGS="-marm -march=armv5te -mfloat-abi=soft -ffreestanding -nostdlib -fno-pic -fno-builtin -O1"
$CC $FLAGS -c "$here/wire.c" -o "$tmp/wire.o"
$CC $FLAGS -c "$here/start.S" -o "$tmp/start.o"
$CC $FLAGS -Wl,-T,"$here/link.ld" -Wl,--build-id=none -o "$tmp/wire.elf" "$tmp/start.o" "$tmp/wire.o"
$OBJCOPY -O binary -j .text9 "$tmp/wire.elf" "$tmp/arm9.bin"
$OBJCOPY -O binary -j .text7 "$tmp/wire.elf" "$tmp/arm7.bin"
python3 "$here/mkrom.py" WIRE "$tmp/arm9.bin" "$tmp/arm7.bin" "$out"
rm -rf "$tmp"
