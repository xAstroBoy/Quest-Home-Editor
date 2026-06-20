// ── shadergen.h — getTime()-driven animation SHADER GENERATOR, in C++ (ports cooker/make_*_shader.py) ────────
// The V79→V203 cook turns decoded node animation into a SPIR-V shader that animates from globalUniforms.time, by
// surgically editing a stock V203 RENDSHAD (.surface.bin)'s forward VERTEX stage: it injects, right after the
// inPos/inUv OpLoad, a small getTime() body and reroutes downstream uses to the animated value, then appends the
// grown SPIR-V module at EOF and repoints the stage's FlatBuffer uoffset. THREE motions (one converter, generated
// on demand for any parameters — no pre-baked per-env shaders):
//   ROTATE   : v' = Rodrigues(v, axis, angle=omega*time)                 (V79 node Y/tilted-axis spin)
//   OSCILLATE: angle = (1-cos(time*2pi/period)) * (amp/2), then Rodrigues (V79 node sway)
//   UVSCROLL : uv' = inUv + vec2(rateU,rateV)*time                       (mat.sanim water/foam/lava scroll)
// 1:1 with the Python it replaces (verified field-for-field); the cooker now calls this instead of system(python).
#pragma once
#include <vector>
#include <string>
#include <map>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace shadergen {

enum Mode { ROTATE = 0, OSCILLATE = 1, UVSCROLL = 2 };

struct Inst { int op; std::vector<uint32_t> w; };   // w[0] = header word (recomputed on emit), w[1..] = operands
struct VStage { int64_t slot, spvOff; uint32_t spvLen; };   // one vertex stage: FlatBuffer uoffset slot, SPIR-V magic, len

