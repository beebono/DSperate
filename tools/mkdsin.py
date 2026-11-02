#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Write a DSperate input replay (.dsin, version 2) from a few scripted touches
# and key holds, for driving a title headless without a recorded session:
#   tools/mkdsin.py out.dsin FRAMES [F:x,y[:N]]... [kF:MASK[:N]]... [gF:MASK[:N]]...
# F:x,y presses the screen at (x,y) from frame F for N frames (default 10);
# kF:MASK holds the button mask (bit 0 A, 1 B, 2 Select, 3 Start, 4 Right,
# 5 Left, 6 Up, 7 Down, 8 R, 9 L, 10 X, 11 Y) likewise -- the same arguments
# a melonDS trace harness can take as --touch / --key, so one script drives both.
# gF:MASK holds Guitar Grip frets (bit 0 green, 1 red, 2 yellow, 3 blue).
import struct, sys
# usage: mkdsin.py out.dsin frames [F:x,y[:N]]... [kF:mask[:N]]...  (touch, or key mask held)
out=sys.argv[1]; n=int(sys.argv[2]); taps=[]; keys=[]; frets=[]
for t in sys.argv[3:]:
    if t[0] in 'kg':
        f,rest=t[1:].split(':',1); m,_,nn=rest.partition(':'); (keys if t[0]=='k' else frets).append((int(f),int(m,0),int(nn or 10)))
    else:
        f,rest=t.split(':',1); xy,_,nn=rest.partition(':'); x,y=xy.split(','); taps.append((int(f),int(x),int(y),int(nn or 10)))
with open(out,'wb') as o:
    o.write(b'DSIN'+struct.pack('<II',2,n)+b'\0'*4)
    for i in range(n):
        x=y=0; down=0; b=0; g=0
        for f,tx,ty,nn in taps:
            if f<=i<f+nn: x,y,down=tx,ty,1
        for f,m,nn in keys:
            if f<=i<f+nn: b|=m
        for f,m,nn in frets:
            if f<=i<f+nn: g|=m
        o.write(struct.pack('<HBBBB',b,x,y,down,g)+b'\0'*2+b'\0'*8)
