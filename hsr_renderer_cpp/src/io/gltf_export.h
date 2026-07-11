// ── gltf_export.h — export the loaded env to a BLENDER-COMPATIBLE glTF 2.0 project ──────────────────────────────
// Writes <outDir>/<env>.gltf + <env>.bin + textures/*.png (standard glTF 2.0 that Blender's bundled importer opens
// natively: meshes, UVs, vertex colours, per-mesh node transforms, and PBR materials with base/normal/emissive maps)
// PLUS a sidecar <env>.blendmeta.json that records, per object, the original mesh index + its full hstf component list
// + the env's spawn/cook config — so the EDITED project can be re-imported and re-cooked back to an APK with the env
// structure intact (the round-trip). Self-contained: PNG via the already-linked miniz (no stb-impl TU needed).
#pragma once
#include "../core/types.h"
#include "../cook/hsl_cooker.h"   // hslcook::ExportMesh — the unified cook source (geometry + skeleton + clips + node anims)
#include "miniz.h"
#include <string>
#include <vector>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <filesystem>

namespace gltfexport {

// ── minimal RGBA8 -> PNG via miniz zlib (PNG = sig + IHDR + IDAT(zlib(filtered scanlines)) + IEND) ──
inline bool writePng(const std::string& path, const uint8_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0 || !rgba) return false;
    std::vector<uint8_t> raw; raw.reserve((size_t)h * (1 + (size_t)w * 4));
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4); }
    mz_ulong clen = mz_compressBound((mz_ulong)raw.size());
    std::vector<uint8_t> comp(clen);
    if (mz_compress(comp.data(), &clen, raw.data(), (mz_ulong)raw.size()) != MZ_OK) return false;
    comp.resize(clen);
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    auto be32 = [](std::vector<uint8_t>& v, uint32_t x){ v.push_back((uint8_t)(x>>24)); v.push_back((uint8_t)(x>>16)); v.push_back((uint8_t)(x>>8)); v.push_back((uint8_t)x); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data){
        std::vector<uint8_t> len; be32(len, (uint32_t)data.size()); fwrite(len.data(),1,4,f);
        mz_ulong crc = mz_crc32(MZ_CRC32_INIT, (const uint8_t*)type, 4);
        crc = mz_crc32(crc, data.data(), data.size());
        fwrite(type,1,4,f); if (!data.empty()) fwrite(data.data(),1,data.size(),f);
        std::vector<uint8_t> c; be32(c, (uint32_t)crc); fwrite(c.data(),1,4,f);
    };
    const uint8_t sig[8] = {137,80,78,71,13,10,26,10}; fwrite(sig,1,8,f);
    std::vector<uint8_t> ihdr; be32(ihdr,(uint32_t)w); be32(ihdr,(uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);  // 8-bit RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", comp);
    chunk("IEND", {});
    fclose(f); return true;
}

// JSON string escaper (mesh names can carry odd chars)
inline std::string jesc(const std::string& s){ std::string o; for(char c:s){ if(c=='"'||c=='\\'){o.push_back('\\');o.push_back(c);} else if((unsigned char)c<0x20){char b[8];snprintf(b,sizeof b,"\\u%04x",c);o+=b;} else o.push_back(c);} return o; }
// glTF resource URIs MUST be percent-encoded (RFC 3986): a raw space/‘#’/etc. makes a NON-conformant .gltf
// that Blender & other importers reject and that breaks on round-trip. Encode everything except the RFC-3986
// unreserved set + '/' (path separator). The file on disk keeps its raw name; the encoded URI decodes back to it.
inline std::string urlEncode(const std::string& s){ std::string o; char b[4];
    for(unsigned char c : s){
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~'||c=='/') o.push_back((char)c);
        else { snprintf(b,sizeof b,"%%%02X",c); o+=b; }
    } return o; }

// pad the binary blob to a 4-byte boundary (glTF accessors require alignment)
inline void pad4(std::vector<uint8_t>& b){ while (b.size() & 3) b.push_back(0); }

