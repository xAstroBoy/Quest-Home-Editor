import struct
# Binary AXML (AndroidManifest.xml) string-pool editor: replace a substring in every pooled string
# and rebuild the pool (offsets/sizes), leaving XML node chunks (which reference strings by INDEX) intact.
def replace_in_axml(axml, old, new):
    assert struct.unpack_from('<H',axml,0)[0]==0x0003, 'not AXML'
    total=struct.unpack_from('<I',axml,4)[0]
    sp=8
    assert struct.unpack_from('<H',axml,sp)[0]==0x0001, 'no string pool'
    sp_hdr=struct.unpack_from('<H',axml,sp+2)[0]
    sp_size=struct.unpack_from('<I',axml,sp+4)[0]
    strCount,styleCount,flags,stringsStart,stylesStart=struct.unpack_from('<IIIII',axml,sp+8)
    utf8=(flags&0x100)!=0
    off0=sp+sp_hdr
    offsets=[struct.unpack_from('<I',axml,off0+i*4)[0] for i in range(strCount)]
    sbase=sp+stringsStart
    strings=[]
    for o in offsets:
        p=sbase+o
        if utf8:
            # u8/u16 charlen then u8/u16 bytelen
            def declen(q):
                n=axml[q]; 
                if n&0x80: return ((n&0x7f)<<8)|axml[q+1], q+2
                return n,q+1
            cl,q=declen(p); bl,q=declen(q); s=axml[q:q+bl].decode('utf-8'); strings.append(s)
        else:
            l=struct.unpack_from('<H',axml,p)[0]; q=p+2
            if l&0x8000: l=((l&0x7fff)<<16)|struct.unpack_from('<H',axml,q)[0]; q+=2
            strings.append(axml[q:q+l*2].decode('utf-16-le'))
    if not any(old in s for s in strings): return axml, 0
    nreplaced=sum(1 for s in strings if old in s)
    new_s=[s.replace(old,new) for s in strings]
    # rebuild string data (keep same encoding)
    data=bytearray(); noff=[]
    for s in new_s:
        noff.append(len(data))
        if utf8:
            b=s.encode('utf-8'); 
            def enc(n):
                return bytes([n]) if n<0x80 else bytes([0x80|(n>>8),n&0xff])
            data+=enc(len(s))+enc(len(b))+b+b'\x00'
        else:
            b=s.encode('utf-16-le'); data+=struct.pack('<H',len(s))+b+b'\x00\x00'
    while len(data)%4: data.append(0)
    off_arr=b''.join(struct.pack('<I',o) for o in noff)
    style_arr=axml[off0+strCount*4:sbase]   # style offsets + any pad (usually empty)
    new_stringsStart=sp_hdr+len(off_arr)+len(style_arr)
    body=struct.pack('<IIIII',strCount,styleCount,flags,new_stringsStart,0)+off_arr+style_arr+bytes(data)
    new_sp_size=8+len(body)              # chunk header(8) + body
    new_sp=struct.pack('<HHI',0x0001,sp_hdr,new_sp_size)+body
    rest=axml[sp+sp_size:]
    out=bytearray(axml[:8])+new_sp+rest
    struct.pack_into('<I',out,4,len(out))
    return bytes(out), nreplaced

if __name__=='__main__':
    import zipfile,sys,subprocess
    z=zipfile.ZipFile(r'Envs To check/v203 Ufficial Envs/Nuxd.apk')
    am=z.read('AndroidManifest.xml')
    out,n=replace_in_axml(am,'com.meta.environment.prod.nuxd','com.environment.outerwilds')
    print('replaced in %d strings, size %d->%d'%(n,len(am),len(out)))
    open('cooker/out/_test_manifest.bin','wb').write(out)
