# Fully self-contained V203/HSL home cooker — ONLY the APK shell comes from Nuxd; ALL scene assets are cooked here.
import zipfile, io, json, sys, os, random, uuid
sys.path.insert(0, os.path.dirname(__file__))
import fb, rendmesh, rendtxtr, rendshad, matl2, fb_asmh, axml
import numpy as np
NUXD=r'Envs To check/v203 Ufficial Envs/Nuxd.apk'
PKG_OLD='com.meta.environment.prod.nuxd'; PKG_NEW='com.environment.outerwilds'
TPL=2719744159; TEX_TGT=0x6E4CC522; SURF_TGT=0xA1767FE9; MESH_TGT=0x4D455348; MAT_TGT=0x095BD446
def make_floor(h=20.0,t=2.0): return [(-h,0,-h),(h,0,-h),(h,0,h),(-h,0,h)],[(0,0),(t,0),(t,t),(0,t)],[0,2,1,0,3,2]
def checker(n=256,sq=32):
    a=np.zeros((n,n,4),np.uint8); a[...,3]=255; yy,xx=np.mgrid[0:n,0:n]; m=((xx//sq+yy//sq)&1).astype(bool)
    a[m]=[220,40,40,255]; a[~m]=[235,235,235,255]; return a
def main():
    z=zipfile.ZipFile(NUXD); apk_names=z.namelist()
    rnd=random.Random(0xC0FFEE)
    def k(t): return (rnd.getrandbits(64), rnd.getrandbits(64)|(1<<63), t)
    mesh_key=k(MESH_TGT); tex_key=(0,rnd.getrandbits(64)|(1<<63),TEX_TGT); shader_key=k(SURF_TGT); mat_key=k(MAT_TGT); content_key=k(TPL); space_key=k(TPL)
    mesh_p='meta/myhome/floor.rendmesh/mesh'; tex_p='meta/myhome/floor.png/tex'; shader_p='meta/myhome/shaders/myunlit.surface/shader'
    mat_p='meta/myhome/floor.material/material'; content_p='meta/myhome/content.hstf/template'; space_p='meta/myhome/space.hstf/template'
    content={}
    Pos,Uv,Idx=make_floor(); content['content/'+mesh_p]=rendmesh.encode_rendmesh(Pos,Uv,Idx)
    img=checker(); content['content/'+tex_p]=rendtxtr.encode_rendtxtr(img.tobytes(),img.shape[1],img.shape[0],(8,8),160)[0]
    vspv=open('cooker/shaders/myunlit.vert.spv','rb').read(); fspv=open('cooker/shaders/myunlit.frag.spv','rb').read()
    content['content/'+shader_p]=rendshad.encode_surface([vspv,fspv])
    content['content/'+mat_p]=matl2.encode_matl(shader_key[0],shader_key[1],tex_key[1])
    def aref(key): return {'packageOrRemoteId':str(key[0]),'ingestionId':str(key[1]),'targetId':key[2]}
    def comp(c,v,dd): return {'data':{'class':'horizon::platform_api::'+c,'version':v,'data':dd},'dataType':'horizon::DataDefinitionAsset'}
    floor={'id':str(uuid.uuid4()),'name':'CustomFloor','components':[
        comp('TransformPlatformComponent',1,{'localPosition':{'x':0.0,'y':0.0,'z':0.0}}),
        comp('MeshPlatformComponent',11,{'mesh':aref(mesh_key)}),
        comp('MaterialPlatformComponent',1,{'materials':[aref(mat_key)]})],'attributes':[]}
    content['content/'+content_p]=json.dumps({'version':5,'entities':[floor],'relationships':[]}).encode()
    content['content/'+space_p]=json.dumps({'version':5,'entities':[{'id':str(uuid.uuid4()),'name':'myhome','type':aref(content_key),'deltas':[],'attributes':[]}],'relationships':[]}).encode()
    content['content/configs/shellconfig.jsonc']=json.dumps({'firstWorldAssetId':aref(space_key),'supportsLocomotion':True}).encode()
    def E(key,p): d=content['content/'+p]; return (key[0],key[1],key[2],p,fb_asmh.asset_fourcc(d),len(d))
    entries=[E(mesh_key,mesh_p),E(tex_key,tex_p),E(shader_key,shader_p),E(mat_key,mat_p),E(content_key,content_p),E(space_key,space_p)]
    content['content/assets.manifest']=fb_asmh.build_asmh(entries)
    sb=io.BytesIO()
    with zipfile.ZipFile(sb,'w',zipfile.ZIP_DEFLATED) as oz:
        for n,d in content.items(): oz.writestr(n,d)
    scene=sb.getvalue(); os.makedirs('cooker/out',exist_ok=True)
    SKIP={'assets/one_query_hash.txt','assets/params_map.txt','assets/params_map_v4_u0.txt','assets/params_names_v4_u0.txt'}
    with zipfile.ZipFile('cooker/out/myhome.apk','w') as o:
        for n in apk_names:
            if n.startswith('META-INF/') or n in SKIP: continue
            si=z.getinfo(n); data=scene if n=='assets/scene.zip' else z.read(n)
            if n=='AndroidManifest.xml': data,_=axml.replace_in_axml(data,PKG_OLD,PKG_NEW)
            zi=zipfile.ZipInfo(n,date_time=si.date_time); zi.external_attr=si.external_attr
            zi.compress_type=zipfile.ZIP_STORED if n=='resources.arsc' else zipfile.ZIP_DEFLATED
            o.writestr(zi,data)
    print('SELF-CONTAINED (only APK shell from Nuxd): %d scene files, %dKB | shader+material+mesh+tex ALL cooked'%(len(content),len(scene)//1024))
main()
