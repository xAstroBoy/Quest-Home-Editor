// ── gltf_import.h — re-import a BLENDER-EDITED glTF 2.0 project back into the editor ─────────────────────────────
// The other half of the Blender round-trip (see gltf_export.h): parse a standard glTF 2.0 (.gltf + .bin + textures,
// or a self-contained file with data:-URI / bufferView-embedded buffers & images) that came out of Blender, recover
// each object's WORLD geometry (local positions × the node's TRS so Blender moves/rotations/scales are captured), UVs,
// vertex colours, indices, and the base-colour texture, into MeshData[] the editor + cooker already understand. A
// sibling <stem>.blendmeta.json (written on export) restores the original object names. Reuses the project's tinyjson
// + stb_image (PNG/JPEG via stbi_load_from_memory). The user can then tweak with the editor's HSL tools and re-cook.
#pragma once
#include "../core/types.h"
#include "../core/tinyjson.h"
#include "stb_image.h"
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace gltfimport {

inline std::vector<uint8_t> readFile(const std::string& p){
    std::ifstream f(p, std::ios::binary); if(!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline int b64v(char c){ if(c>='A'&&c<='Z')return c-'A'; if(c>='a'&&c<='z')return c-'a'+26; if(c>='0'&&c<='9')return c-'0'+52; if(c=='+')return 62; if(c=='/')return 63; return -1; }
inline std::vector<uint8_t> b64decode(const std::string& s){ std::vector<uint8_t> o; int val=0,bits=-8; for(char c:s){ if(c=='=')break; int d=b64v(c); if(d<0)continue; val=(val<<6)+d; bits+=6; if(bits>=0){ o.push_back((uint8_t)((val>>bits)&0xFF)); bits-=8; } } return o; }
// glTF URIs are PERCENT-ENCODED per the spec (RFC 3986): a space is "%20", etc. Blender and every
// conformant exporter encode them, so a model named "My Home" writes uri "My%20Home.bin". Decode before
// hitting the filesystem (the old code fed "My%20Home.bin" straight to open() -> file-not-found -> the
// whole .gltf "refused" to load). Also decode '+' as a literal '+' (glTF does NOT use form-encoding).
inline std::string percentDecode(const std::string& s){
    std::string o; o.reserve(s.size());
    auto hex=[](char c)->int{ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
    for(size_t i=0;i<s.size();++i){
        if(s[i]=='%' && i+2<s.size()){ int h=hex(s[i+1]),l=hex(s[i+2]); if(h>=0&&l>=0){ o.push_back((char)((h<<4)|l)); i+=2; continue; } }
        o.push_back(s[i]);
    }
    return o;
}
// Resolve an external glTF resource robustly: try the percent-DECODED path, then the RAW uri (in case a
// non-conformant exporter — like our own older export — wrote a literal space), then the decoded BASENAME
// in `dir` (handles a .gltf moved without its subfolder layout). Empty only if none exist.
inline std::vector<uint8_t> readExternal(const std::string& uri, const std::string& dir){
    namespace fs=std::filesystem; std::error_code ec;
    std::string dec=percentDecode(uri);
    for(const std::string& cand : { dec, uri }){
        auto p=(fs::path(dir)/cand);
        if(fs::exists(p,ec)){ auto b=readFile(p.string()); if(!b.empty()) return b; }
    }
    // last resort: just the filename (decoded) directly under dir
    std::string base=fs::path(dec).filename().string();
    if(!base.empty()){ auto p=fs::path(dir)/base; if(fs::exists(p,ec)){ auto b=readFile(p.string()); if(!b.empty()) return b; } }
    return {};
}
inline std::vector<uint8_t> loadBuffer(const std::string& uri, const std::string& dir){
    if (uri.rfind("data:",0)==0){ size_t c=uri.find("base64,"); return c!=std::string::npos ? b64decode(uri.substr(c+7)) : std::vector<uint8_t>(); }
    return readExternal(uri, dir);
}

// Returns true if the file is a plain glTF 2.0 (NOT a V79 .gltf.ovrscene, which gltf_loader.h handles).
inline bool isPlainGltf(const std::string& path){
    std::string p=path; for(auto&c:p)c=(char)tolower((unsigned char)c);
    if (p.size()>9 && p.substr(p.size()-9)==".ovrscene") return false;
    return (p.size()>5 && p.substr(p.size()-5)==".gltf") || (p.size()>4 && p.substr(p.size()-4)==".glb");
}

// Reconstructed animation per imported object (parallel to `out`). Maps 1:1 to the cooker's hz* fields so a
// Blender-edited skin/clip re-cooks to a faithful V205 HZANIM. Empty (skinned=false, frames=0) for static objects.
struct ImportedAnim {
    bool skinned=false;
    std::vector<float> jointPos;     // jointCount*3
    std::vector<float> jointQuat;    // jointCount*4  WXYZ (hz/HZAN:SKEL order)
    std::vector<float> jointScale;   // jointCount    (uniform)
    std::vector<int>   parents;      // jointCount
    std::vector<uint8_t> boneIdx, boneWgt;  // nv*4
    std::vector<float> trsLocal;     // frames*jointCount*10 (quat XYZW + t3 + s3)  -> ACL
    std::vector<float> restPos;      // nv*3 model-space (NOT world-baked)
    int jointCount=0, frames=0; float fps=0;
};

inline bool importEnv(const std::string& gltfPath, std::vector<MeshData>& out, std::vector<ImportedAnim>* anims = nullptr) {
    namespace fs = std::filesystem;
    std::string dir = fs::path(gltfPath).parent_path().string();
    std::vector<uint8_t> jb = readFile(gltfPath); if (jb.empty()) return false;

    // .glb: JSON chunk is after the 12-byte header + 8-byte chunk header; a following BIN chunk = buffer 0.
    std::string js; std::vector<uint8_t> glbBin; bool isGlb=false;
    if (jb.size()>12 && jb[0]=='g'&&jb[1]=='l'&&jb[2]=='T'&&jb[3]=='F'){
        isGlb=true; size_t off=12;
        while (off+8<=jb.size()){ uint32_t len; memcpy(&len,&jb[off],4); uint32_t typ; memcpy(&typ,&jb[off+4],4); off+=8;
            if (off+len>jb.size()) break;
            if (typ==0x4E4F534A) js.assign((char*)&jb[off], len);                 // 'JSON'
            else if (typ==0x004E4942) glbBin.assign(jb.begin()+off, jb.begin()+off+len); // 'BIN\0'
            off+=len; }
    } else js.assign((char*)jb.data(), jb.size());

    tinyjson::Value root; try { root = tinyjson::parse(js); } catch(...) { return false; }
    if (!root.has("meshes") || !root.has("accessors") || !root.has("bufferViews")) return false;

    std::vector<std::vector<uint8_t>> buffers;
    if (root.has("buffers")){ const auto& B=root["buffers"]; for(size_t i=0;i<B.size();++i){
        std::string uri=B[i].has("uri")?B[i]["uri"].asString():"";
        if (uri.empty() && isGlb && i==0) buffers.push_back(glbBin); else buffers.push_back(loadBuffer(uri,dir)); } }

    const auto& BV = root["bufferViews"]; const auto& AC = root["accessors"];
    auto compSize=[](int ct)->int{ switch(ct){case 5120:case 5121:return 1;case 5122:case 5123:return 2;case 5125:case 5126:return 4;} return 0; };
    auto typeComps=[](const std::string& t)->int{ if(t=="SCALAR")return 1; if(t=="VEC2")return 2; if(t=="VEC3")return 3; if(t=="VEC4")return 4; if(t=="MAT4")return 16; return 0; };

    auto readF=[&](int ai, std::vector<float>& dst, int& comps)->bool{
        if(ai<0||ai>=(int)AC.size()) return false; const auto& a=AC[ai];
        int ct=(int)a["componentType"].asInt(); comps=typeComps(a["type"].asString()); size_t count=(size_t)a["count"].asInt();
        bool norm=a.has("normalized")&&a["normalized"].asBool();
        if(!a.has("bufferView")) return false; const auto& bv=BV[(size_t)a["bufferView"].asInt()]; int buf=(int)bv["buffer"].asInt();
        size_t bOff=(bv.has("byteOffset")?(size_t)bv["byteOffset"].asInt():0)+(a.has("byteOffset")?(size_t)a["byteOffset"].asInt():0);
        int cs=compSize(ct); if(cs==0||comps==0||buf<0||buf>=(int)buffers.size()) return false;
        size_t stride=(bv.has("byteStride")&&bv["byteStride"].asInt()>0)?(size_t)bv["byteStride"].asInt():(size_t)cs*comps;
        const auto& B=buffers[buf]; if(bOff+(count?(count-1)*stride+(size_t)cs*comps:0)>B.size()) return false;
        dst.resize(count*comps);
        for(size_t e=0;e<count;++e){ const uint8_t* p=B.data()+bOff+e*stride; for(int c=0;c<comps;++c){ const uint8_t* q=p+(size_t)c*cs; float v=0;
            switch(ct){ case 5126:{ float f; memcpy(&f,q,4); v=f; } break;
                case 5125:{ uint32_t u; memcpy(&u,q,4); v=norm?(float)u/4294967295.0f:(float)u; } break;
                case 5123:{ uint16_t u; memcpy(&u,q,2); v=norm?(float)u/65535.0f:(float)u; } break;
                case 5122:{ int16_t u; memcpy(&u,q,2); v=norm?std::fmax(-1.f,(float)u/32767.0f):(float)u; } break;
                case 5121:{ uint8_t u=*q; v=norm?(float)u/255.0f:(float)u; } break;
                case 5120:{ int8_t u=*(const int8_t*)q; v=norm?std::fmax(-1.f,(float)u/127.0f):(float)u; } break; }
            dst[e*comps+c]=v; } }
        return true;
    };
    auto readIdx=[&](int ai, std::vector<uint32_t>& dst)->bool{
        if(ai<0||ai>=(int)AC.size()) return false; const auto& a=AC[ai];
        int ct=(int)a["componentType"].asInt(); size_t count=(size_t)a["count"].asInt();
        if(!a.has("bufferView")) return false; const auto& bv=BV[(size_t)a["bufferView"].asInt()]; int buf=(int)bv["buffer"].asInt();
        size_t bOff=(bv.has("byteOffset")?(size_t)bv["byteOffset"].asInt():0)+(a.has("byteOffset")?(size_t)a["byteOffset"].asInt():0);
        int cs=compSize(ct); if(cs==0||buf<0||buf>=(int)buffers.size())return false; const auto& B=buffers[buf];
        if(bOff+count*cs>B.size()) return false; dst.resize(count);
        for(size_t e=0;e<count;++e){ const uint8_t* q=B.data()+bOff+e*cs; uint32_t v=0;
            if(ct==5125){memcpy(&v,q,4);} else if(ct==5123){uint16_t u;memcpy(&u,q,2);v=u;} else if(ct==5121){v=*q;} dst[e]=v; }
        return true;
    };
    auto loadImage=[&](int imgIdx, std::vector<uint8_t>& rgba, int& w, int& h)->bool{
        if(!root.has("images")) return false; const auto& IM=root["images"]; if(imgIdx<0||imgIdx>=(int)IM.size())return false;
        const auto& im=IM[imgIdx]; std::vector<uint8_t> bytes;
        if(im.has("uri")){ std::string uri=im["uri"].asString();
            if(uri.rfind("data:",0)==0){ size_t c=uri.find("base64,"); if(c!=std::string::npos)bytes=b64decode(uri.substr(c+7)); }
            else bytes=readExternal(uri, dir); }   // percent-decode + fallbacks (see readExternal)
        else if(im.has("bufferView")){ const auto& bv=BV[(size_t)im["bufferView"].asInt()]; int buf=(int)bv["buffer"].asInt();
            size_t off=bv.has("byteOffset")?(size_t)bv["byteOffset"].asInt():0; size_t len=(size_t)bv["byteLength"].asInt();
            if(buf>=0&&buf<(int)buffers.size()&&off+len<=buffers[buf].size()) bytes.assign(buffers[buf].begin()+off, buffers[buf].begin()+off+len); }
        if(bytes.empty()) return false;
        int n; unsigned char* px=stbi_load_from_memory(bytes.data(),(int)bytes.size(),&w,&h,&n,4); if(!px)return false;
        rgba.assign(px,px+(size_t)w*h*4); stbi_image_free(px); return true;
    };
    auto matBaseImage=[&](int mi)->int{
        if(mi<0||!root.has("materials"))return -1; const auto& M=root["materials"]; if(mi>=(int)M.size())return -1;
        const auto& m=M[mi]; if(!m.has("pbrMetallicRoughness"))return -1; const auto& pbr=m["pbrMetallicRoughness"];
        if(!pbr.has("baseColorTexture"))return -1; int ti=(int)pbr["baseColorTexture"]["index"].asInt();
        if(!root.has("textures"))return -1; const auto& T=root["textures"]; if(ti<0||ti>=(int)T.size())return -1;
        if(!T[ti].has("source"))return -1; return (int)T[ti]["source"].asInt();
    };
    // FALLBACK BLANK MATERIAL: a primitive with NO baseColorTexture (or a texture that fails to decode)
    // must still ship a valid texture — else the renderer/cook hits the textureless path, which on some
    // GPUs/drivers HANGS the upload (the "application unresponsive" on drag-in). Take the material's solid
    // baseColorFactor RGBA (default opaque white). Mirrors gltf_loader.h's matSolidColor for the V79 path.
    auto matSolidColorFactor=[&](int mi, uint8_t out[4]){
        out[0]=out[1]=out[2]=out[3]=255;
        if(mi<0||!root.has("materials"))return; const auto& M=root["materials"]; if(mi>=(int)M.size())return;
        const auto& m=M[mi]; if(!m.has("pbrMetallicRoughness"))return; const auto& pbr=m["pbrMetallicRoughness"];
        if(!pbr.has("baseColorFactor"))return; const auto& f=pbr["baseColorFactor"];
        for(int k=0;k<4&&k<(int)f.size();++k){ float v=(float)f[k].asFloat(); out[k]=(uint8_t)std::lround(std::fmin(1.f,std::fmax(0.f,v))*255.f); }
    };

    // per-mesh world matrix from the first node referencing it (exporter writes one node per mesh)
    auto trs=[&](const tinyjson::Value& n)->std::array<float,16>{
        std::array<float,16> M={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if(n.has("matrix")){ const auto& a=n["matrix"]; for(int i=0;i<16&&i<(int)a.size();++i)M[i]=(float)a[i].asFloat(); return M; }
        float t[3]={0,0,0},r[4]={0,0,0,1},s[3]={1,1,1};
        if(n.has("translation"))for(int i=0;i<3;i++)t[i]=(float)n["translation"][i].asFloat();
        if(n.has("rotation"))for(int i=0;i<4;i++)r[i]=(float)n["rotation"][i].asFloat();
        if(n.has("scale"))for(int i=0;i<3;i++)s[i]=(float)n["scale"][i].asFloat();
        float x=r[0],y=r[1],z=r[2],w=r[3];
        float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
        float R[9]={1-2*(yy+zz),2*(xy-wz),2*(xz+wy), 2*(xy+wz),1-2*(xx+zz),2*(yz-wx), 2*(xz-wy),2*(yz+wx),1-2*(xx+yy)};
        M[0]=R[0]*s[0]; M[1]=R[3]*s[0]; M[2]=R[6]*s[0]; M[3]=0;
        M[4]=R[1]*s[1]; M[5]=R[4]*s[1]; M[6]=R[7]*s[1]; M[7]=0;
        M[8]=R[2]*s[2]; M[9]=R[5]*s[2]; M[10]=R[8]*s[2]; M[11]=0;
        M[12]=t[0]; M[13]=t[1]; M[14]=t[2]; M[15]=1; return M;
    };
    const auto& MS = root["meshes"];
    std::vector<std::array<float,16>> meshXform(MS.size(), {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1});
    std::vector<bool> hasX(MS.size(), false);
    if(root.has("nodes")){ const auto& N=root["nodes"]; for(size_t i=0;i<N.size();++i){ if(N[i].has("mesh")){ int mi=(int)N[i]["mesh"].asInt();
        if(mi>=0&&mi<(int)MS.size()&&!hasX[mi]){ meshXform[mi]=trs(N[i]); hasX[mi]=true; } } } }

    // sidecar: restore original object names
    std::vector<std::string> sideNames;
    { std::string stem=fs::path(gltfPath).stem().string();
      std::vector<uint8_t> sb=readFile((fs::path(dir)/(stem+".blendmeta.json")).string());
      if(!sb.empty()){ try{ std::string s((char*)sb.data(),sb.size()); tinyjson::Value sv=tinyjson::parse(s);
          if(sv.has("objects")){ const auto& O=sv["objects"]; sideNames.resize(O.size()); for(size_t i=0;i<O.size();++i) if(O[i].has("name")) sideNames[i]=O[i]["name"].asString(); } }catch(...){} } }

    auto applyX=[&](const std::array<float,16>& M, float& X, float& Y, float& Z){ float x=X,y=Y,z=Z;
        X=M[0]*x+M[4]*y+M[8]*z+M[12]; Y=M[1]*x+M[5]*y+M[9]*z+M[13]; Z=M[2]*x+M[6]*y+M[10]*z+M[14]; };

    int objCounter=0;
    for(size_t mi=0; mi<MS.size(); ++mi){
        const auto& mesh=MS[mi]; if(!mesh.has("primitives")) continue;
        const auto& X = meshXform[mi]; const auto& prims=mesh["primitives"];
        for(size_t pi=0; pi<prims.size(); ++pi){
            const auto& pr=prims[pi]; if(!pr.has("attributes")) continue; const auto& at=pr["attributes"];
            if(!at.has("POSITION")) continue;
            std::vector<float> pos; int pc=0; if(!readF((int)at["POSITION"].asInt(), pos, pc) || pos.size()<9){ continue; }
            MeshData md;
            md.name = (objCounter<(int)sideNames.size() && !sideNames[objCounter].empty()) ? sideNames[objCounter]
                      : (mesh.has("name")?mesh["name"].asString():("imported_"+std::to_string(mi)));
            size_t nv=pos.size()/3; md.positions.resize(nv*3);
            for(size_t v=0;v<nv;++v){ float x=pos[v*3],y=pos[v*3+1],z=pos[v*3+2]; applyX(X,x,y,z); md.positions[v*3]=x; md.positions[v*3+1]=y; md.positions[v*3+2]=z; }
            // ATTRIBUTE-COUNT SAFETY: a primitive's TEXCOORD_0/COLOR_0 accessor can legally (or through a buggy
            // exporter) have a DIFFERENT count than POSITION. The old code indexed these buffers by POSITION's
            // vertex count nv -> out-of-bounds read = CRASH on re-import of some .gltf/.glb. Size every per-vertex
            // array to EXACTLY nv, sourcing only in-range elements and padding the rest.
            if(at.has("TEXCOORD_0")){ std::vector<float> uv; int c=0; if(readF((int)at["TEXCOORD_0"].asInt(), uv, c) && c>=2){
                md.uvs.assign(nv*2, 0.f);
                for(size_t v=0;v<nv;++v){ size_t s=(size_t)v*c; if(s+1<uv.size()){ md.uvs[v*2]=uv[s]; md.uvs[v*2+1]=uv[s+1]; } } } }
            if(at.has("COLOR_0")){ std::vector<float> col; int c=0; if(readF((int)at["COLOR_0"].asInt(), col, c) && c>0){
                md.colors.assign(nv*4, 255);
                for(size_t v=0;v<nv;++v) for(int k=0;k<4;k++){ size_t idx=(size_t)v*c+(size_t)k;
                    float cv=(k<c && idx<col.size())?col[idx]:(k<3?0.f:1.f); md.colors[v*4+k]=(uint8_t)std::lround(std::fmin(1.f,std::fmax(0.f,cv))*255.f); } } }
            if(pr.has("indices")){ readIdx((int)pr["indices"].asInt(), md.indices); }
            if(md.indices.empty()){ md.indices.resize(nv); for(size_t v=0;v<nv;++v)md.indices[v]=(uint32_t)v; }
            else {
                // DROP any triangle referencing an out-of-range vertex (indices >= vertexCount -> OOB vertex fetch
                // downstream in the renderer/cook = crash/garbage). Keep only whole in-range triangles.
                std::vector<uint32_t> good; good.reserve(md.indices.size());
                for(size_t t=0;t+2<md.indices.size();t+=3){ uint32_t a=md.indices[t],b2=md.indices[t+1],cc=md.indices[t+2];
                    if(a<nv&&b2<nv&&cc<nv){ good.push_back(a); good.push_back(b2); good.push_back(cc); } }
                md.indices.swap(good);
            }
            if(md.indices.size()<3) continue;   // no usable geometry -> skip (don't ship an empty mesh)
            md.nVerts=(u32)nv; md.nIdx=(u32)md.indices.size();
            if(pr.has("material")){ int img=matBaseImage((int)pr["material"].asInt());
                if(img>=0){ std::vector<uint8_t> rgba; int w=0,h=0; if(loadImage(img,rgba,w,h)){ md.texRGBA=std::move(rgba); md.texW=(u32)w; md.texH=(u32)h; md.hasTexture=true; } } }
            // No (or failed) texture -> synthesize a 4x4 SOLID baseColorFactor texel so every mesh ALWAYS
            // has a valid texture. Prevents the textureless-upload hang and keeps solid-color materials the
            // right color (white if the material has no factor).
            if(!md.hasTexture){
                uint8_t c[4]={255,255,255,255};
                if(pr.has("material")) matSolidColorFactor((int)pr["material"].asInt(), c);
                md.texW=md.texH=4; md.texRGBA.assign((size_t)md.texW*md.texH*4, 0);
                for(size_t k=0;k<md.texRGBA.size();k+=4){ md.texRGBA[k]=c[0]; md.texRGBA[k+1]=c[1]; md.texRGBA[k+2]=c[2]; md.texRGBA[k+3]=c[3]; }
                md.hasTexture=true;
            }
            out.push_back(std::move(md)); objCounter++;
        }
    }
    return !out.empty();
}

} // namespace gltfimport
