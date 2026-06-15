import struct, sys, os, subprocess, tempfile
sys.path.insert(0, os.path.dirname(__file__))
import fb
ASTCENC = os.path.abspath('astcenc_bin/bin/astcenc-avx2.exe')

def _read_fb_slot(d, root, fi):
    u16=lambda o: struct.unpack_from('<H',d,o)[0]; u32=lambda o: struct.unpack_from('<I',d,o)[0]; i32=lambda o: struct.unpack_from('<i',d,o)[0]
    vt=root-i32(root); vtsz=u16(vt); idx=vt+4+fi*2
    if idx+2>vt+vtsz: return 0
    fo=u16(idx); return (root+fo) if fo else 0

def read_rendtxtr_format(d):
    assert d[4:8]==b'TXTR'
    root=struct.unpack_from('<I',d,0)[0]
    sF=_read_fb_slot(d,root,6)
    return d[sF] if sF else 0

def encode_rendtxtr(rgba, w, h, block=(8,8), formatCode=None, quality='-medium'):
    # rgba: bytes RGBA8 w*h. Write a temp PNG-less .tga? astcenc reads .png/.tga/.exr/.bmp...
    import numpy as np
    arr=np.frombuffer(rgba,dtype=np.uint8).reshape(h,w,4)
    tdir=tempfile.mkdtemp()
    inp=os.path.join(tdir,'in.bmp'); outp=os.path.join(tdir,'out.astc')
    _write_bmp(inp, arr)
    bs='%dx%d'%block
    r=subprocess.run([ASTCENC,'-cl',inp,outp,bs,quality],capture_output=True,text=True)
    if not os.path.exists(outp): raise RuntimeError('astcenc failed: '+r.stderr[-400:])
    raw=open(outp,'rb').read()
    # .astc header = 16 bytes: magic(4)+blockdim(3)+xyz dims(9). Strip it -> raw blocks (mip0).
    assert raw[:4]==b'\x13\xab\xa1\x5c', raw[:4].hex()
    blocks=raw[16:]
    if formatCode is None: formatCode=0
    b=fb.Builder(len(blocks)+512)
    dv=b.CreateByteVector(blocks)
    b.StartObject(10)
    b.AddScalar('H',3,w); b.AddScalar('H',4,h); b.AddScalar('B',6,formatCode); b.AddScalar('H',7,1)
    b.AddOffset(9,dv); root=b.EndObject(); b.Finish(root,b'TXTR')
    return b.Output(), block

def _write_bmp(path, arr):
    h,w,_=arr.shape
    import numpy as np
    bgr=arr[::-1,:,[2,1,0]]  # BMP is bottom-up BGR
    rowbytes=w*3; pad=(4-rowbytes%4)%4; stride=rowbytes+pad
    img=bytearray()
    for y in range(h):
        img+=bgr[y].tobytes(); img+=b'\x00'*pad
    size=54+len(img)
    hdr=b'BM'+struct.pack('<IHHI',size,0,0,54)+struct.pack('<IiiHHIIiiII',40,w,h,1,24,0,len(img),2835,2835,0,0)
    open(path,'wb').write(hdr+bytes(img))

# decode round-trip check via parseRendtxtrHeader logic
def parse_check(d):
    assert d[4:8]==b'TXTR'
    root=struct.unpack_from('<I',d,0)[0]
    sW=_read_fb_slot(d,root,3); sH=_read_fb_slot(d,root,4); sD=_read_fb_slot(d,root,9)
    w=struct.unpack_from('<H',d,sW)[0]; h=struct.unpack_from('<H',d,sH)[0]
    uoff=struct.unpack_from('<I',d,sD)[0]; vp=sD+uoff; ln=struct.unpack_from('<I',d,vp)[0]
    # derive block via mip0 bytes
    blk=None
    for bw,bh in [(4,4),(5,5),(6,6),(8,8),(10,10),(12,12)]:
        if ((w+bw-1)//bw)*((h+bh-1)//bh)*16==ln: blk=(bw,bh); break
    return w,h,ln,blk

if __name__=='__main__':
    import numpy as np
    W=H=128
    img=np.zeros((H,W,4),np.uint8)
    img[...,0]=np.add.outer(np.arange(H),np.arange(W)).astype(np.uint8)  # gradient R
    img[...,1]=128; img[...,3]=255
    blob,blk=encode_rendtxtr(img.tobytes(),W,H,(8,8),formatCode=160)
    w,h,ln,dblk=parse_check(blob)
    print('RENDTXTR:', 'OK %dx%d payload=%dB block=%s (encoded %dB)'%(w,h,ln,dblk,len(blob)) if (w==W and h==H and dblk==(8,8)) else 'FAIL %s'%((w,h,ln,dblk),))

def wrap_astc(blocks, w, h, formatCode=160):
    # wrap PRE-COMPRESSED ASTC mip0 blocks into a RENDTXTR FB (no re-encode) — for glTF KTX (already ASTC).
    import sys, os; sys.path.insert(0, os.path.dirname(__file__)); import fb
    b=fb.Builder(len(blocks)+512); dv=b.CreateByteVector(blocks)
    b.StartObject(10); b.AddScalar('H',3,w); b.AddScalar('H',4,h); b.AddScalar('B',6,formatCode); b.AddScalar('H',7,1)
    b.AddOffset(9,dv); root=b.EndObject(); b.Finish(root,b'TXTR'); return b.Output()

def ktx1_mip0(ktx):
    # extract (w,h,internalFormat, mip0_bytes) from a KTX1 file
    import struct
    assert ktx[:12]==b'\xabKTX 11\xbb\r\n\x1a\n', ktx[:12]
    endian=struct.unpack_from('<I',ktx,12)[0]
    (glType,glTypeSize,glFormat,glIntFmt,glBaseFmt,w,h,d,arr,faces,mips,kvlen)=struct.unpack_from('<12I',ktx,16)
    off=64+kvlen
    imgsize=struct.unpack_from('<I',ktx,off)[0]; off+=4
    mip0=ktx[off:off+imgsize]
    return w,h,glIntFmt,mip0

# GL ASTC internalFormat -> (blockW,blockH) -> our formatCode (renderer derives from len anyway; set sane)
ASTC_GL={0x93B0:(4,4),0x93B1:(5,4),0x93B2:(5,5),0x93B3:(6,5),0x93B4:(6,6),0x93B5:(8,5),0x93B6:(8,6),0x93B7:(8,8),
         0x93B8:(10,5),0x93B9:(10,6),0x93BA:(10,8),0x93BB:(10,10),0x93BC:(12,10),0x93BD:(12,12)}