// Export `meshes` (the editor's CPU scene) to a Blender-ready glTF project under outDir.
// metaExtra = caller-supplied JSON object body (no braces) appended to the sidecar (spawn, cook config, navmesh...).
inline bool exportEnv(const std::vector<MeshData>& meshes, const std::string& outDir,
                      const std::string& envName, const std::string& metaExtra = "") {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(outDir) / "textures", ec);

    std::vector<uint8_t> bin;
    std::string aMeshes, aMaterials, aTextures, aImages, aAccessors, aBufferViews, aNodes;
    std::string meta;   // sidecar per-object records
    int acc = 0, bv = 0, tex = 0, img = 0, mat = 0, node = 0;
    std::vector<int> sceneNodes;

    auto addBufferView = [&](size_t off, size_t len, int target){
        if (bv) aBufferViews += ",";
        char t[160]; snprintf(t,sizeof t,"{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu%s}", off, len,
                              target ? (target==34963 ? ",\"target\":34963" : ",\"target\":34962") : "");
        aBufferViews += t; return bv++;
    };
    auto addImage = [&](const std::vector<uint8_t>& rgba, int w, int h, const std::string& fname)->int{
        if (rgba.size() < (size_t)w*h*4) return -1;
        if (!writePng((fs::path(outDir)/"textures"/fname).string(), rgba.data(), w, h)) return -1;
        if (img) aImages += ","; aImages += "{\"uri\":\"textures/" + urlEncode(fname) + "\"}";
        int ii = img++;
        if (tex) aTextures += ","; aTextures += "{\"source\":" + std::to_string(ii) + "}";
        return tex++;   // return TEXTURE index
    };

    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const MeshData& m = meshes[mi];
        if (m.positions.size() < 9 || m.indices.size() < 3) continue;
        size_t nv = m.positions.size() / 3;

        // per-mesh world centroid -> node translation; local positions = world - centroid (clean origin for editing)
        double cx=0,cy=0,cz=0; for (size_t v=0; v<nv; ++v){ cx+=m.positions[v*3]; cy+=m.positions[v*3+1]; cz+=m.positions[v*3+2]; }
        cx/=nv; cy/=nv; cz/=nv;
        float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};

        // POSITION (local)
        pad4(bin); size_t posOff = bin.size();
        for (size_t v=0; v<nv; ++v){ float p[3]={(float)(m.positions[v*3]-cx),(float)(m.positions[v*3+1]-cy),(float)(m.positions[v*3+2]-cz)};
            for(int k=0;k<3;k++){ if(p[k]<mn[k])mn[k]=p[k]; if(p[k]>mx[k])mx[k]=p[k]; uint8_t b[4]; memcpy(b,&p[k],4); bin.insert(bin.end(),b,b+4);} }
        int posBv = addBufferView(posOff, bin.size()-posOff, 34962);
        if (acc) aAccessors += ",";
        { char t[256]; snprintf(t,sizeof t,"{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]}",
            posBv,nv,mn[0],mn[1],mn[2],mx[0],mx[1],mx[2]); aAccessors += t; }
        int posAcc = acc++;

        // TEXCOORD_0
        int uvAcc = -1;
        if (m.uvs.size() >= nv*2){ pad4(bin); size_t off=bin.size();
            for (size_t v=0; v<nv*2; ++v){ float u=m.uvs[v]; uint8_t b[4]; memcpy(b,&u,4); bin.insert(bin.end(),b,b+4);}
            int b2=addBufferView(off,bin.size()-off,34962); if(acc)aAccessors+=",";
            char t[160]; snprintf(t,sizeof t,"{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC2\"}",b2,nv); aAccessors+=t; uvAcc=acc++; }

        // COLOR_0 (u8x4 normalized)
        int colAcc = -1;
        if (m.colors.size() >= nv*4){ pad4(bin); size_t off=bin.size();
            bin.insert(bin.end(), m.colors.begin(), m.colors.begin()+nv*4);
            int b2=addBufferView(off,bin.size()-off,34962); if(acc)aAccessors+=",";
            char t[200]; snprintf(t,sizeof t,"{\"bufferView\":%d,\"componentType\":5121,\"normalized\":true,\"count\":%zu,\"type\":\"VEC4\"}",b2,nv); aAccessors+=t; colAcc=acc++; }

        // indices (u32)
        pad4(bin); size_t iOff=bin.size();
        for (uint32_t idx : m.indices){ uint8_t b[4]; memcpy(b,&idx,4); bin.insert(bin.end(),b,b+4); }
        int idxBv = addBufferView(iOff, bin.size()-iOff, 34963); if(acc)aAccessors+=",";
        { char t[160]; snprintf(t,sizeof t,"{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}",idxBv,m.indices.size()); aAccessors+=t; }
        int idxAcc = acc++;

        // material + textures
        int baseTex = m.hasTexture ? addImage(m.texRGBA, m.texW, m.texH, "base_"+std::to_string(mi)+".png") : -1;
        int normTex = m.hasNormal  ? addImage(m.normalRGBA, m.normalW, m.normalH, "norm_"+std::to_string(mi)+".png") : -1;
        int emisTex = m.hasEmissive? addImage(m.emissiveRGBA, m.emissiveW, m.emissiveH, "emis_"+std::to_string(mi)+".png") : -1;
        if (mat) aMaterials += ",";
        std::string pbr = "\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1";
        if (baseTex>=0) pbr += ",\"baseColorTexture\":{\"index\":"+std::to_string(baseTex)+"}";
        else { char bc[96]; snprintf(bc,sizeof bc,",\"baseColorFactor\":[%g,%g,%g,1]",m.tint[0],m.tint[1],m.tint[2]); pbr+=bc; }
        pbr += "}";
        aMaterials += "{\"name\":\""+jesc(m.name)+"\","+pbr;
        if (normTex>=0) aMaterials += ",\"normalTexture\":{\"index\":"+std::to_string(normTex)+"}";
        if (emisTex>=0) aMaterials += ",\"emissiveTexture\":{\"index\":"+std::to_string(emisTex)+"},\"emissiveFactor\":[1,1,1]";
        aMaterials += "}";
        int matIdx = mat++;

        // mesh primitive
        if (mi && !aMeshes.empty()) aMeshes += ",";
        std::string attrs = "\"POSITION\":"+std::to_string(posAcc);
        if (uvAcc>=0) attrs += ",\"TEXCOORD_0\":"+std::to_string(uvAcc);
        if (colAcc>=0) attrs += ",\"COLOR_0\":"+std::to_string(colAcc);
        aMeshes += "{\"name\":\""+jesc(m.name)+"\",\"primitives\":[{\"attributes\":{"+attrs+"},\"indices\":"+std::to_string(idxAcc)+",\"material\":"+std::to_string(matIdx)+"}]}";
        int meshIdx = (int)( [&]{ int c=0; for(size_t k=0;k<=mi;k++){ if(meshes[k].positions.size()>=9 && meshes[k].indices.size()>=3) ++c; } return c-1; }() );

        // node (translation = world centroid)
        if (node) aNodes += ",";
        char ntb[160]; snprintf(ntb,sizeof ntb,",\"mesh\":%d,\"translation\":[%g,%g,%g]}", meshIdx, cx,cy,cz);
        std::string nt="{\"name\":\""+jesc(m.name)+"\""+ntb;   // name via unbounded std::string (avoid fixed-buffer truncation)
        aNodes += nt; sceneNodes.push_back(node++);

        // sidecar record: original index + hstf components (verbatim) for faithful re-cook
        if (!meta.empty()) meta += ",";
        meta += "{\"node\":"+std::to_string(node-1)+",\"srcMeshIndex\":"+std::to_string(mi)+",\"name\":\""+jesc(m.name)+"\"";
        meta += ",\"skybox\":"+std::string(m.isSkybox?"true":"false")+"}";
    }

    if (sceneNodes.empty()) return false;
    std::string sn; for (size_t i=0;i<sceneNodes.size();++i){ if(i)sn+=","; sn+=std::to_string(sceneNodes[i]); }

    // assemble glTF
    std::string gltf = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"HSR Renderer (V79 Quest Home Porter)\"},";
    gltf += "\"scene\":0,\"scenes\":[{\"name\":\""+jesc(envName)+"\",\"nodes\":["+sn+"]}],";
    gltf += "\"nodes\":["+aNodes+"],";
    gltf += "\"meshes\":["+aMeshes+"],";
    gltf += "\"materials\":["+aMaterials+"],";
    if (tex) gltf += "\"textures\":["+aTextures+"],\"images\":["+aImages+"],";
    gltf += "\"accessors\":["+aAccessors+"],";
    gltf += "\"bufferViews\":["+aBufferViews+"],";
    gltf += "\"buffers\":[{\"uri\":\""+urlEncode(envName)+".bin\",\"byteLength\":"+std::to_string(bin.size())+"}]}";

    { FILE* f=fopen((fs::path(outDir)/(envName+".bin")).string().c_str(),"wb"); if(!f)return false; if(!bin.empty())fwrite(bin.data(),1,bin.size(),f); fclose(f); }
    { FILE* f=fopen((fs::path(outDir)/(envName+".gltf")).string().c_str(),"wb"); if(!f)return false; fwrite(gltf.data(),1,gltf.size(),f); fclose(f); }
    { FILE* f=fopen((fs::path(outDir)/(envName+".blendmeta.json")).string().c_str(),"wb"); if(f){
        std::string s = "{\"env\":\""+jesc(envName)+"\",\"objects\":["+meta+"]";
        if (!metaExtra.empty()) s += ","+metaExtra;
        s += "}"; fwrite(s.data(),1,s.size(),f); fclose(f); } }
    return true;
}