// ── little-endian + FlatBuffer (signed soffset) readers over the source bytes ──
namespace detail {
inline uint16_t u16(const uint8_t* d, size_t N, int64_t o){ return (o>=0 && o+2<=(int64_t)N) ? (uint16_t)(d[o]|(d[o+1]<<8)) : 0; }
inline uint32_t u32(const uint8_t* d, size_t N, int64_t o){ return (o>=0 && o+4<=(int64_t)N) ? (uint32_t)(d[o]|(d[o+1]<<8)|(d[o+2]<<16)|((uint32_t)d[o+3]<<24)) : 0; }
inline int32_t  i32(const uint8_t* d, size_t N, int64_t o){ return (int32_t)u32(d,N,o); }
inline int64_t vt_field(const uint8_t* d, size_t N, int64_t tbl, int fi){
    int64_t vt = tbl - i32(d,N,tbl); if (vt<0 || vt+4>(int64_t)N) return 0;
    uint16_t vs = u16(d,N,vt); int64_t sl = vt+4+fi*2; if (sl+2 > vt+vs) return 0;
    uint16_t fo = u16(d,N,sl); return fo ? tbl+fo : 0;
}
inline int vt_nf(const uint8_t* d, size_t N, int64_t tbl){
    int64_t vt = tbl - i32(d,N,tbl); if (vt<0 || vt+4>(int64_t)N) return 0;
    uint16_t vs = u16(d,N,vt); return vs>=4 ? (vs-4)/2 : 0;
}
inline std::string str_at(const uint8_t* d, size_t N, int64_t p){
    if (!p || p+4>(int64_t)N) return "";
    int64_t s = p + u32(d,N,p); uint32_t ln = u32(d,N,s);
    return (ln>0 && ln<=256 && s+4+(int64_t)ln<=(int64_t)N) ? std::string((const char*)d+s+4, ln) : "";
}
inline std::string wstr(const std::vector<uint32_t>& w, size_t start){   // operand words -> nul-terminated string
    std::string s; for (size_t i=start;i<w.size();++i){ uint32_t v=w[i]; for (int b=0;b<4;++b){ char c=(char)((v>>(b*8))&0xff); if (!c) return s; s.push_back(c); } } return s;
}
// Locate the forward pass's VERTEX stage SPIR-V: slot = byte offset of the uoffset to repoint, spvOff = blob start
// (the 0x07230203 magic), spvLen = blob byte length. Mirrors stage discovery in make_rotate_shader.py.
inline bool findFwdVertSpv(const uint8_t* d, size_t N, int64_t& slot, int64_t& spvOff, uint32_t& spvLen){
    int64_t root = u32(d,N,0); int64_t stagesBase=0; int nPasses=0,nStages=0,fwdIdx=-1;
    for (int fi=0, nf=vt_nf(d,N,root); fi<nf; ++fi){
        int64_t fp=vt_field(d,N,root,fi); if (!fp || !u32(d,N,fp)) continue;
        int64_t vec=fp+u32(d,N,fp); if (vec+4>(int64_t)N) continue;
        uint32_t cnt=u32(d,N,vec); if (!(cnt>0 && cnt<=64)) continue;
        int64_t base=vec+4; if (base+(int64_t)cnt*4>(int64_t)N) continue;
        int64_t e0=base+u32(d,N,base);
        for (int ef=0, m=std::min(vt_nf(d,N,e0),4); ef<m; ++ef){
            if (str_at(d,N,vt_field(d,N,e0,ef)).rfind("forward",0)==0){
                nPasses=cnt;
                for (uint32_t pi=0; pi<cnt; ++pi){ int64_t pt=base+pi*4+u32(d,N,base+pi*4);
                    for (int pf=0, mm=std::min(vt_nf(d,N,pt),4); pf<mm; ++pf)
                        if (str_at(d,N,vt_field(d,N,pt,pf))=="forward") fwdIdx=(int)pi; }
            }
        }
        for (int ef=0, m=std::min(vt_nf(d,N,e0),4); ef<m; ++ef){
            int64_t sp=vt_field(d,N,e0,ef); if (!sp) continue;
            int64_t sv=sp+u32(d,N,sp); if (sv+4>(int64_t)N) continue;
            uint32_t L=u32(d,N,sv); if (L>500 && L<2000000 && sv+4+(int64_t)L<=(int64_t)N){ stagesBase=base; nStages=cnt; }
        }
    }
    if (fwdIdx<0 || nStages!=2*nPasses) return false;
    auto stageSpv=[&](int si, int64_t& sl, int64_t& v, uint32_t& b)->bool{
        int64_t se=stagesBase+si*4; int64_t st=se+u32(d,N,se);
        for (int ef=0, m=std::min(vt_nf(d,N,st),6); ef<m; ++ef){
            int64_t sp=vt_field(d,N,st,ef); if (!sp) continue;
            int64_t vv=sp+u32(d,N,sp); if (vv+4>(int64_t)N) continue;
            uint32_t L=u32(d,N,vv);
            if (L>500 && vv+4+(int64_t)L<=(int64_t)N && L%4==0 && u32(d,N,vv+4)==0x07230203){ sl=sp; v=vv; b=L; return true; }
        }
        return false;
    };
    for (int si : { 2*fwdIdx, 2*fwdIdx+1 }){
        int64_t sl,v; uint32_t b; if (!stageSpv(si,sl,v,b)) continue;
        const uint8_t* sd=d+v+4;                                        // v = [ubyte] vector loc; +4 skips the len prefix -> SPIR-V magic
        int em=-1; size_t nw=b/4, i=5;                                  // EntryPoint exec model 0 = Vertex
        while (i<nw){ uint32_t ins=u32(sd,b,(int64_t)i*4); uint32_t op=ins&0xffff, wc=ins>>16; if (!wc) break; if (op==15){ em=(int)u32(sd,b,(int64_t)(i+1)*4); break; } i+=wc; }
        if (em==0){ slot=sl; spvOff=v+4; spvLen=b; return true; }       // spvOff -> the SPIR-V bytes (the magic word)
    }
    return false;
}
} // namespace detail

