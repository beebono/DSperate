#!/usr/bin/env python3
# edgediff.py A.raw A.edge B.raw B.edge out.png: two vk_probe outputs of one frame (the 3D
# record and the edge plane, DS_VK_EDGES=1) compared. Prints the edge bytes that differ by
# kind and the colour-only differences; writes A at 3x with differing pixels marked
# (red: edge vs none, yellow: side/axis, orange: coverage, blue: colour).
def png(path,w,h,rows):
    raw=b''.join(b'\x00'+r for r in rows)
    def ch(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
ar,ae,br,be,out=sys.argv[1:6]
A=open(ar,'rb').read(); E=open(ae,'rb').read(); B=open(br,'rb').read(); F=open(be,'rb').read()
W,H=256,192
kinds=collections.Counter(); cdiff=0; maxd=0
rows=[]
for y in range(H):
    r=bytearray()
    for x in range(W):
        i=y*W+x; a=struct.unpack_from('<I',A,i*4)[0]; b=struct.unpack_from('<I',B,i*4)[0]
        ea,eb=E[i],F[i]
        px=[(a&63)*4,((a>>8)&63)*4,((a>>16)&63)*4]
        if ea!=eb:
            if (ea&0x80)!=(eb&0x80): kinds['edge vs none']+=1; px=[255,0,0]
            elif (ea&0x60)!=(eb&0x60): kinds['side/axis']+=1; px=[255,255,0]
            else: kinds['coverage only']+=1; px=[255,128,0]
        elif a!=b:
            cdiff+=1; d=max(abs((a>>s&63)-(b>>s&63)) for s in (0,8,16)); maxd=max(maxd,d); px=[0,0,255] if d>2 else [0,160,255]
        r+=bytes(px)
    rows.append(bytes(r))
# 3x upscale for viewing
png(out,W*3,H*3,[bytes(b''.join(row[i*3:i*3+3]*3 for i in range(W))) for row in rows for _ in range(3)])
print('edge diffs:',dict(kinds),'colour-only diffs:',cdiff,'max channel delta',maxd)