// ── matrix helpers (column-major 4×4) for skeletal inverse-bind matrices ──
typedef std::array<float,16> M4;
inline M4 m4id(){ return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
inline M4 m4mul(const M4& a, const M4& b){ M4 r{}; for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++){ float s=0; for(int k=0;k<4;k++) s+=a[k*4+rr]*b[c*4+k]; r[c*4+rr]=s; } return r; }
inline M4 m4trs(const float p[3], const float q[4]/*xyzw*/, const float s[3]){
    float x=q[0],y=q[1],z=q[2],w=q[3];
    float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    M4 m{};
    m[0]=(1-2*(yy+zz))*s[0]; m[1]=(2*(xy+wz))*s[0];   m[2]=(2*(xz-wy))*s[0];   m[3]=0;
    m[4]=(2*(xy-wz))*s[1];   m[5]=(1-2*(xx+zz))*s[1]; m[6]=(2*(yz+wx))*s[1];   m[7]=0;
    m[8]=(2*(xz+wy))*s[2];   m[9]=(2*(yz-wx))*s[2];   m[10]=(1-2*(xx+yy))*s[2];m[11]=0;
    m[12]=p[0]; m[13]=p[1]; m[14]=p[2]; m[15]=1; return m;
}
inline M4 m4inv(const M4& m){
    M4 inv{}; const float* a=m.data();
    inv[0]= a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
    inv[4]=-a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
    inv[8]= a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
    inv[12]=-a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
    inv[1]=-a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
    inv[5]= a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
    inv[9]=-a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
    inv[13]= a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
    inv[2]= a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
    inv[6]=-a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
    inv[10]= a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
    inv[14]=-a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
    inv[3]=-a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
    inv[7]= a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
    inv[11]=-a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
    inv[15]= a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
    float det=a[0]*inv[0]+a[1]*inv[4]+a[2]*inv[8]+a[3]*inv[12];
    if (std::fabs(det)<1e-20f) return m4id(); det=1.0f/det; for(int i=0;i<16;i++) inv[i]*=det; return inv;
}
inline void m4decompose(const M4& m, float q[4]/*xyzw*/, float t[3], float s[3]){
    t[0]=m[12]; t[1]=m[13]; t[2]=m[14];
    s[0]=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
    s[1]=std::sqrt(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]);
    s[2]=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
    float ix=s[0]>1e-8f?1.f/s[0]:0.f, iy=s[1]>1e-8f?1.f/s[1]:0.f, iz=s[2]>1e-8f?1.f/s[2]:0.f;
    float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
    float tr=r0+r4+r8;
    if(tr>0){float S=std::sqrt(tr+1)*2;q[3]=0.25f*S;q[0]=(r5-r7)/S;q[1]=(r6-r2)/S;q[2]=(r1-r3)/S;}
    else if(r0>r4&&r0>r8){float S=std::sqrt(1+r0-r4-r8)*2;q[3]=(r5-r7)/S;q[0]=0.25f*S;q[1]=(r3+r1)/S;q[2]=(r6+r2)/S;}
    else if(r4>r8){float S=std::sqrt(1+r4-r0-r8)*2;q[3]=(r6-r2)/S;q[0]=(r3+r1)/S;q[1]=0.25f*S;q[2]=(r7+r5)/S;}
    else{float S=std::sqrt(1+r8-r0-r4)*2;q[3]=(r1-r3)/S;q[0]=(r6+r2)/S;q[1]=(r7+r5)/S;q[2]=0.25f*S;} }
inline void quatAxisAngle(const float ax[3], float ang, float out[4]/*xyzw*/){
    float n=std::sqrt(ax[0]*ax[0]+ax[1]*ax[1]+ax[2]*ax[2]); if(n<1e-9f){out[0]=out[1]=out[2]=0;out[3]=1;return;}
    float s=std::sin(ang*0.5f); out[0]=ax[0]/n*s; out[1]=ax[1]/n*s; out[2]=ax[2]/n*s; out[3]=std::cos(ang*0.5f);
}