// Collect ALL vertex stages across ALL passes (forward + depth/shadow/motion). Mirrors findFwdVertSpv's pass/stage
// discovery but returns EVERY Vertex-execution-model stage as {slot,spvOff,spvLen}, so the cook can animate them all.
inline void collectVertStages(const uint8_t* d, size_t N, std::vector<VStage>& out){
    using namespace detail;
    int64_t root=u32(d,N,0); int64_t stagesBase=0; int nPasses=0,nStages=0;
    for (int fi=0,nf=vt_nf(d,N,root); fi<nf; ++fi){
        int64_t fp=vt_field(d,N,root,fi); if(!fp||!u32(d,N,fp)) continue;
        int64_t vec=fp+u32(d,N,fp); if(vec+4>(int64_t)N) continue;
        uint32_t cnt=u32(d,N,vec); if(!(cnt>0&&cnt<=64)) continue;
        int64_t base=vec+4; if(base+(int64_t)cnt*4>(int64_t)N) continue;
        int64_t e0=base+u32(d,N,base);
        for(int ef=0,m=std::min(vt_nf(d,N,e0),4);ef<m;++ef)
            if(str_at(d,N,vt_field(d,N,e0,ef)).rfind("forward",0)==0) nPasses=(int)cnt;
        for(int ef=0,m=std::min(vt_nf(d,N,e0),4);ef<m;++ef){
            int64_t sp=vt_field(d,N,e0,ef); if(!sp) continue;
            int64_t sv=sp+u32(d,N,sp); if(sv+4>(int64_t)N) continue;
            uint32_t L=u32(d,N,sv); if(L>500&&L<2000000&&sv+4+(int64_t)L<=(int64_t)N){ stagesBase=base; nStages=(int)cnt; }
        }
    }
    if(!stagesBase||nPasses==0||nStages!=2*nPasses) return;
    for(int si=0; si<nStages; ++si){
        int64_t se=stagesBase+(int64_t)si*4; int64_t st=se+u32(d,N,se);
        for(int ef=0,mm=std::min(vt_nf(d,N,st),6);ef<mm;++ef){
            int64_t sp=vt_field(d,N,st,ef); if(!sp) continue;
            int64_t vv=sp+u32(d,N,sp); if(vv+4>(int64_t)N) continue;
            uint32_t L=u32(d,N,vv);
            if(L>500&&vv+4+(int64_t)L<=(int64_t)N&&L%4==0&&u32(d,N,vv+4)==0x07230203){
                const uint8_t* sd=d+vv+4; int em=-1; size_t nw2=L/4,i=5;
                while(i<nw2){ uint32_t ins=u32(sd,L,(int64_t)i*4); uint32_t op=ins&0xffff,wc=ins>>16; if(!wc)break; if(op==15){em=(int)u32(sd,L,(int64_t)(i+1)*4);break;} i+=wc; }
                if(em==0) out.push_back({sp, vv+4, L});
                break;
            }
        }
    }
}

