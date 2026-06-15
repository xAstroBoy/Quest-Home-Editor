import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__)); import fb
SURF_TGT=0xA1767FE9; TEX_TGT=0x6E4CC522
# Minimal MATL: root.field7 = shader AssetRef struct {pkg,ing,tgt}; a texture AssetRef carrying the 0x6E4CC522
# sentinel (renderer scans for it, i>=72); field5 = matParams blob (16B white). Magic "MATL"@4.
def encode_matl(shader_pkg, shader_ing, tex_ing, tint=(1,1,1,1)):
    b=fb.Builder(1024)
    # matParams blob (16 bytes: a vec4) as a ubyte vector -> field 5
    mp=b.CreateByteVector(struct.pack('<4f',*tint))
    # texture AssetRef as a 20B struct {pkg=0,ing,tgt=sentinel} inside a 1-elem vector (so it lands past off 72)
    texraw=struct.pack('<QQI',0,tex_ing & ((1<<64)-1),TEX_TGT)+b'\x00\x00\x00\x00'  # pad to 24
    texvec=b.CreateStructVector(texraw,24,1,align=8)
    b.StartObject(10)
    b.AddOffset(5,mp)                 # field5 matParams blob
    b.AddOffset(8,texvec)             # field8 texture vector (any field; renderer scans bytes for sentinel)
    # field7 = inline shader AssetRef struct (20B). fb has no struct-slot; prepend manually before EndObject:
    b.Prep(8,20); b.head-=20; struct.pack_into('<QQI',b.Bytes,b.head,shader_pkg&((1<<64)-1),shader_ing&((1<<64)-1),SURF_TGT); b.Slot(7)
    root=b.EndObject(); b.Finish(root,b'MATL'); return b.Output()
if __name__=='__main__':
    d=encode_matl(0x1111,0x2222,0x3333)
    root=struct.unpack_from('<I',d,0)[0]; print('MATL %d bytes magic=%r'%(len(d),d[4:8]))
    i=72; found=False
    while i+12<=len(d):
        if struct.unpack_from('<I',d,i)[0]==TEX_TGT: print('  texture sentinel @%d ing=%016X'%(i,struct.unpack_from('<Q',d,i-8)[0])); found=True; break
        i+=4
    print('  sentinel found=%s'%found)
