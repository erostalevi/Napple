#!/usr/bin/env python3
"""Writes a valid but empty RomFS image (no game data) for runtime tests when the real one is unavailable."""
import struct, sys
out = sys.argv[1]
# header: u64 header_size(0x50), dirhash(off,size), dirmeta(off,size), filehash(off,size), filemeta(off,size), data_off
dirhash = struct.pack('<I', 0xFFFFFFFF)                       # one bucket, empty
dirmeta = struct.pack('<IIIIII', 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0) + b''  # root: parent,sibling,child dir,child file,next hash,name len
filehash = struct.pack('<I', 0xFFFFFFFF)
filemeta = b''
off = 0x200
h = struct.pack('<QQQQQQQQQQ', 0x50, off, len(dirhash), off + 4, len(dirmeta), off + 4 + len(dirmeta), len(filehash), off + 8 + len(dirmeta), len(filemeta), (off + 8 + len(dirmeta) + 0x20) & ~0xf)
img = bytearray(0x400)
img[:0x50] = h
img[off:off + 4] = dirhash
img[off + 4:off + 4 + len(dirmeta)] = dirmeta
img[off + 4 + len(dirmeta):off + 8 + len(dirmeta)] = filehash
open(out, 'wb').write(img)