// Inject the getTime() animation into ONE vertex SPIR-V module (sd -> the 0x07230203 magic word, spvLen bytes).
// Returns the grown module bytes, or {} if this stage can't be animated for this mode (lacks inPos/inUv — safe to skip).
inline std::vector<uint8_t> editVertModule(const uint8_t* sd, uint32_t spvLen, Mode mode, float p0, float p1, float ax, float ay, float az){
    using namespace detail;
    size_t nw = spvLen/4;
    auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5; i<nw; ){ uint32_t ins=W(i), wc=ins>>16, op=ins&0xffff; if (!wc) break; Inst t; t.op=(int)op; for (uint32_t k=0;k<wc;++k) t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }

    // discover the ids we need
    uint32_t tFloat=0,tInt=0,tV2=0,tV3=0,glsl=0,gu=0,inPos=0,inUv=0; int timeIdx=-1;
    std::map<int64_t,uint32_t> fltc; std::map<int32_t,uint32_t> intc; std::map<uint64_t,uint32_t> ptr;
    std::map<uint32_t,std::string> names;
    auto fround=[](float v){ return (int64_t)llround((double)v*1e6); };
    for (auto& t : insts){ auto& w=t.w;
        if (t.op==5) names[w[1]]=wstr(w,2);
        else if (t.op==6){ if (wstr(w,3)=="time") timeIdx=(int)w[2]; }
        else if (t.op==11 && wstr(w,2).find("GLSL")!=std::string::npos) glsl=w[1];
        else if (t.op==22 && w[2]==32) tFloat=w[1];
        else if (t.op==21 && w[2]==32) tInt=w[1];
    }
    for (auto& t : insts){ auto& w=t.w;
        if (t.op==23 && w[2]==tFloat && w[3]==3) tV3=w[1];
        else if (t.op==23 && w[2]==tFloat && w[3]==2) tV2=w[1];
        else if (t.op==32) ptr[((uint64_t)w[2]<<32)|w[3]]=w[1];
    }
    for (auto& t : insts){ auto& w=t.w;
        if (t.op==43 && w[1]==tInt){ int32_t iv; memcpy(&iv,&w[3],4); intc[iv]=w[2]; }
        else if (t.op==43 && w[1]==tFloat){ float fv; memcpy(&fv,&w[3],4); fltc[fround(fv)]=w[2]; }
    }
    for (auto& kv : names){ if (kv.second=="globalUniforms") gu=kv.first; else if (kv.second=="inPos") inPos=kv.first; else if (kv.second=="inUv") inUv=kv.first; }

    uint32_t input = (mode==UVSCROLL) ? inUv : inPos;
    uint32_t vecT  = (mode==UVSCROLL) ? tV2  : tV3;
    if (!tFloat||!tInt||!vecT||!glsl||!gu||!input||timeIdx<0) return {};

    // id + constant/type pools
    std::vector<Inst> newConsts, newTypes;
    auto nid=[&](){ return bound++; };
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    auto fconst=[&](float val)->uint32_t{ int64_t k=fround(val); auto it=fltc.find(k); if (it!=fltc.end()) return it->second; uint32_t id=nid(); newConsts.push_back({43,{0,tFloat,id,fbits(val)}}); fltc[k]=id; return id; };
    auto iconst=[&](int32_t val)->uint32_t{ auto it=intc.find(val); if (it!=intc.end()) return it->second; uint32_t id=nid(); newConsts.push_back({43,{0,tInt,id,(uint32_t)val}}); intc[val]=id; return id; };
    auto ptrOf=[&](uint32_t sc, uint32_t ty)->uint32_t{ uint64_t k=((uint64_t)sc<<32)|ty; auto it=ptr.find(k); if (it!=ptr.end()) return it->second; uint32_t id=nid(); newTypes.push_back({32,{0,id,sc,ty}}); ptr[k]=id; return id; };

    // find the inPos/inUv OpLoad (the value we animate + reroute)
    int loadIdx=-1; uint32_t loadRes=0;
    for (size_t k=0;k<insts.size();++k){ auto& w=insts[k].w; if (insts[k].op==61 && w.size()>=4 && w[3]==input){ loadIdx=(int)k; loadRes=w[2]; break; } }
    if (loadIdx<0) return {};

    // Id allocation order below matches make_*_shader.py EXACTLY (per mode) so the output is byte-identical to the
    // proven Python — SPIR-V ids are arbitrary, but matching the reference is the strongest device-correctness proof.
    const uint32_t GLSL_SIN=13, GLSL_COS=14, GLSL_CROSS=68;
    std::vector<Inst> body; uint32_t result=0;

    if (mode==UVSCROLL){
        uint32_t c_ru=fconst(p0), c_rv=fconst(p1), c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
        uint32_t ratevec=nid(); newConsts.push_back({0x2c,{0,tV2,ratevec,c_ru,c_rv}});
        uint32_t pt=nid(), t=nid(), off=nid(), uvout=nid(); result=uvout;
        body = {
            {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,t,pt}},
            {142,{0,tV2,off,ratevec,t}}, {129,{0,tV2,uvout,loadRes,off}},   // uv' = inUv + vec2(ru,rv)*time
        };
    } else if (mode==ROTATE){
        uint32_t c_om=fconst(p0), c_one=fconst(1.0f), cax=fconst(ax), cay=fconst(ay), caz=fconst(az);
        uint32_t c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
        uint32_t axisVec=nid(); newConsts.push_back({0x2c,{0,tV3,axisVec,cax,cay,caz}});
        uint32_t pt=nid(),t=nid(),a=nid(),cs=nid(),sn=nid(),omc=nid();
        uint32_t dotav=nid(),crossv=nid(),term1=nid(),term2=nid(),kk=nid(),term3=nid(),tmp=nid(),rot=nid(); result=rot;
        body = {
            {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,t,pt}}, {133,{0,tFloat,a,t,c_om}},      // a = time*omega
            {12,{0,tFloat,cs,glsl,GLSL_COS,a}}, {12,{0,tFloat,sn,glsl,GLSL_SIN,a}}, {131,{0,tFloat,omc,c_one,cs}},
            {148,{0,tFloat,dotav,axisVec,loadRes}}, {12,{0,tV3,crossv,glsl,GLSL_CROSS,axisVec,loadRes}},
            {142,{0,tV3,term1,loadRes,cs}}, {142,{0,tV3,term2,crossv,sn}}, {133,{0,tFloat,kk,dotav,omc}},
            {142,{0,tV3,term3,axisVec,kk}}, {129,{0,tV3,tmp,term1,term2}}, {129,{0,tV3,rot,tmp,term3}},
        };
    } else { // OSCILLATE
        float W2 = (p1!=0.f) ? (float)(2.0*M_PI/p1) : 0.f;
        uint32_t c_w=fconst(W2), c_half=fconst(p0*0.5f), c_one=fconst(1.0f), cax=fconst(ax), cay=fconst(ay), caz=fconst(az);
        uint32_t c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
        uint32_t axisVec=nid(); newConsts.push_back({0x2c,{0,tV3,axisVec,cax,cay,caz}});
        uint32_t pt=nid(),t=nid(),arg=nid(),carg=nid(),omcarg=nid(),a=nid(),cs=nid(),sn=nid(),omc=nid();
        uint32_t dotav=nid(),crossv=nid(),term1=nid(),term2=nid(),kk=nid(),term3=nid(),tmp=nid(),rot=nid(); result=rot;
        body = {
            {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,t,pt}}, {133,{0,tFloat,arg,t,c_w}},     // arg = time*(2pi/period)
            {12,{0,tFloat,carg,glsl,GLSL_COS,arg}}, {131,{0,tFloat,omcarg,c_one,carg}}, {133,{0,tFloat,a,omcarg,c_half}}, // a=(1-cos)*amp/2
            {12,{0,tFloat,cs,glsl,GLSL_COS,a}}, {12,{0,tFloat,sn,glsl,GLSL_SIN,a}}, {131,{0,tFloat,omc,c_one,cs}},
            {148,{0,tFloat,dotav,axisVec,loadRes}}, {12,{0,tV3,crossv,glsl,GLSL_CROSS,axisVec,loadRes}},
            {142,{0,tV3,term1,loadRes,cs}}, {142,{0,tV3,term2,crossv,sn}}, {133,{0,tFloat,kk,dotav,omc}},
            {142,{0,tV3,term3,axisVec,kk}}, {129,{0,tV3,tmp,term1,term2}}, {129,{0,tV3,rot,tmp,term3}},
        };
    }

    // assemble: new types+consts before the first OpFunction (54); the body right after the input OpLoad
    std::vector<Inst> out; bool inserted=false, injected=false;
    for (size_t k=0;k<insts.size();++k){
        if (insts[k].op==54 && !inserted){ for (auto& t:newTypes) out.push_back(t); for (auto& c:newConsts) out.push_back(c); inserted=true; }
        out.push_back(insts[k]);
        if ((int)k==loadIdx && !injected){ for (auto& b:body) out.push_back(b); injected=true; }
    }
    if (!inserted || !injected) return {};
    // reroute downstream uses of the raw load to the animated result (after the final OpFAdd that produced it)
    bool seen=false;
    for (auto& t : out){
        if (t.op==129 && t.w.size()>=4 && t.w[2]==result){ seen=true; continue; }
        if (seen) for (size_t j=1;j<t.w.size();++j) if (t.w[j]==loadRes) t.w[j]=result;
    }

    // emit the grown module
    std::vector<uint32_t> words = { 0x07230203u, version, generator, bound, 0u };
    for (auto& t : out){ uint32_t hdr=((uint32_t)t.w.size()<<16)|(uint32_t)t.op; words.push_back(hdr); for (size_t j=1;j<t.w.size();++j) words.push_back(t.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(), words.data(), mod.size());

    return mod;
}