// Least-squares affine M (column-major) mapping src -> dst (both nv*3). Recovers the entity's model->world placement
// (rotation + SCALE + translation) from the model bind pose (hzRestPos) and the world bind pose (ExportMesh.positions),
// so a skinned rig lands AND scales correctly in Blender. A translation-only rig left the (often huge) model at model
// scale = a giant featureless blob ("renders as spheres").
inline M4 computeAffine(const float* src, const float* dst, size_t nv){
    double ATA[4][4]={{0}}, ATb[3][4]={{0}};
    for(size_t i=0;i<nv;i++){ double p[4]={src[i*3],src[i*3+1],src[i*3+2],1.0};
        for(int a=0;a<4;a++){ for(int b=0;b<4;b++) ATA[a][b]+=p[a]*p[b];
            for(int c=0;c<3;c++) ATb[c][a]+=p[a]*(double)dst[i*3+c]; } }
    M4 M=m4id();
    for(int c=0;c<3;c++){
        double A[4][5];
        for(int a=0;a<4;a++){ for(int b=0;b<4;b++) A[a][b]=ATA[a][b]; A[a][4]=ATb[c][a]; }
        bool ok=true;
        for(int col=0;col<4;col++){
            int piv=col; for(int r=col+1;r<4;r++) if(std::fabs(A[r][col])>std::fabs(A[piv][col])) piv=r;
            if(std::fabs(A[piv][col])<1e-12){ ok=false; break; }
            for(int k=0;k<5;k++) std::swap(A[col][k],A[piv][k]);
            for(int r=0;r<4;r++) if(r!=col){ double f=A[r][col]/A[col][col]; for(int k=0;k<5;k++) A[r][k]-=f*A[col][k]; }
        }
        if(!ok) return m4id();
        for(int a=0;a<4;a++){ double x=A[a][4]/A[a][a]; M[a*4+c]=(float)x; }
    }
    M[3]=0;M[7]=0;M[11]=0;M[15]=1; return M;
}

