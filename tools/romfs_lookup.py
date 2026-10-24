import struct,sys,re
# usage: romfs_lookup.py <romfs.bin> <log with NAPPLE_FS_TRACE lines> <from secs> <to secs>: which RomFS files the game read in a time window
f=open(sys.argv[1],'rb')
hdr=f.read(0x50)
(hs,dh_off,dh_size,dm_off,dm_size,fh_off,fh_size,fm_off,fm_size,data_off)=struct.unpack('<QQQQQQQQQQ',hdr[:0x50])
def rd(off,n): f.seek(off); return f.read(n)
dirm=rd(dm_off,dm_size); filem=rd(fm_off,fm_size)
files=[]
def walk(doff,path):
    parent,sib,child,file,hashn,nlen=struct.unpack('<IIIIII',dirm[doff:doff+24])
    name=dirm[doff+24:doff+24+nlen].decode()
    p=path+'/'+name if name else path
    fo=file
    while fo!=0xffffffff:
        parent2,sibling,off,size,hashn2,nl=struct.unpack('<IIQQII',filem[fo:fo+32])
        nm=filem[fo+32:fo+32+nl].decode()
        files.append((data_off+off,size,p+'/'+nm)); fo=sibling
    c=child
    while c!=0xffffffff:
        walk(c,p); c=struct.unpack('<I',dirm[c+4:c+8])[0]
walk(0,'')
files.sort()
import bisect
starts=[x[0] for x in files]
def lookup(o):
    i=bisect.bisect_right(starts,o)-1
    if i>=0 and files[i][0]<=o<files[i][0]+files[i][1]: return files[i][2]
    return '?'
for line in open(sys.argv[2]):
    m=re.search(r'\[napple +([0-9.]+)\] fs: RomFS read offset (0x[0-9a-f]+) size (0x[0-9a-f]+)',line)
    if m:
        t=float(m.group(1))
        if float(sys.argv[3])<=t<=float(sys.argv[4]): print(t,m.group(3),lookup(int(m.group(2),16)))
