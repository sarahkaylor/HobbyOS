#!/bin/bash
ssh -i ~/.ssh/mac_to_r1 -o StrictHostKeyChecking=no root@192.168.10.174 "
(sleep 0.5; echo '{\"execute\":\"qmp_capabilities\"}'; sleep 0.3; echo '{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"/root/ss225.ppm\"}}'; sleep 2) | socat - UNIX-CONNECT:/var/run/qemu-server/225.qmp 2>&1 | tr -d '\r'
"
scp -i ~/.ssh/mac_to_r1 -o StrictHostKeyChecking=no root@192.168.10.174:/root/ss225.ppm /Users/sarah/.gemini/antigravity/brain/fe3a48c8-684f-4be2-8295-3071c22c98db/vm225_cur.ppm
python3 -c "
import struct, zlib
with open('/Users/sarah/.gemini/antigravity/brain/fe3a48c8-684f-4be2-8295-3071c22c98db/vm225_cur.ppm','rb') as f:
    h=b''
    while h.count(b'\n')<3: h+=f.readline()
    parts=h.split(); w,h2=int(parts[1]),int(parts[2]); data=f.read()
def chunk(n,d): c=zlib.crc32(n+d)&0xffffffff; return struct.pack('>I',len(d))+n+d+struct.pack('>I',c)
rows=b''.join(b'\x00'+data[y*w*3:(y+1)*w*3] for y in range(h2))
with open('/Users/sarah/.gemini/antigravity/brain/fe3a48c8-684f-4be2-8295-3071c22c98db/vm225_cur.png','wb') as f:
    f.write(b'\x89PNG\r\n\x1a\n')
    f.write(chunk(b'IHDR',struct.pack('>IIBBBBB',w,h2,8,2,0,0,0)))
    f.write(chunk(b'IDAT',zlib.compress(rows)))
    f.write(chunk(b'IEND',b''))
"