// ── FULL exporter: hslcook::ExportMesh[] -> glTF 2.0 with SKINS + SKELETAL animation + NODE animation ──────────────
// This is the round-trip's authoritative path (the exact data the cooker ships): skinned meshes become a glTF skin
// (armature + JOINTS_0/WEIGHTS_0 + per-joint TRS animation sampled from the ACL clip); node-animated meshes (spin /
// sway / translate / scale / pose) become glTF node-transform animations. Material/UV anims (uvScroll/flipbook/VAT)
// aren't glTF-native — they're recorded in the sidecar so the re-cook restores them.
inline bool exportEnvFull(const std::vector<hslcook::ExportMesh>& meshes, const std::string& outDir,
                          const std::string& envName, const std::string& metaExtra = "") {
    namespace fs = std::filesystem; std::error_code ec;
    fs::create_directories(fs::path(outDir) / "textures", ec);

    std::vector<uint8_t> bin;
    std::vector<std::string> Jnodes, Jmeshes, Jmaterials, Jacc, Jbv, Jtex, Jimg, Jskins, Janims;
    std::string meta; std::vector<int> sceneNodes;

    auto join=[](const std::vector<std::string>& v){ std::string s; for(size_t i=0;i<v.size();++i){ if(i)s+=","; s+=v[i]; } return s; };
    auto addBV=[&](size_t off,size_t len,int target)->int{
        char t[160]; snprintf(t,sizeof t,"{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu%s}",off,len,
            target?(target==34963?",\"target\":34963":",\"target\":34962"):""); Jbv.push_back(t); return (int)Jbv.size()-1; };
    auto addAccF=[&](const std::vector<float>& vals,int comps,bool minmax)->int{
        pad4(bin); size_t off=bin.size(); std::vector<float> mn(comps,1e30f),mx(comps,-1e30f);
        for(size_t e=0;e<vals.size()/comps;++e)for(int c=0;c<comps;c++){ float v=vals[e*comps+c]; if(v<mn[c])mn[c]=v; if(v>mx[c])mx[c]=v; uint8_t b[4]; memcpy(b,&v,4); bin.insert(bin.end(),b,b+4); }
        int bvI=addBV(off,bin.size()-off,0); const char* ty=comps==1?"SCALAR":comps==2?"VEC2":comps==3?"VEC3":comps==4?"VEC4":"MAT4";
        std::string a="{\"bufferView\":"+std::to_string(bvI)+",\"componentType\":5126,\"count\":"+std::to_string(vals.size()/comps)+",\"type\":\""+ty+"\"";
        if(minmax){ a+=",\"min\":["; for(int c=0;c<comps;c++){if(c)a+=",";char n[32];snprintf(n,sizeof n,"%g",mn[c]);a+=n;} a+="],\"max\":["; for(int c=0;c<comps;c++){if(c)a+=",";char n[32];snprintf(n,sizeof n,"%g",mx[c]);a+=n;} a+="]"; }
        a+="}"; Jacc.push_back(a); return (int)Jacc.size()-1; };
    auto addAccU8N=[&](const std::vector<uint8_t>& vals,int comps,bool normalized)->int{
        pad4(bin); size_t off=bin.size(); bin.insert(bin.end(),vals.begin(),vals.end());
        int bvI=addBV(off,bin.size()-off,34962); const char* ty=comps==4?"VEC4":comps==3?"VEC3":"SCALAR";
        std::string a="{\"bufferView\":"+std::to_string(bvI)+",\"componentType\":5121,"+(normalized?std::string("\"normalized\":true,"):std::string())+"\"count\":"+std::to_string(vals.size()/comps)+",\"type\":\""+ty+"\"}";
        Jacc.push_back(a); return (int)Jacc.size()-1; };
    auto addAccIdx=[&](const std::vector<uint32_t>& idx)->int{
        pad4(bin); size_t off=bin.size(); for(uint32_t v:idx){uint8_t b[4];memcpy(b,&v,4);bin.insert(bin.end(),b,b+4);}
        int bvI=addBV(off,bin.size()-off,34963);
        Jacc.push_back("{\"bufferView\":"+std::to_string(bvI)+",\"componentType\":5125,\"count\":"+std::to_string(idx.size())+",\"type\":\"SCALAR\"}"); return (int)Jacc.size()-1; };
    auto addImage=[&](const std::vector<uint8_t>& rgba,uint32_t w,uint32_t h,const std::string& fn)->int{
        if(rgba.size()<(size_t)w*h*4||w==0||h==0) return -1;
        if(!writePng((fs::path(outDir)/"textures"/fn).string(),rgba.data(),(int)w,(int)h)) return -1;
        Jimg.push_back("{\"uri\":\"textures/"+urlEncode(fn)+"\"}"); Jtex.push_back("{\"source\":"+std::to_string((int)Jimg.size()-1)+"}"); return (int)Jtex.size()-1; };

    for (size_t mi=0; mi<meshes.size(); ++mi) {
        const hslcook::ExportMesh& m = meshes[mi];
        bool skinned = m.hzFrames>1 && m.hzJointCount>0 && m.hzRestPos.size()>=9 && m.hzBoneIdx.size()>=(m.hzRestPos.size()/3)*4;
        // POSITION = the WORLD bind pose for BOTH (static + skinned). For a skinned mesh whose world bake matches the
        // rest verts we skin from the WORLD POSITION (joint rig + inverseBind carry the deform); only if no world bake
        // exists do we fall back to the model rest pose.
        const std::vector<float>& srcPos = (skinned && m.positions.size()!=m.hzRestPos.size()) ? m.hzRestPos : m.positions;
        if (srcPos.size()<9 || m.indices.size()<3) continue;
        size_t nv = srcPos.size()/3;

        // pivot: skinned -> recover world placement via (worldCentroid - restCentroid); spin/sway -> the rot pivot; else centroid
        double cx=0,cy=0,cz=0; for(size_t v=0;v<nv;v++){ cx+=srcPos[v*3]; cy+=srcPos[v*3+1]; cz+=srcPos[v*3+2]; } cx/=nv;cy/=nv;cz/=nv;
        float pivot[3]={(float)cx,(float)cy,(float)cz};
        if (!skinned && (m.rotAnim||m.rotOsc)) { pivot[0]=m.rotPivot[0]; pivot[1]=m.rotPivot[1]; pivot[2]=m.rotPivot[2]; }
        float skinRoot[3]={0,0,0};
        if (skinned){ double wx=0,wy=0,wz=0; size_t wnv=m.positions.size()/3; if(wnv){ for(size_t v=0;v<wnv;v++){wx+=m.positions[v*3];wy+=m.positions[v*3+1];wz+=m.positions[v*3+2];} wx/=wnv;wy/=wnv;wz/=wnv; skinRoot[0]=(float)(wx-cx); skinRoot[1]=(float)(wy-cy); skinRoot[2]=(float)(wz-cz);} }
        // BACKDROP detection: skyboxes + huge enclosing domes (vista/stars) dwarf the editable interior in Blender's
        // viewport. Prefix their OBJECT name so they're obvious in the outliner and one-click hideable (H) to edit the env.
        float emn[3]={1e30f,1e30f,1e30f}, emx[3]={-1e30f,-1e30f,-1e30f};
        for(size_t v=0;v<nv;v++)for(int k=0;k<3;k++){ float c=srcPos[v*3+k]; if(c<emn[k])emn[k]=c; if(c>emx[k])emx[k]=c; }
        float maxExtent=std::max(emx[0]-emn[0],std::max(emx[1]-emn[1],emx[2]-emn[2]));
        bool backdrop = m.skybox || maxExtent>3000.f;
        std::string dispName = backdrop ? ("BACKDROP_"+m.name) : m.name;

        // POSITION (local to pivot for non-skinned; model-space for skinned, placed by the skinRoot node)
        std::vector<float> pos(nv*3);
        for(size_t v=0;v<nv;v++){ pos[v*3]=srcPos[v*3]-(skinned?0.f:pivot[0]); pos[v*3+1]=srcPos[v*3+1]-(skinned?0.f:pivot[1]); pos[v*3+2]=srcPos[v*3+2]-(skinned?0.f:pivot[2]); }
        int posAcc=addAccF(pos,3,true);
        int uvAcc=-1; if(m.uvs.size()>=nv*2){ std::vector<float> uv(m.uvs.begin(),m.uvs.begin()+nv*2); uvAcc=addAccF(uv,2,false); }
        int colAcc=-1; if(m.iblVertCol.size()>=nv*4){ std::vector<uint8_t> c(m.iblVertCol.begin(),m.iblVertCol.begin()+nv*4); colAcc=addAccU8N(c,4,true); }
        int idxAcc=addAccIdx(m.indices);
        int jntAcc=-1,wgtAcc=-1;
        if (skinned){ std::vector<uint8_t> ji(m.hzBoneIdx.begin(),m.hzBoneIdx.begin()+nv*4), jw(m.hzBoneWgt.begin(),m.hzBoneWgt.begin()+nv*4);
            jntAcc=addAccU8N(ji,4,false); wgtAcc=addAccU8N(jw,4,true); }

        // material + base texture
        int baseTex = (!m.rgba.empty() && m.w && m.h) ? addImage(m.rgba,m.w,m.h,"base_"+std::to_string(mi)+".png") : -1;
        int normTex = (m.hasNormal && !m.normalRGBA.empty()) ? addImage(m.normalRGBA,m.normalW,m.normalH,"norm_"+std::to_string(mi)+".png") : -1;
        std::string pbr="\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1";
        if(baseTex>=0) pbr+=",\"baseColorTexture\":{\"index\":"+std::to_string(baseTex)+"}";
        else { char bc[96]; snprintf(bc,sizeof bc,",\"baseColorFactor\":[%g,%g,%g,1]",m.matTint[0],m.matTint[1],m.matTint[2]); pbr+=bc; }
        pbr+="}";
        std::string matJ="{\"name\":\""+jesc(m.name)+"\","+pbr;
        if(normTex>=0) matJ+=",\"normalTexture\":{\"index\":"+std::to_string(normTex)+"}";
        if(m.blend||m.additive) matJ+=",\"alphaMode\":\"BLEND\""; else if(m.alphaTest) matJ+=",\"alphaMode\":\"MASK\"";
        if(m.doubleSided) matJ+=",\"doubleSided\":true";
        matJ+="}"; Jmaterials.push_back(matJ); int matIdx=(int)Jmaterials.size()-1;

        std::string attrs="\"POSITION\":"+std::to_string(posAcc);
        if(uvAcc>=0) attrs+=",\"TEXCOORD_0\":"+std::to_string(uvAcc);
        if(colAcc>=0) attrs+=",\"COLOR_0\":"+std::to_string(colAcc);
        if(skinned){ attrs+=",\"JOINTS_0\":"+std::to_string(jntAcc)+",\"WEIGHTS_0\":"+std::to_string(wgtAcc); }
        Jmeshes.push_back("{\"name\":\""+jesc(dispName)+"\",\"primitives\":[{\"attributes\":{"+attrs+"},\"indices\":"+std::to_string(idxAcc)+",\"material\":"+std::to_string(matIdx)+"}]}");
        int meshIdx=(int)Jmeshes.size()-1;

        int meshNode=-1, skinIdx=-1;
        if (skinned){
            int jc=m.hzJointCount;
            // joint nodes get contiguous indices AFTER the upcoming mesh node + skinRoot node.
            // layout we emit: [skinRoot][joint0..jc-1]; mesh node added separately. Indices:
            int skinRootNode = (int)Jnodes.size();          // reserve
            int jbase = skinRootNode + 1;
            std::vector<std::vector<int>> kids(jc); int rootJoint=0;
            for(int j=0;j<jc;j++){ int p=(j<(int)m.hzParents.size())?m.hzParents[j]:-1; if(p<0) rootJoint=j; else if(p<jc) kids[p].push_back(jbase+j); }
            // rig root carries the FULL model->world placement T (rotation+scale+translation) recovered from the bind
            // poses; the joints (model space) hang under it and inverseBind stays inverse(world[j]) (no T) so at bind
            // displayed = T * hzRestPos = the world-placed, correctly-scaled mesh. (Was translation-only -> giant blob.)
            M4 T;
            if (m.positions.size()==m.hzRestPos.size() && nv>=4) T=computeAffine(m.hzRestPos.data(), m.positions.data(), nv);
            else { T=m4id(); T[12]=skinRoot[0]+pivot[0]; T[13]=skinRoot[1]+pivot[1]; T[14]=skinRoot[2]+pivot[2]; }
            { std::string mtx; for(int k=0;k<16;k++){ if(k)mtx+=","; char n[32]; snprintf(n,sizeof n,"%g",T[k]); mtx+=n; }
              Jnodes.push_back("{\"name\":\""+jesc(dispName)+"_rig\",\"matrix\":["+mtx+"],\"children\":["+std::to_string(jbase+rootJoint)+"]}"); }
            // joint nodes (rest local TRS; jointQuat is WXYZ -> XYZW)
            std::vector<float> ibm; ibm.reserve(jc*16); std::vector<M4> world(jc);
            for(int j=0;j<jc;j++){
                float p[3]={0,0,0},q[4]={0,0,0,1},s[3]={1,1,1};
                if((size_t)j*3+2<m.hzJointPos.size()){ p[0]=m.hzJointPos[j*3];p[1]=m.hzJointPos[j*3+1];p[2]=m.hzJointPos[j*3+2]; }
                if((size_t)j*4+3<m.hzJointQuat.size()){ q[0]=m.hzJointQuat[j*4+1];q[1]=m.hzJointQuat[j*4+2];q[2]=m.hzJointQuat[j*4+3];q[3]=m.hzJointQuat[j*4+0]; }
                if((size_t)j<m.hzJointScale.size()){ float sc=m.hzJointScale[j]; if(sc!=0){s[0]=s[1]=s[2]=sc;} }
                M4 local=m4trs(p,q,s); int par=(j<(int)m.hzParents.size())?m.hzParents[j]:-1;
                world[j]=(par>=0&&par<jc)?m4mul(world[par],local):local;
                std::string ch; for(size_t k=0;k<kids[j].size();++k){ if(k)ch+=","; ch+=std::to_string(kids[j][k]); }
                // Build the joint node with an UNBOUNDED std::string: a hub joint can parent dozens of children,
                // and the old fixed char[320] snprintf TRUNCATED the "children":[...] list for such joints -> the
                // closing ]} was lost -> invalid glTF JSON (cyberhome: joint "j6" parents ~40 joints). Only the
                // fixed-width numeric prefix goes through snprintf; the variable-length children append as a string.
                char nb[224]; snprintf(nb,sizeof nb,"{\"name\":\"j%d\",\"translation\":[%g,%g,%g],\"rotation\":[%g,%g,%g,%g],\"scale\":[%g,%g,%g]",
                    j,p[0],p[1],p[2],q[0],q[1],q[2],q[3],s[0],s[1],s[2]);
                std::string node=nb; if(!kids[j].empty()) node+=",\"children\":["+ch+"]"; node+="}";
                Jnodes.push_back(node);
                // inverseBind = inverse of the joint's GLOBAL rest (rig T * world[j]) so it matches exactly what glTF
                // composes from the node hierarchy -> at bind the mesh shows the WORLD POSITION (no Blender mismatch).
                M4 inv=m4inv(m4mul(T,world[j])); for(int k=0;k<16;k++) ibm.push_back(inv[k]);
            }
            int ibmAcc=addAccF(ibm,16,false);
            std::string jl; for(int j=0;j<jc;j++){ if(j)jl+=","; jl+=std::to_string(jbase+j); }
            Jskins.push_back("{\"name\":\""+jesc(m.name)+"_skin\",\"skeleton\":"+std::to_string(jbase+rootJoint)+",\"joints\":["+jl+"],\"inverseBindMatrices\":"+std::to_string(ibmAcc)+"}");
            skinIdx=(int)Jskins.size()-1;
            // skeletal animation: per-joint rotation/translation/scale samplers from hzTrsLocal (frames*jc*10: q xyzw, t3, s3)
            int frames=m.hzFrames; float fps=m.hzFps>0?m.hzFps:30.f;
            std::vector<float> times(frames); for(int f=0;f<frames;f++) times[f]=(float)f/fps;
            int timeAcc=addAccF(times,1,true);
            std::string chans, samps; int sCount=0;
            M4 Tinv=m4inv(T);
            for(int j=0;j<jc;j++){
                std::vector<float> rot(frames*4),tra(frames*3),scl(frames*3);
                // Root joint (no parent) trsLocal = WORLD TRS; skinRoot T carries world placement.
                // Store T^{-1}*world so the joint node's local animation is T-relative (correct glTF skinning).
                bool isRoot=(j<(int)m.hzParents.size())?m.hzParents[j]<0:(j==0);
                for(int f=0;f<frames;f++){ size_t b=((size_t)f*jc+j)*10;
                    if(b+9<m.hzTrsLocal.size()){
                        if(isRoot){
                            float tq[4]={m.hzTrsLocal[b],m.hzTrsLocal[b+1],m.hzTrsLocal[b+2],m.hzTrsLocal[b+3]};
                            float tt[3]={m.hzTrsLocal[b+4],m.hzTrsLocal[b+5],m.hzTrsLocal[b+6]};
                            float ts[3]={m.hzTrsLocal[b+7],m.hzTrsLocal[b+8],m.hzTrsLocal[b+9]};
                            M4 W=m4trs(tt,tq,ts); M4 L=m4mul(Tinv,W);
                            float lq[4],lt[3],ls[3]; m4decompose(L,lq,lt,ls);
                            for(int k=0;k<4;k++) rot[f*4+k]=lq[k]; for(int k=0;k<3;k++) tra[f*3+k]=lt[k]; for(int k=0;k<3;k++) scl[f*3+k]=ls[k];
                        } else {
                            for(int k=0;k<4;k++) rot[f*4+k]=m.hzTrsLocal[b+k]; for(int k=0;k<3;k++) tra[f*3+k]=m.hzTrsLocal[b+4+k]; for(int k=0;k<3;k++) scl[f*3+k]=m.hzTrsLocal[b+7+k]; }
                    } else { rot[f*4+3]=1; scl[f*3]=scl[f*3+1]=scl[f*3+2]=1; } }
                int rAcc=addAccF(rot,4,false), tAcc=addAccF(tra,3,false), scAcc=addAccF(scl,3,false);
                int jn=jbase+j;
                for (auto pr : { std::make_pair(rAcc,"rotation"), std::make_pair(tAcc,"translation"), std::make_pair(scAcc,"scale") }){
                    if(sCount){chans+=",";samps+=",";}
                    samps+="{\"input\":"+std::to_string(timeAcc)+",\"output\":"+std::to_string(pr.first)+",\"interpolation\":\"LINEAR\"}";
                    chans+="{\"sampler\":"+std::to_string(sCount)+",\"target\":{\"node\":"+std::to_string(jn)+",\"path\":\""+pr.second+"\"}}";
                    sCount++;
                }
            }
            Janims.push_back("{\"name\":\""+jesc(m.name)+"_clip\",\"channels\":["+chans+"],\"samplers\":["+samps+"]}");
            // skinned MESH node (its own transform is ignored for skinning; identity at origin)
            meshNode=(int)Jnodes.size();
            Jnodes.push_back("{\"name\":\""+jesc(dispName)+"\",\"mesh\":"+std::to_string(meshIdx)+",\"skin\":"+std::to_string(skinIdx)+"}");
            sceneNodes.push_back(skinRootNode); sceneNodes.push_back(meshNode);
        } else {
            // non-skinned mesh node at the pivot
            meshNode=(int)Jnodes.size();
            char ntb[160]; snprintf(ntb,sizeof ntb,",\"mesh\":%d,\"translation\":[%g,%g,%g]}",meshIdx,pivot[0],pivot[1],pivot[2]);
            std::string nt="{\"name\":\""+jesc(dispName)+"\""+ntb;   // name via unbounded std::string (a long mesh name must NOT truncate the node -> malformed JSON)
            Jnodes.push_back(nt); sceneNodes.push_back(meshNode);
            // NODE animation (spin / sway / translate / scale / pose) -> a node-transform animation on this node
            bool anim=false; std::string chans,samps; int sCount=0; const int NS=24;
            auto pushChan=[&](int outAcc,int timeAcc,const char* path){ if(sCount){chans+=",";samps+=",";}
                samps+="{\"input\":"+std::to_string(timeAcc)+",\"output\":"+std::to_string(outAcc)+",\"interpolation\":\"LINEAR\"}";
                chans+="{\"sampler\":"+std::to_string(sCount)+",\"target\":{\"node\":"+std::to_string(meshNode)+",\"path\":\""+path+"\"}}"; sCount++; };
            if (m.rotAnim && std::fabs(m.rotOmega)>1e-6f){   // continuous spin: sample one full revolution
                float period=(float)(2.0*M_PI/std::fabs(m.rotOmega)); std::vector<float> times(NS+1),rot((NS+1)*4);
                for(int f=0;f<=NS;f++){ float t=period*f/NS; times[f]=t; float q[4]; quatAxisAngle(m.rotAxis,m.rotOmega*t,q); for(int k=0;k<4;k++)rot[f*4+k]=q[k]; }
                pushChan(addAccF(rot,4,false), addAccF(times,1,true), "rotation"); anim=true;
            } else if (m.rotOsc && m.rotPeriod>1e-6f){      // sway: angle=(amp/2)(1-cos(2pi t/period))
                std::vector<float> times(NS+1),rot((NS+1)*4);
                for(int f=0;f<=NS;f++){ float t=m.rotPeriod*f/NS; times[f]=t; float ang=(m.rotAmp*0.5f)*(1.f-std::cos((float)(2.0*M_PI)*f/NS)); float q[4]; quatAxisAngle(m.rotAxis,ang,q); for(int k=0;k<4;k++)rot[f*4+k]=q[k]; }
                pushChan(addAccF(rot,4,false), addAccF(times,1,true), "rotation"); anim=true;
            }
            if (m.transAnim && m.transN>0 && (int)m.transFrames.size()>=m.transN*3){
                float loop=m.transLoop>0?m.transLoop:1.f; std::vector<float> times(m.transN),tra(m.transN*3);
                for(int f=0;f<m.transN;f++){ times[f]=loop*f/std::max(1,m.transN-1); tra[f*3]=pivot[0]+m.transFrames[f*3]; tra[f*3+1]=pivot[1]+m.transFrames[f*3+1]; tra[f*3+2]=pivot[2]+m.transFrames[f*3+2]; }
                pushChan(addAccF(tra,3,false), addAccF(times,1,true), "translation"); anim=true;
            }
            if (m.scaleAnim && m.scaleN>0 && (int)m.scaleFrames.size()>=m.scaleN*3){
                float loop=m.scaleLoop>0?m.scaleLoop:1.f; std::vector<float> times(m.scaleN),scl(m.scaleN*3);
                for(int f=0;f<m.scaleN;f++){ times[f]=loop*f/std::max(1,m.scaleN-1); for(int k=0;k<3;k++) scl[f*3+k]=m.scaleFrames[f*3+k]; }
                pushChan(addAccF(scl,3,false), addAccF(times,1,true), "scale"); anim=true;
            } else if (m.poseAnim){     // start<->end scale over poseDuration
                float dur=m.poseDuration>0?m.poseDuration:1.f; std::vector<float> times={0.f,dur},scl(6);
                for(int k=0;k<3;k++){ scl[k]=m.poseStartScale[k]; scl[3+k]=m.poseEndScale[k]; }
                pushChan(addAccF(scl,3,false), addAccF(times,1,true), "scale"); anim=true;
            }
            if (anim) Janims.push_back("{\"name\":\""+jesc(m.name)+"_node\",\"channels\":["+chans+"],\"samplers\":["+samps+"]}");
        }

        // sidecar: identity + the non-glTF-native anims the re-cook must restore
        if(!meta.empty()) meta+=",";
        meta+="{\"node\":"+std::to_string(meshNode)+",\"srcMeshIndex\":"+std::to_string(mi)+",\"name\":\""+jesc(m.name)+"\",\"skinned\":"+(skinned?"true":"false")+",\"skybox\":"+std::string(m.skybox?"true":"false");
        if(m.uvScroll){ char u[96]; snprintf(u,sizeof u,",\"uvScroll\":[%g,%g]",m.uvRate[0],m.uvRate[1]); meta+=u; }
        if(m.flipbook){ char fb[96]; snprintf(fb,sizeof fb,",\"flipbook\":{\"cols\":%d,\"rows\":%d,\"fps\":%g}",m.flipCols,m.flipRows,m.flipFps); meta+=fb; }
        if(m.vatFrames>0) meta+=",\"vatFrames\":"+std::to_string(m.vatFrames);
        meta+="}";
    }

    if (sceneNodes.empty()) return false;
    std::string sn; for(size_t i=0;i<sceneNodes.size();++i){ if(i)sn+=","; sn+=std::to_string(sceneNodes[i]); }

    std::string gltf="{\"asset\":{\"version\":\"2.0\",\"generator\":\"HSR Renderer (V79 Quest Home Porter)\"},";
    gltf+="\"scene\":0,\"scenes\":[{\"name\":\""+jesc(envName)+"\",\"nodes\":["+sn+"]}],";
    gltf+="\"nodes\":["+join(Jnodes)+"],";
    gltf+="\"meshes\":["+join(Jmeshes)+"],";
    gltf+="\"materials\":["+join(Jmaterials)+"],";
    if(!Jskins.empty()) gltf+="\"skins\":["+join(Jskins)+"],";
    if(!Janims.empty()) gltf+="\"animations\":["+join(Janims)+"],";
    if(!Jtex.empty()) gltf+="\"textures\":["+join(Jtex)+"],\"images\":["+join(Jimg)+"],";
    gltf+="\"accessors\":["+join(Jacc)+"],";
    gltf+="\"bufferViews\":["+join(Jbv)+"],";
    gltf+="\"buffers\":[{\"uri\":\""+urlEncode(envName)+".bin\",\"byteLength\":"+std::to_string(bin.size())+"}]}";

    { FILE* f=fopen((fs::path(outDir)/(envName+".bin")).string().c_str(),"wb"); if(!f)return false; if(!bin.empty())fwrite(bin.data(),1,bin.size(),f); fclose(f); }
    { FILE* f=fopen((fs::path(outDir)/(envName+".gltf")).string().c_str(),"wb"); if(!f)return false; fwrite(gltf.data(),1,gltf.size(),f); fclose(f); }
    { FILE* f=fopen((fs::path(outDir)/(envName+".blendmeta.json")).string().c_str(),"wb"); if(f){
        std::string s="{\"env\":\""+jesc(envName)+"\",\"objects\":["+meta+"]"; if(!metaExtra.empty()) s+=","+metaExtra; s+="}";
        fwrite(s.data(),1,s.size(),f); fclose(f); } }
    { FILE* f=fopen((fs::path(outDir)/"HOW_TO_OPEN_IN_BLENDER.txt").string().c_str(),"wb"); if(f){
        std::string r =
          "HOW TO OPEN THIS IN BLENDER\n"
          "===========================\n\n"
          "This is a glTF 2.0 project, NOT a .blend file. Do NOT use File > Open\n"
          "(that only opens .blend files and will say \"not a valid blend file\").\n\n"
          "  1. Open Blender.\n"
          "  2. File > Import > glTF 2.0 (.glb/.gltf)\n"
          "  3. Pick:  " + envName + ".gltf\n"
          "  4. Keep " + envName + ".bin and the textures/ folder NEXT TO the .gltf\n"
          "     (Blender reads geometry from the .bin and images from textures/).\n\n"
          "Animated/skinned objects come in as an Armature + a skinned Mesh. Press the\n"
          "Play button on the timeline to see the animation. Bones may look like small\n"
          "spheres/sticks - that's Blender's normal bone display, the mesh is parented to them.\n\n"
          "** SEEING A GIANT SPHERE / can't find the room? ** Many envs have a huge skybox\n"
          "DOME + starfield that ENCLOSE the scene (often 20,000+ units across). On import\n"
          "Blender frames that dome, so you're staring at the inside of a sphere. Those\n"
          "objects are named  BACKDROP_*  - select them in the Outliner and press H to hide\n"
          "them (or move them to their own Collection). The actual editable rooms/props are\n"
          "the small objects inside. Use View > Frame Selected on a room to zoom to it.\n\n"
          "When done editing: File > Export > glTF 2.0, then load that .gltf back into the\n"
          "HSR editor (drag it onto the window, or the \"Blender ->\" button) and cook the APK.\n"
          "Keep " + envName + ".blendmeta.json next to it - it carries the env structure for re-cook.\n";
        fwrite(r.data(),1,r.size(),f); fclose(f); } }
    return true;
}

} // namespace gltfexport
