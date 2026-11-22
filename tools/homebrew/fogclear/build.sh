#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the rear-plane fog test ROM. Usage: build.sh [OUT.nds] (default: fogclear.nds beside this script).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/fogclear.nds}
tmp=$(mktemp -d)
CC=${CC:-arm-linux-gnueabihf-gcc}
OBJCOPY=${OBJCOPY:-arm-linux-gnueabihf-objcopy}
FLAGS="-marm -march=armv5te -mfloat-abi=soft -ffreestanding -nostdlib -fno-pic -fno-builtin -O1"
$CC $FLAGS -c "$here/fog.c" -o "$tmp/fog.o"
$CC $FLAGS -c "$here/start.S" -o "$tmp/start.o"
$CC $FLAGS -Wl,-T,"$here/link.ld" -Wl,--build-id=none -o "$tmp/fog.elf" "$tmp/start.o" "$tmp/fog.o"
$OBJCOPY -O binary -j .text9 "$tmp/fog.elf" "$tmp/arm9.bin"
$OBJCOPY -O binary -j .text7 "$tmp/fog.elf" "$tmp/arm7.bin"
python3 "$here/../wiretest/mkrom.py" FOGC "$tmp/arm9.bin" "$tmp/arm7.bin" "$out"
rm -rf "$tmp"