// Generate an animated shader from a stock RENDSHAD `src`. Returns the new .surface.bin bytes (empty on failure).
//   ROTATE: p0=omega(rad/s), axis | OSCILLATE: p0=amp(rad), p1=period(s), axis | UVSCROLL: p0=rateU, p1=rateV
// Animates EVERY vertex stage (all passes) so geometry/UV is consistent across the V205 multi-pass RenderGraph
// (forward + depth + shadow + motion-vector). Editing only the forward stage left depth/motion using the un-animated
// vertex on device -> depth-test/cull/motion mismatch = "animated meshes/textures don't render on device". (UV-scroll's
// depth-only stages lack inUv -> editVertModule returns {} -> harmlessly skipped, so only real position stages grow.)
inline std::vector<uint8_t> generate(const std::vector<uint8_t>& src, Mode mode, float p0, float p1=0, float ax=0, float ay=1, float az=0){
    using namespace detail;
    if (mode != UVSCROLL){ float l=std::sqrt(ax*ax+ay*ay+az*az); if (l<=0.f) l=1.f; ax/=l; ay/=l; az/=l; }  // axis -> unit
    const uint8_t* d = src.data(); size_t N = src.size();
    std::vector<VStage> stages; collectVertStages(d, N, stages);
    if (stages.empty()) return {};
    std::vector<uint8_t> o = src; int edited = 0;
    for (const auto& st : stages){
        std::vector<uint8_t> mod = editVertModule(d + st.spvOff, st.spvLen, mode, p0, p1, ax, ay, az);
        if (mod.empty()) continue;     // this vertex stage lacks the needed input for this mode -> skip (harmless)
        while (o.size()%4) o.push_back(0);
        uint32_t nv=(uint32_t)o.size(), modLen=(uint32_t)mod.size();
        o.insert(o.end(),(uint8_t*)&modLen,(uint8_t*)&modLen+4);
        o.insert(o.end(),mod.begin(),mod.end());
        uint32_t rel=nv-(uint32_t)st.slot; memcpy(o.data()+st.slot,&rel,4);   // FlatBuffer uoffset is self-relative
        ++edited;
    }
    return edited ? o : std::vector<uint8_t>();
}

} // namespace shadergen
