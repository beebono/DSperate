#!/usr/bin/env python3
"""Regenerate src/core/bios/freebios_data.cpp from a FreeBIOS build.

Usage: tools/freebios_import.py path/to/FreeBIOS_Data.h
(the header melonDS's freebios/Makefile writes: `make -C freebios` with an
arm-none-eabi toolchain). Only the NTR images are taken.
"""
import re, sys

src = open(sys.argv[1]).read()

def arr(name):
    m = re.search(r'unsigned char %s\[\] = \{(.*?)\};' % name, src, re.S)
    return [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(1))]

def emit(name, data):
    out = 'const u8 %s[%d] = {\n' % (name, len(data))
    for i in range(0, len(data), 16):
        out += '  ' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',\n'
    return out + '};\nconst u32 %s_len = %d;\n' % (name, len(data))

hdr = '''// SPDX-License-Identifier: BSD-3-Clause
// FreeBIOS: a clean-room NDS ARM7/ARM9 BIOS replacement.
// Copyright (c) 2013, Gilead Kutnick. All rights reserved. See LICENSE.freebios.
//
// Generated from the melonDS FreeBIOS_Data.h build of freebios/src/bios_common.s
// (NTR targets only). Do not edit; regenerate with tools/freebios_import.py.
#include "core/bios/freebios.h"

namespace ds::bios {
'''
out = hdr + emit('kFreeBios9', arr('bios_ntr_arm9')) + '\n' + emit('kFreeBios7', arr('bios_ntr_arm7')) + '\n} // namespace ds::bios\n'
open('src/core/bios/freebios_data.cpp', 'w').write(out)
