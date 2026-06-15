import struct
TEX_TGT=0x6E4CC522
def patch_material_texture(matl_bytes, new_tex_ing):
    # repoint every texture AssetReference ([8B ing][4B 0x6E4CC522]) to new_tex_ing. Shader ref (field7/@48) untouched.
    d=bytearray(matl_bytes); i=72
    while i+12<=len(d):
        if struct.unpack_from('<I',d,i)[0]==TEX_TGT:
            struct.pack_into('<Q',d,i-8,new_tex_ing & ((1<<64)-1))
        i+=4
    return bytes(d)
def shader_ref(matl_bytes):
    spkg,sing,stgt=struct.unpack_from('<QQI',matl_bytes,48); return spkg,sing,stgt
