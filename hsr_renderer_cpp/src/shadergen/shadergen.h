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
#include <set>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace shadergen {

//   FLIPBOOK : uv' = inUv*(1/cols,1/rows) + (col/cols,row/rows), frame=mod(floor(time*fps),frames) (skinned/grid sprite TV)
//   TRANSLATE: pos' = inPos + replay(N sampled translation OFFSETS, looped) — FAITHFULLY ports ANY node TRANSLATION track
//              (e.g. Star Trek sliding screens) by piecewise-linear interpolation of the sampled frames; not pattern-matched.
//   SCALE    : pos' = pivot + (inPos - pivot)*replay(N sampled per-axis SCALE FACTORS, looped) — FAITHFULLY ports ANY node
//              SCALE "breathe" track (e.g. Erebor's 12 wisps, NON-UNIFORM per-axis amplitudes) by piecewise-linear
//              interpolation of the sampled per-axis factor frames (frame0 = (1,1,1)); not a (1-cos) shape guess.
enum Mode { ROTATE = 0, OSCILLATE = 1, UVSCROLL = 2, FLIPBOOK = 3, TRANSLATE = 4, SCALE = 5, CUTOUT = 6, ROTREPLAY = 7, TINTREPLAY = 8, FOLIAGE = 9, TINTREPLAY_VERT = 10 };
// TINTREPLAY: per-frame RGBA MaterialTint replay in the forward FRAGMENT (color *= tint[frame]) — ports V79's
//   mat.sanim MaterialTint COLOR cycling (stinson fireworks flashes / city window lights) that the cook previously
//   dropped entirely (only the alpha FADE subset was ported, and only on the flipbook path). tframes = N*4 RGBA,
//   tN = N, p0 = loopSec. Frame-SNAP select (flashes are steps); chainable onto any generated surface.
// ROTREPLAY: per-frame ANGLE replay about a fixed axis+pivot (the aurora's NON-UNIFORM rotation — its w=0 flip-axis
// sweeps unevenly, so a constant-omega ROTATE is wrong). p0=loopSec, ax/ay/az=unit axis, tframes=[px,py,pz, a0..aN]
// (pivot then N+1 CONTINUOUS unwrapped angles, radians), tN=N segments. Vertex getTime shader interpolates the angle
// over the loop then Rodrigues-rotates (inPos-pivot) about axis by it. Runs every frame in the vertex stage = SMOOTH
// and NEVER animation-LOD-throttled or ACL-quantized like the skeletal clip (the aurora device-judder fix).

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
                // Prefer the "forwardSkinned" pass when present (SKINNED bases carry FOUR passes —
                // forwardSkinned, forward, +_debug twins — and skinned meshes DRAW forwardSkinned).
                // Editing plain "forward" put the cutout/tint into a pass the mesh never runs = the
                // edit silently invisible on device AND preview (the zen-tree uncut-canopy saga).
                int fwdPlain=-1, fwdSkin=-1;
                for (uint32_t pi=0; pi<cnt; ++pi){ int64_t pt=base+pi*4+u32(d,N,base+pi*4);
                    for (int pf=0, mm=std::min(vt_nf(d,N,pt),4); pf<mm; ++pf){
                        std::string pn = str_at(d,N,vt_field(d,N,pt,pf));
                        if (pn=="forward") fwdPlain=(int)pi;
                        else if (pn=="forwardSkinned") fwdSkin=(int)pi; } }
                fwdIdx = (fwdSkin>=0) ? fwdSkin : fwdPlain;
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
// Same discovery as findFwdVertSpv but returns the forward pass's FRAGMENT stage (EntryPoint exec model 4) — for the
// CUTOUT mode, which injects an alpha-test discard into the forward frag (the make_cutout_shader.py job, in-binary).
inline bool findFwdFragSpv(const uint8_t* d, size_t N, int64_t& slot, int64_t& spvOff, uint32_t& spvLen){
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
                // Prefer the "forwardSkinned" pass when present (SKINNED bases carry FOUR passes —
                // forwardSkinned, forward, +_debug twins — and skinned meshes DRAW forwardSkinned).
                // Editing plain "forward" put the cutout/tint into a pass the mesh never runs = the
                // edit silently invisible on device AND preview (the zen-tree uncut-canopy saga).
                int fwdPlain=-1, fwdSkin=-1;
                for (uint32_t pi=0; pi<cnt; ++pi){ int64_t pt=base+pi*4+u32(d,N,base+pi*4);
                    for (int pf=0, mm=std::min(vt_nf(d,N,pt),4); pf<mm; ++pf){
                        std::string pn = str_at(d,N,vt_field(d,N,pt,pf));
                        if (pn=="forward") fwdPlain=(int)pi;
                        else if (pn=="forwardSkinned") fwdSkin=(int)pi; } }
                fwdIdx = (fwdSkin>=0) ? fwdSkin : fwdPlain;
            }
        }
        for (int ef=0, m=std::min(vt_nf(d,N,e0),4); ef<m; ++ef){
            int64_t sp=vt_field(d,N,e0,ef); if (!sp) continue;
            int64_t sv=sp+u32(d,N,sp); if (sv+4>(int64_t)N) continue;
            uint32_t L=u32(d,N,sv); if (L>500 && L<2000000 && sv+4+(int64_t)L<=(int64_t)N){ stagesBase=base; nStages=cnt; }
        }
    }
    if (fwdIdx<0 || nStages!=2*nPasses) return false;
    for (int si : { 2*fwdIdx, 2*fwdIdx+1 }){
        int64_t se=stagesBase+si*4; int64_t st=se+u32(d,N,se);
        for (int ef=0, m=std::min(vt_nf(d,N,st),6); ef<m; ++ef){
            int64_t sp=vt_field(d,N,st,ef); if (!sp) continue;
            int64_t vv=sp+u32(d,N,sp); if (vv+4>(int64_t)N) continue;
            uint32_t L=u32(d,N,vv);
            if (L>500 && vv+4+(int64_t)L<=(int64_t)N && L%4==0 && u32(d,N,vv+4)==0x07230203){
                const uint8_t* sd=d+vv+4; int em=-1; size_t nw=L/4, i=5;
                while (i<nw){ uint32_t ins=u32(sd,L,(int64_t)i*4); uint32_t op=ins&0xffff, wc=ins>>16; if(!wc)break; if(op==15){em=(int)u32(sd,L,(int64_t)(i+1)*4);break;} i+=wc; }
                if (em==4){ slot=sp; spvOff=vv+4; spvLen=L; return true; }   // exec model 4 = Fragment
            }
        }
    }
    return false;
}
// REPOINT ALL references to the module at vvOld (its length-prefix position) to the appended module at nv.
// THE "renderer draws the UN-EDITED frag" fix: a RENDSHAD's multiple forward passes (forward /
// forward_dynamic / forward_receiveshadow / *_debug) SHARE one physical SPIR-V module through SEPARATE
// stage-table fields. Repointing only the ONE field findFwdFragSpv returned left every OTHER pass entry
// pointing at the ORIGINAL module — and which entry the renderer/device reads is table-order dependent,
// so cutout/tint edits landed for some shaders and silently VANISHED for others (the zen tree drew a
// no-discard frag while its skincutout .surface contained the edit = uncut solid-quad canopy).
inline void repointAll(std::vector<uint8_t>& o, size_t oldSize, int64_t vvOld, uint32_t nv){
    for (size_t sp = 0; sp + 4 <= oldSize; sp += 4){
        uint32_t v; memcpy(&v, o.data()+sp, 4);
        if ((int64_t)sp + v == vvOld){ uint32_t rel = nv - (uint32_t)sp; memcpy(o.data()+sp, &rel, 4); }
    }
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

// Collect ALL FRAGMENT stages across ALL passes (forwardSkinned / forward / LOD + _debug twins). Mirrors
// collectVertStages with execution model 4. The device picks a pass per technique/LOD — editing only
// findFwdFragSpv's single pick left the OTHER pass frags UN-EDITED, so a DISTANT mesh drawing a lower
// technique rendered the RAW base (stinson balloons at 3km: no cutout discard + alpha forced 1.0 =
// opaque rectangle cards, "wrong render"). Every frag edit must cover every stage that accepts it.
inline void collectFragStages(const uint8_t* d, size_t N, std::vector<VStage>& out){
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
                if(em==4) out.push_back({sp, vv+4, L});
                break;
            }
        }
    }
}
// Apply a fragment-module edit to EVERY Fragment stage in the surface. Modules that can't take the edit
// (depth/shadow frags with no base-color sample) return {} from `edit` and are skipped. Shared physical
// modules are edited ONCE (dedup by offset) — repointAll moves every table entry that referenced them.
template <typename F>
inline std::vector<uint8_t> editAllFragStages(const std::vector<uint8_t>& src, F&& edit){
    std::vector<VStage> stages; collectFragStages(src.data(), src.size(), stages);
    if (stages.empty()) return {};
    std::vector<uint8_t> o = src; int edited = 0;
    std::vector<int64_t> done;
    for (const auto& st : stages){
        bool dup=false; for (int64_t v : done) if (v==st.spvOff){ dup=true; break; }
        if (dup) continue; done.push_back(st.spvOff);
        std::vector<uint8_t> mod = edit(o.data()+st.spvOff, st.spvLen);   // originals stay in place (orphaned) -> offsets valid
        if (mod.empty()) continue;
        while (o.size()%4) o.push_back(0);
        uint32_t nv=(uint32_t)o.size(), modLen=(uint32_t)mod.size();
        o.insert(o.end(),(uint8_t*)&modLen,(uint8_t*)&modLen+4);
        o.insert(o.end(),mod.begin(),mod.end());
        detail::repointAll(o, (size_t)nv, st.spvOff-4, nv);
        ++edited;
    }
    return edited ? o : std::vector<uint8_t>();
}

// Inject the getTime() animation into ONE vertex SPIR-V module (sd -> the 0x07230203 magic word, spvLen bytes).
// Returns the grown module bytes, or {} if this stage can't be animated for this mode (lacks inPos/inUv — safe to skip).
inline std::vector<uint8_t> editVertModule(const uint8_t* sd, uint32_t spvLen, Mode mode, float p0, float p1, float ax, float ay, float az,
                                           const std::vector<float>& tframes = {}, int tN = 0){
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

    uint32_t input = (mode==UVSCROLL||mode==FLIPBOOK) ? inUv : inPos;
    uint32_t vecT  = (mode==UVSCROLL||mode==FLIPBOOK) ? tV2  : tV3;
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
        // uv' = inUv + fract(vec2(ru,rv)*time). The FRACT bounds the scroll offset to one tile: getTime() grows for the
        // whole home session, so an UNBOUNDED rate*time reaches hundreds of UV units -> float32 loses sub-texel precision
        // = shimmer/FLASH on the headset (matches the preview's fmod). The texture REPEATs, so fract's 1->0 wrap is a
        // uniform whole-card shift = seamless (no torn triangle, no mip change). Direction/speed come capped+signed
        // from uvScrollRate (the device scrolls the SAME way + speed as the live preview).
        const uint32_t GLSL_FRACT=10;
        uint32_t c_ru=fconst(p0), c_rv=fconst(p1), c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
        uint32_t ratevec=nid(); newConsts.push_back({0x2c,{0,tV2,ratevec,c_ru,c_rv}});
        uint32_t pt=nid(), t=nid(), off=nid(), offf=nid(), uvout=nid(); result=uvout;
        body = {
            {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,t,pt}},
            {142,{0,tV2,off,ratevec,t}},                       // off  = vec2(ru,rv)*time   (unbounded)
            {12,{0,tV2,offf,glsl,GLSL_FRACT,off}},             // offf = fract(off)          (bounded to one tile)
            {129,{0,tV2,uvout,loadRes,offf}},                  // uv'  = inUv + fract(rate*time)
        };
    } else if (mode==TRANSLATE){
        // GENERAL node-translation REPLAY: p0=loopSec, tframes = tN vec3 OFFSETS (delta from the baked base) sampled
        // uniformly over the loop. t01=fract(time/loopSec); piecewise-linearly interpolate the N offsets (segment i spans
        // [i/N,(i+1)/N], the last wraps off[N-1]->off[0] for a seamless loop); pos' = inPos + offset. Ports ANY translation.
        const uint32_t GLSL_FRACT=10, GLSL_STEP=48, GLSL_FCLAMP=43;
        int N = tN<2?2:tN; float loopSec = (p0>1e-4f)?p0:1.f;
        uint32_t c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
        uint32_t c_invloop=fconst(1.0f/loopSec), c_N=fconst((float)N), c0=fconst(0.f), c1=fconst(1.f);
        std::vector<uint32_t> off(N);
        for (int i=0;i<N;i++){ uint32_t cx=fconst(tframes[i*3]), cy=fconst(tframes[i*3+1]), cz=fconst(tframes[i*3+2]);
            uint32_t v=nid(); newConsts.push_back({0x2c,{0,tV3,v,cx,cy,cz}}); off[i]=v; }
        uint32_t pt=nid(),t=nid(),tn=nid(),t01=nid();
        body.push_back({65,{0,pUf,pt,gu,c_time}});
        body.push_back({61,{0,tFloat,t,pt}});
        body.push_back({133,{0,tFloat,tn,t,c_invloop}});                 // time / loopSec
        body.push_back({12,{0,tFloat,t01,glsl,GLSL_FRACT,tn}});          // fract -> [0,1)
        uint32_t acc=off[0]; float invN=1.0f/(float)N;
        // LOOP-vs-ONE-WAY auto-detect (the "jumpy on device everywhere" fix): the last segment either
        //   WRAPS off[N-1]->off[0]  (j=(i+1)%N)   — for a CYCLIC path (fog/smoke/ambient drift that returns
        //     near its start): interpolates smoothly across the seam, NO teleport = no jump. OR
        //   HOLDS off[N-1]          (j=clamp N-1) — for a ONE-WAY path (train/cars: end far from start): the
        //     time fract-wrap teleports back, avoiding a fast BACKWARD SWEEP across the whole path.
        // Decide from the DATA: if off[N-1] returns near off[0] (seam << path span) it's cyclic -> wrap.
        float _sx=tframes[(N-1)*3]-tframes[0], _sy=tframes[(N-1)*3+1]-tframes[1], _sz=tframes[(N-1)*3+2]-tframes[2];
        float _seam=std::sqrt(_sx*_sx+_sy*_sy+_sz*_sz), _span=0.f;
        for (int i=0;i<N;i++){ float a=std::fabs(tframes[i*3]),b=std::fabs(tframes[i*3+1]),c=std::fabs(tframes[i*3+2]); float m=std::max(a,std::max(b,c)); if(m>_span)_span=m; }
        bool _looping = _seam < 0.20f*_span + 0.5f;   // last frame returns near first => seamless cyclic loop
        for (int i=0;i<N;i++){ int j = _looping ? ((i+1)%N) : ((i+1<N)?(i+1):(N-1)); uint32_t c_lo=fconst((float)i*invN);
            uint32_t w =nid(); body.push_back({12,{0,tFloat,w,glsl,GLSL_STEP,c_lo,t01}});   // step(lo,t01): 1 if t01>=lo
            uint32_t sb=nid(); body.push_back({131,{0,tFloat,sb,t01,c_lo}});                // t01 - lo
            uint32_t ml=nid(); body.push_back({133,{0,tFloat,ml,sb,c_N}});                  // *(N)
            uint32_t fr=nid(); body.push_back({12,{0,tFloat,fr,glsl,GLSL_FCLAMP,ml,c0,c1}});// clamp(.,0,1) = local frac
            uint32_t df=nid(); body.push_back({131,{0,tV3,df,off[j],off[i]}});              // off[j]-off[i]
            uint32_t sc=nid(); body.push_back({142,{0,tV3,sc,df,fr}});                      // *fr
            uint32_t sg=nid(); body.push_back({129,{0,tV3,sg,off[i],sc}});                  // seg = off[i] + ...
            uint32_t rd=nid(); body.push_back({131,{0,tV3,rd,sg,acc}});                     // seg - acc
            uint32_t rs=nid(); body.push_back({142,{0,tV3,rs,rd,w}});                       // *w
            uint32_t na=nid(); body.push_back({129,{0,tV3,na,acc,rs}}); acc=na;             // acc = mix(acc,seg,w)
        }
        uint32_t posOut=nid(); body.push_back({129,{0,tV3,posOut,loadRes,acc}}); result=posOut;   // pos' = inPos + offset
    } else if (mode==SCALE){
        // GENERAL node-SCALE REPLAY (Erebor "breathe" wisps): tframes = tN per-axis SCALE FACTORS (= source scale(t)/scale(0),
        // so frame 0 = (1,1,1)) sampled uniformly over the loop; ax/ay/az = the world PIVOT. t01=fract(time/loopSec); piecewise-
        // linearly interpolate the N vec3 factors (segment i spans [i/N,(i+1)/N], the last wraps factor[N-1]->factor[0] for a
        // seamless loop); pos' = pivot + (inPos - pivot)*factor. Multiplies (NOT adds) around the pivot, per axis. Ports ANY
        // SCALE track with FAITHFUL per-axis amplitudes (the whole point — wispscale.surface could not).
        const uint32_t GLSL_FRACT=10, GLSL_STEP=48, GLSL_FCLAMP=43;
        int N = tN<2?2:tN; float loopSec = (p0>1e-4f)?p0:1.f;
        uint32_t c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
        uint32_t c_invloop=fconst(1.0f/loopSec), c_N=fconst((float)N), c0=fconst(0.f), c1=fconst(1.f);
        uint32_t c_px=fconst(ax), c_py=fconst(ay), c_pz=fconst(az);
        uint32_t pivot=nid(); newConsts.push_back({0x2c,{0,tV3,pivot,c_px,c_py,c_pz}});
        std::vector<uint32_t> fac(N);
        for (int i=0;i<N;i++){ uint32_t cx=fconst(tframes[i*3]), cy=fconst(tframes[i*3+1]), cz=fconst(tframes[i*3+2]);
            uint32_t v=nid(); newConsts.push_back({0x2c,{0,tV3,v,cx,cy,cz}}); fac[i]=v; }
        uint32_t pt=nid(),t=nid(),tn=nid(),t01=nid();
        body.push_back({65,{0,pUf,pt,gu,c_time}});
        body.push_back({61,{0,tFloat,t,pt}});
        body.push_back({133,{0,tFloat,tn,t,c_invloop}});                 // time / loopSec
        body.push_back({12,{0,tFloat,t01,glsl,GLSL_FRACT,tn}});          // fract -> [0,1)
        uint32_t acc=fac[0]; float invN=1.0f/(float)N;
        for (int i=0;i<N;i++){ int j=(i+1)%N; uint32_t c_lo=fconst((float)i*invN);
            uint32_t w =nid(); body.push_back({12,{0,tFloat,w,glsl,GLSL_STEP,c_lo,t01}});   // step(lo,t01): 1 if t01>=lo
            uint32_t sb=nid(); body.push_back({131,{0,tFloat,sb,t01,c_lo}});                // t01 - lo
            uint32_t ml=nid(); body.push_back({133,{0,tFloat,ml,sb,c_N}});                  // *(N)
            uint32_t fr=nid(); body.push_back({12,{0,tFloat,fr,glsl,GLSL_FCLAMP,ml,c0,c1}});// clamp(.,0,1) = local frac
            uint32_t df=nid(); body.push_back({131,{0,tV3,df,fac[j],fac[i]}});              // fac[j]-fac[i]
            uint32_t sc=nid(); body.push_back({142,{0,tV3,sc,df,fr}});                      // *fr
            uint32_t sg=nid(); body.push_back({129,{0,tV3,sg,fac[i],sc}});                  // seg = fac[i] + ...
            uint32_t rd=nid(); body.push_back({131,{0,tV3,rd,sg,acc}});                     // seg - acc
            uint32_t rs=nid(); body.push_back({142,{0,tV3,rs,rd,w}});                       // *w
            uint32_t na=nid(); body.push_back({129,{0,tV3,na,acc,rs}}); acc=na;             // acc = mix(acc,seg,w)
        }
        uint32_t rel=nid(); body.push_back({131,{0,tV3,rel,loadRes,pivot}});               // inPos - pivot
        uint32_t scl=nid(); body.push_back({133,{0,tV3,scl,rel,acc}});                      // *factor (component-wise)
        uint32_t posOut=nid(); body.push_back({129,{0,tV3,posOut,scl,pivot}}); result=posOut; // pos' = pivot + (inPos-pivot)*factor
    } else if (mode==FLIPBOOK){
        // p0=cols p1=rows ax=frames ay=fps. Two layouts (az = offset-only flag):
        //   az<=0.5 (SCALE flipbook, spritesheet TV): base UV is the FULL quad -> uv' = inUv*(1/cols,1/rows)+(col/cols,row/rows).
        //   az >0.5 (OFFSET flipbook, the lakeside waterfall/stream/fog mat.sanim): the mesh UV ALREADY maps to ONE
        //           cell -> uv' = inUv + (col/cols,row/rows). This matches libshell's BaseTextureMtx play (uv+=offset)
        //           SNAPPED to the integer cell (STEP) = the frame-snap the preview does. cell scale is skipped.
        const uint32_t GLSL_FLOOR=8;
        bool offsetOnly = (az>0.5f);
        int ncols=(int)(p0+0.5f), nrows=(int)(p1+0.5f), nframes=(int)(ax+0.5f); float fps=ay;
        if (ncols<1) ncols=1; if (nrows<1) nrows=1; if (nframes<1) nframes=ncols*nrows; if (fps<=0.f) fps=5.f;
        uint32_t c_fps=fconst(fps), c_nf=fconst((float)nframes), c_nc=fconst((float)ncols);
        uint32_t c_invc=fconst(1.0f/ncols), c_invr=fconst(1.0f/nrows);
        uint32_t c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
        uint32_t pt=nid(),t=nid(),tf=nid(),fr=nid(),fm=nid(),col=nid(),rdiv=nid(),row=nid();
        uint32_t uo=nid(),vo=nid(),cell=nid(),off=nid(),scaled=nid(),uvout=nid(); result=uvout;
        body = {
            {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,t,pt}},
            {133,{0,tFloat,tf,t,c_fps}},                 // time*FPS
            {12,{0,tFloat,fr,glsl,GLSL_FLOOR,tf}},       // floor()
            {141,{0,tFloat,fm,fr,c_nf}},                 // frame = mod(.., NFRAMES)
            {141,{0,tFloat,col,fm,c_nc}},                // col = mod(frame, NCOLS)
            {133,{0,tFloat,rdiv,fm,c_invc}},             // frame/NCOLS
            {12,{0,tFloat,row,glsl,GLSL_FLOOR,rdiv}},    // row = floor(frame/NCOLS)
            {133,{0,tFloat,uo,col,c_invc}},              // uOff = col/NCOLS
            {133,{0,tFloat,vo,row,c_invr}},              // vOff = row/NROWS
            {80,{0,tV2,off,uo,vo}},                      // vec2(uOff,vOff)
        };
        if (offsetOnly) {
            body.push_back({129,{0,tV2,uvout,loadRes,off}});            // uv' = inUv + off
        } else {
            body.push_back({80,{0,tV2,cell,c_invc,c_invr}});           // vec2(1/NCOLS,1/NROWS)
            body.push_back({133,{0,tV2,scaled,loadRes,cell}});         // inUv * cell
            body.push_back({129,{0,tV2,uvout,scaled,off}});            // + off
        }
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
    } else if (mode==ROTREPLAY){
        // EXACT per-frame QUATERNION replay about pivot(tframes[0..2]); tframes[3 .. 3+(N+1)*4] = N+1 relative quats (xyzw,
        // sign-continuous). q(t) = nlerp over t01=fract(time/loopSec) of the N+1 quats (piecewise-linear select of the
        // active segment, same accumulate as the angle path but on vec4), then normalize. Rotate rel=inPos-pivot by q:
        // v' = rel + 2*w*cross(u,rel) + 2*cross(u,cross(u,rel)), u=q.xyz w=q.w. pos'=pivot+v'. This reproduces the render's
        // EXACT non-uniform node rotation (any axis wobble) — a single-axis angle flattened it into a uniform texture-spin.
        const uint32_t GLSL_FRACT=10, GLSL_STEP=48, GLSL_FCLAMP=43, GLSL_CROSS=68, GLSL_NORMALIZE=69;
        int N = tN<2?2:tN; float loopSec = (p0>1e-4f)?p0:1.f;
        if ((int)tframes.size() < 3 + (N+1)*4) return {};
        float px=tframes[0], py=tframes[1], pz=tframes[2]; const float* Q=tframes.data()+3;
        uint32_t tV4=0; for (auto& tt:insts){ auto&w=tt.w; if (tt.op==23 && w.size()>=4 && w[2]==tFloat && w[3]==4){ tV4=w[1]; break; } }
        if (!tV4){ tV4=nid(); newTypes.push_back({23,{0,tV4,tFloat,4}}); }
        uint32_t c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
        uint32_t c_invloop=fconst(1.0f/loopSec), c_N=fconst((float)N), c0=fconst(0.f), c1=fconst(1.f), c2=fconst(2.0f);
        uint32_t cpx=fconst(px),cpy=fconst(py),cpz=fconst(pz); uint32_t pivot=nid(); newConsts.push_back({0x2c,{0,tV3,pivot,cpx,cpy,cpz}});
        std::vector<uint32_t> cq(N+1);
        for(int i=0;i<=N;i++){ uint32_t a=fconst(Q[i*4]),b=fconst(Q[i*4+1]),c=fconst(Q[i*4+2]),d=fconst(Q[i*4+3]); uint32_t v=nid(); newConsts.push_back({0x2c,{0,tV4,v,a,b,c,d}}); cq[i]=v; }
        uint32_t pt=nid(),t=nid(),tn=nid(),t01=nid();
        body.push_back({65,{0,pUf,pt,gu,c_time}});
        body.push_back({61,{0,tFloat,t,pt}});
        body.push_back({133,{0,tFloat,tn,t,c_invloop}});                 // time / loopSec
        body.push_back({12,{0,tFloat,t01,glsl,GLSL_FRACT,tn}});          // fract -> [0,1)
        uint32_t acc=cq[0]; float invN=1.0f/(float)N;
        for(int i=0;i<N;i++){ int j=i+1; uint32_t c_lo=fconst((float)i*invN);
            uint32_t w =nid(); body.push_back({12,{0,tFloat,w,glsl,GLSL_STEP,c_lo,t01}});   // step(lo,t01)
            uint32_t sb=nid(); body.push_back({131,{0,tFloat,sb,t01,c_lo}});                // t01-lo
            uint32_t ml=nid(); body.push_back({133,{0,tFloat,ml,sb,c_N}});                  // *N
            uint32_t fr=nid(); body.push_back({12,{0,tFloat,fr,glsl,GLSL_FCLAMP,ml,c0,c1}});// clamp 0..1 = local frac
            uint32_t df=nid(); body.push_back({131,{0,tV4,df,cq[j],cq[i]}});                // q[j]-q[i]
            uint32_t sc=nid(); body.push_back({142,{0,tV4,sc,df,fr}});                      // *fr
            uint32_t sg=nid(); body.push_back({129,{0,tV4,sg,cq[i],sc}});                   // seg=q[i]+..
            uint32_t rd=nid(); body.push_back({131,{0,tV4,rd,sg,acc}});                     // seg-acc
            uint32_t rs=nid(); body.push_back({142,{0,tV4,rs,rd,w}});                       // *w
            uint32_t na=nid(); body.push_back({129,{0,tV4,na,acc,rs}}); acc=na;             // acc=mix(acc,seg,w)
        }
        uint32_t qn=nid();  body.push_back({12,{0,tV4,qn,glsl,GLSL_NORMALIZE,acc}});        // normalize(nlerp) quaternion
        uint32_t u=nid();   body.push_back({79,{0,tV3,u,qn,qn,0,1,2}});                     // u = q.xyz
        uint32_t qw=nid();  body.push_back({81,{0,tFloat,qw,qn,3}});                        // w = q.w
        uint32_t rel=nid(); body.push_back({131,{0,tV3,rel,loadRes,pivot}});               // rel = inPos - pivot
        uint32_t cr1=nid(); body.push_back({12,{0,tV3,cr1,glsl,GLSL_CROSS,u,rel}});         // cross(u,rel)
        uint32_t t2=nid();  body.push_back({142,{0,tV3,t2,cr1,c2}});                        // 2*cross(u,rel)
        uint32_t wt=nid();  body.push_back({142,{0,tV3,wt,t2,qw}});                         // w*(2*cross)
        uint32_t cr2=nid(); body.push_back({12,{0,tV3,cr2,glsl,GLSL_CROSS,u,t2}});          // cross(u, 2*cross)
        uint32_t s1=nid();  body.push_back({129,{0,tV3,s1,rel,wt}});                        // rel + w*t2
        uint32_t vr=nid();  body.push_back({129,{0,tV3,vr,s1,cr2}});                        // + cross(u,t2)
        uint32_t posOut=nid(); body.push_back({129,{0,tV3,posOut,vr,pivot}}); result=posOut; // pos' = pivot + q*rel
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

// CUTOUT: inject an alpha-test DISCARD into a forward-fragment SPIR-V module (the make_cutout_shader.py job, done
// IN-BINARY so the cook generates it on demand from the stock unlit surface — no NDK spirv-dis/as, no pre-baked .bin,
// no haveCutout/CWD fragility). Faithful to Meta's unlitfoliage (decompiled via SPIRV-Cross): a mostly-opaque "_alpha"/
// "_masked" scenery card must DEPTH-WRITE (occlude = no depth issues) AND drop its transparent silhouette (= no black
// background) — exactly `if (baseColor.a < threshold) discard;` in the depth-writing forward pass. (unlitfoliage also
// `sharpenAlpha`s the edge for AA-coverage; a hard discard at 0.5 is the no-MSAA equivalent — crisp 1-texel cutout.)
inline std::vector<uint8_t> editFragCutout(const uint8_t* sd, uint32_t spvLen, float threshold){
    using namespace detail;
    size_t nw = spvLen/4;
    auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5;i<nw;){ uint32_t ins=W(i),wc=ins>>16,op=ins&0xffff; if(!wc)break; Inst t; t.op=(int)op; for(uint32_t k=0;k<wc;++k)t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    uint32_t tFloat=0, tBool=0, c_thr=0, sampleRes=0; int sampleIdx=-1;
    for (auto& t:insts){ auto&w=t.w;
        if (t.op==22 && w.size()>=3 && w[2]==32) tFloat=w[1];          // OpTypeFloat: w=[header, result, width] -> width is w[2] (32-bit), result id is w[1]
        else if (t.op==20 && w.size()>=2) tBool=w[1];                  // OpTypeBool
    }
    for (auto& t:insts){ auto&w=t.w;                                   // reuse an existing 0.5 const if present
        if (t.op==43 && tFloat && w.size()>=4 && w[1]==tFloat){ float fv; memcpy(&fv,&w[3],4); if (std::fabs(fv-threshold)<1e-6f) c_thr=w[2]; }
    }
    for (size_t k=0;k<insts.size();++k){ if (insts[k].op==87 && insts[k].w.size()>=3){ sampleRes=insts[k].w[2]; sampleIdx=(int)k; break; } } // 1st OpImageSampleImplicitLod = basecolor
    if (!tFloat || sampleIdx<0) return {};
    uint32_t nidNext=bound; auto nid=[&](){ return nidNext++; };
    std::vector<Inst> newGlobals;
    if (!tBool)  { tBool=nid();  newGlobals.push_back({20,{0,tBool}}); }                       // OpTypeBool
    if (!c_thr)  { c_thr=nid();  newGlobals.push_back({43,{0,tFloat,c_thr,fbits(threshold)}}); } // OpConstant float threshold
    uint32_t aId=nid(), chk=nid(), killL=nid(), mrgL=nid();
    std::vector<Inst> body = {
        {81,{0,tFloat,aId,sampleRes,3}},      // alpha   = baseColor.w
        {184,{0,tBool,chk,aId,c_thr}},        // chk     = alpha < threshold   (OpFOrdLessThan)
        {247,{0,mrgL,0}},                     // OpSelectionMerge merge None
        {250,{0,chk,killL,mrgL}},             // OpBranchConditional chk kill merge
        {248,{0,killL}}, {252,{0}},           // kill: OpLabel ; OpKill
        {248,{0,mrgL}},                       // merge: OpLabel  (the rest of the block continues here)
    };
    std::vector<Inst> out; bool addedG=false;
    for (size_t k=0;k<insts.size();++k){
        if (!addedG && insts[k].op==54){ for(auto&g:newGlobals) out.push_back(g); addedG=true; }   // types/consts before the 1st OpFunction
        out.push_back(insts[k]);
        if ((int)k==sampleIdx){ for(auto&b:body) out.push_back(b); }                                // discard right after the basecolor sample
    }
    std::vector<uint32_t> words={0x07230203u,version,generator,nidNext,0u};
    for(auto&t:out){ uint32_t hdr=((uint32_t)t.w.size()<<16)|(uint32_t)t.op; words.push_back(hdr); for(size_t j=1;j<t.w.size();++j)words.push_back(t.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(),words.data(),mod.size());
    return mod;
}

// FOLIAGE SHARPEN-ALPHA (Meta's unlitfoliage recipe, for the a2c/masked pipeline): right after the basecolor
// sample, rewrite alpha to `clamp((a - cutoff) / max(fwidth(a), eps) + 0.5, 0, 1)` and discard only when the
// RAW alpha is ~0 (kill fully-empty texels so depth doesn't block behind holes). With the MATL masked/a2c flags
// the MSAA hardware converts that crisp 1-texel-transition alpha into coverage = SMOOTH edges AND depth-write —
// the native V205 foliage look. (A raw-alpha a2c without the sharpen = the "goopy mess": mip-blurred soft alpha
// dithers whole leaves translucent.) All downstream uses of the sample see the sharpened vector.
inline std::vector<uint8_t> editFragFoliageSharpen(const uint8_t* sd, uint32_t spvLen, float cutoff){
    using namespace detail;
    size_t nw = spvLen/4;
    auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5;i<nw;){ uint32_t ins=W(i),wc=ins>>16,op=ins&0xffff; if(!wc)break; Inst t; t.op=(int)op; for(uint32_t k=0;k<wc;++k)t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    uint32_t tFloat=0, tBool=0, glslExt=0, sampleRes=0, tVec4=0; int sampleIdx=-1;
    for (auto& t:insts){ auto&w=t.w;
        if (t.op==22 && w.size()>=3 && w[2]==32) tFloat=w[1];               // OpTypeFloat 32
        else if (t.op==20 && w.size()>=2) tBool=w[1];                       // OpTypeBool
        else if (t.op==11 && w.size()>=3) glslExt=w[1];                     // OpExtInstImport (GLSL.std.450)
    }
    for (size_t k=0;k<insts.size();++k){ if (insts[k].op==87 && insts[k].w.size()>=3){ tVec4=insts[k].w[1]; sampleRes=insts[k].w[2]; sampleIdx=(int)k; break; } } // 1st OpImageSampleImplicitLod
    if (!tFloat || sampleIdx<0 || !glslExt) return {};
    uint32_t nidNext=bound; auto nid=[&](){ return nidNext++; };
    std::vector<Inst> newGlobals;
    if (!tBool) { tBool=nid(); newGlobals.push_back({20,{0,tBool}}); }
    uint32_t cCut=nid(); newGlobals.push_back({43,{0,tFloat,cCut,fbits(cutoff)}});
    uint32_t cEps=nid(); newGlobals.push_back({43,{0,tFloat,cEps,fbits(1e-4f)}});
    uint32_t cHalf=nid();newGlobals.push_back({43,{0,tFloat,cHalf,fbits(0.5f)}});
    uint32_t c0=nid();   newGlobals.push_back({43,{0,tFloat,c0,fbits(0.0f)}});
    uint32_t c1=nid();   newGlobals.push_back({43,{0,tFloat,c1,fbits(1.0f)}});
    uint32_t cKill=nid();newGlobals.push_back({43,{0,tFloat,cKill,fbits(0.02f)}});
    uint32_t aId=nid(), fwId=nid(), fwMax=nid(), sub=nid(), div=nid(), add=nid(), clm=nid(), newVec=nid(), chk=nid(), killL=nid(), mrgL=nid();
    std::vector<Inst> body = {
        {81,{0,tFloat,aId,sampleRes,3}},                 // a  = base.w                (OpCompositeExtract)
        {209,{0,tFloat,fwId,aId}},                       // fw = OpFwidth(a)
        {12,{0,tFloat,fwMax,glslExt,40,fwId,cEps}},      // fw'= FMax(fw, eps)         (GLSL.std.450 FMax=40)
        {131,{0,tFloat,sub,aId,cCut}},                   // s  = a - cutoff            (OpFSub)
        {136,{0,tFloat,div,sub,fwMax}},                  // s /= fw'                   (OpFDiv)
        {129,{0,tFloat,add,div,cHalf}},                  // s += 0.5                   (OpFAdd)
        {12,{0,tFloat,clm,glslExt,43,add,c0,c1}},        // s  = FClamp(s,0,1)         (FClamp=43)
        {172,{0,tVec4,newVec,clm,sampleRes,3}},          // base' = insert s at .w     (OpCompositeInsert: [type,res,Object,Composite,idx])
        {184,{0,tBool,chk,aId,cKill}},                   // chk = a < 0.02             (OpFOrdLessThan)
        {247,{0,mrgL,0}},                                // OpSelectionMerge
        {250,{0,chk,killL,mrgL}},                        // OpBranchConditional
        {248,{0,killL}}, {252,{0}},                      // kill: OpLabel ; OpKill
        {248,{0,mrgL}},                                  // merge
    };
    std::vector<Inst> out; bool addedG=false;
    for (size_t k=0;k<insts.size();++k){
        if (!addedG && insts[k].op==54){ for(auto&g:newGlobals) out.push_back(g); addedG=true; }
        Inst t = insts[k];
        if ((int)k>sampleIdx)                             // downstream uses see the SHARPENED vector
            for (size_t j=1;j<t.w.size();++j) if (t.w[j]==sampleRes && !(t.op==87)) t.w[j]=newVec;
        out.push_back(t);
        if ((int)k==sampleIdx){ for(auto&b:body) out.push_back(b); }
    }
    std::vector<uint32_t> words={0x07230203u,version,generator,nidNext,0u};
    for(auto&t:out){ uint32_t hdr=((uint32_t)t.w.size()<<16)|(uint32_t)t.op; words.push_back(hdr); for(size_t j=1;j<t.w.size();++j)words.push_back(t.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(),words.data(),mod.size());
    return mod;
}

// FLIPBOOK in the FORWARD FRAGMENT: step the base-color texture-sample UV through spritesheet cells from
// globalUniforms.time, right before the OpImageSampleImplicitLod. THE device fix: the vertex-stage UV edit
// (editVertModule FLIPBOOK) did NOT animate on device (the transparent pass's UV varying never advanced → smoke/fog/
// dust frozen at cell 0 = static/"invisible faint cell"), while the SAME getTime() math IN THE FRAGMENT animates
// (the forward frag reads globalUniforms.time + offsets the exact sample coord in the exact rendered pass — the place
// CUTOUT's editFragCutout already proves reaches device). offsetOnly: base UV maps to ONE cell (uv+=cell); else the UV
// is the full quad (uv*cell+off). cols/rows/frames/fps as FLIPBOOK.
inline std::vector<uint8_t> editFragFlipbook(const uint8_t* sd, uint32_t spvLen, int ncols, int nrows, int nframes, float fps, bool offsetOnly){
    using namespace detail;
    size_t nw=spvLen/4; auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5;i<nw;){ uint32_t ins=W(i),wc=ins>>16,op=ins&0xffff; if(!wc)break; Inst t; t.op=(int)op; for(uint32_t k=0;k<wc;++k)t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }
    uint32_t tFloat=0,tInt=0,tV2=0,glsl=0,gu=0; int timeIdx=-1;
    std::map<int64_t,uint32_t> fltc; std::map<int32_t,uint32_t> intc; std::map<uint64_t,uint32_t> ptr;
    std::map<uint32_t,std::string> names;
    auto fround=[](float v){ return (int64_t)llround((double)v*1e6); };
    for (auto& t:insts){ auto&w=t.w;
        if (t.op==5) names[w[1]]=wstr(w,2);
        else if (t.op==6){ if (wstr(w,3)=="time") timeIdx=(int)w[2]; }
        else if (t.op==11 && wstr(w,2).find("GLSL")!=std::string::npos) glsl=w[1];
        else if (t.op==22 && w[2]==32) tFloat=w[1];
        else if (t.op==21 && w[2]==32) tInt=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==23 && w[2]==tFloat && w[3]==2) tV2=w[1]; else if (t.op==32) ptr[((uint64_t)w[2]<<32)|w[3]]=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==43 && w[1]==tInt){ int32_t iv; memcpy(&iv,&w[3],4); intc[iv]=w[2]; } else if (t.op==43 && w[1]==tFloat){ float fv; memcpy(&fv,&w[3],4); fltc[fround(fv)]=w[2]; } }
    for (auto& kv:names) if (kv.second=="globalUniforms") gu=kv.first;
    if (!tFloat||!tInt||!tV2||!glsl||!gu||timeIdx<0) return {};
    int sampIdx=-1; uint32_t coord=0;                                  // first base-color texture sample (op 87)
    for (size_t k=0;k<insts.size();++k){ if (insts[k].op==87 && insts[k].w.size()>=5){ sampIdx=(int)k; coord=insts[k].w[4]; break; } }
    if (sampIdx<0) return {};
    std::vector<Inst> newConsts,newTypes;
    auto nid=[&](){ return bound++; };
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    auto fconst=[&](float val)->uint32_t{ int64_t k=fround(val); auto it=fltc.find(k); if(it!=fltc.end())return it->second; uint32_t id=nid(); newConsts.push_back({43,{0,tFloat,id,fbits(val)}}); fltc[k]=id; return id; };
    auto iconst=[&](int32_t val)->uint32_t{ auto it=intc.find(val); if(it!=intc.end())return it->second; uint32_t id=nid(); newConsts.push_back({43,{0,tInt,id,(uint32_t)val}}); intc[val]=id; return id; };
    auto ptrOf=[&](uint32_t sc,uint32_t ty)->uint32_t{ uint64_t k=((uint64_t)sc<<32)|ty; auto it=ptr.find(k); if(it!=ptr.end())return it->second; uint32_t id=nid(); newTypes.push_back({32,{0,id,sc,ty}}); ptr[k]=id; return id; };
    const uint32_t GLSL_FLOOR=8;
    if (ncols<1)ncols=1; if(nrows<1)nrows=1; if(nframes<1)nframes=ncols*nrows; if(fps<=0.f)fps=5.f;
    uint32_t c_fps=fconst(fps), c_nf=fconst((float)nframes), c_nc=fconst((float)ncols), c_invc=fconst(1.0f/(float)ncols), c_invr=fconst(1.0f/(float)nrows);
    uint32_t c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);             // storageClass 2 = Uniform
    uint32_t pt=nid(),t=nid(),tf=nid(),fr=nid(),fm=nid(),col=nid(),rdiv=nid(),row=nid(),uo=nid(),vo=nid(),off=nid(),newUv=nid();
    std::vector<Inst> body = {
        {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,t,pt}},             // time = globalUniforms.time
        {133,{0,tFloat,tf,t,c_fps}},                                  // time*fps
        {12,{0,tFloat,fr,glsl,GLSL_FLOOR,tf}},                        // floor
        {141,{0,tFloat,fm,fr,c_nf}},                                  // frame = mod(.,NFRAMES)
        {141,{0,tFloat,col,fm,c_nc}},                                 // col   = mod(frame,NCOLS)
        {133,{0,tFloat,rdiv,fm,c_invc}},                              // frame/NCOLS
        {12,{0,tFloat,row,glsl,GLSL_FLOOR,rdiv}},                     // row   = floor(frame/NCOLS)
        {133,{0,tFloat,uo,col,c_invc}},                               // uOff = col/NCOLS
        {133,{0,tFloat,vo,row,c_invr}},                               // vOff = row/NROWS
        {80,{0,tV2,off,uo,vo}},                                       // vec2(uOff,vOff)
    };
    if (offsetOnly) body.push_back({129,{0,tV2,newUv,coord,off}});    // newUv = coord + off
    else { uint32_t cell=nid(),scaled=nid(); body.push_back({80,{0,tV2,cell,c_invc,c_invr}}); body.push_back({133,{0,tV2,scaled,coord,cell}}); body.push_back({129,{0,tV2,newUv,scaled,off}}); }
    std::vector<Inst> out; bool addedG=false;
    for (size_t k=0;k<insts.size();++k){
        if (!addedG && insts[k].op==54){ for(auto&t2:newTypes)out.push_back(t2); for(auto&c:newConsts)out.push_back(c); addedG=true; }
        if ((int)k==sampIdx){ for(auto&b:body) out.push_back(b); Inst s=insts[k]; s.w[4]=newUv; out.push_back(s); continue; }   // sample at the stepped cell
        out.push_back(insts[k]);
    }
    if (!addedG) return {};
    std::vector<uint32_t> words={0x07230203u,version,generator,bound,0u};
    for(auto&t2:out){ uint32_t hdr=((uint32_t)t2.w.size()<<16)|(uint32_t)t2.op; words.push_back(hdr); for(size_t j=1;j<t2.w.size();++j)words.push_back(t2.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(),words.data(),mod.size());
    return mod;
}

// UV-SCROLL in the FORWARD FRAGMENT: offset the base-color sample UV by fract(rate*globalUniforms.time) right before
// the OpImageSampleImplicitLod. The continuous-scroll analogue of editFragFlipbook (the bluehills drifting FOG fix) —
// the vertex-stage UV scroll did NOT animate on device, the fragment one does (CUTOUT proves the forward frag reaches
// device). fract bounds the offset to one tile so the texture REPEAT wrap stays seamless (no float32 precision drift).
inline std::vector<uint8_t> editFragUvScroll(const uint8_t* sd, uint32_t spvLen, float ru, float rv){
    using namespace detail;
    size_t nw=spvLen/4; auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5;i<nw;){ uint32_t ins=W(i),wc=ins>>16,op=ins&0xffff; if(!wc)break; Inst t; t.op=(int)op; for(uint32_t k=0;k<wc;++k)t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }
    uint32_t tFloat=0,tInt=0,tV2=0,glsl=0,gu=0; int timeIdx=-1;
    std::map<int64_t,uint32_t> fltc; std::map<int32_t,uint32_t> intc; std::map<uint64_t,uint32_t> ptr;
    std::map<uint32_t,std::string> names;
    auto fround=[](float v){ return (int64_t)llround((double)v*1e6); };
    for (auto& t:insts){ auto&w=t.w;
        if (t.op==5) names[w[1]]=wstr(w,2);
        else if (t.op==6){ if (wstr(w,3)=="time") timeIdx=(int)w[2]; }
        else if (t.op==11 && wstr(w,2).find("GLSL")!=std::string::npos) glsl=w[1];
        else if (t.op==22 && w[2]==32) tFloat=w[1];
        else if (t.op==21 && w[2]==32) tInt=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==23 && w[2]==tFloat && w[3]==2) tV2=w[1]; else if (t.op==32) ptr[((uint64_t)w[2]<<32)|w[3]]=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==43 && w[1]==tInt){ int32_t iv; memcpy(&iv,&w[3],4); intc[iv]=w[2]; } else if (t.op==43 && w[1]==tFloat){ float fv; memcpy(&fv,&w[3],4); fltc[fround(fv)]=w[2]; } }
    for (auto& kv:names) if (kv.second=="globalUniforms") gu=kv.first;
    if (!tFloat||!tInt||!tV2||!glsl||!gu||timeIdx<0) return {};
    int sampIdx=-1; uint32_t coord=0;
    for (size_t k=0;k<insts.size();++k){ if (insts[k].op==87 && insts[k].w.size()>=5){ sampIdx=(int)k; coord=insts[k].w[4]; break; } }
    if (sampIdx<0) return {};
    std::vector<Inst> newConsts,newTypes;
    auto nid=[&](){ return bound++; };
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    auto fconst=[&](float val)->uint32_t{ int64_t k=fround(val); auto it=fltc.find(k); if(it!=fltc.end())return it->second; uint32_t id=nid(); newConsts.push_back({43,{0,tFloat,id,fbits(val)}}); fltc[k]=id; return id; };
    auto iconst=[&](int32_t val)->uint32_t{ auto it=intc.find(val); if(it!=intc.end())return it->second; uint32_t id=nid(); newConsts.push_back({43,{0,tInt,id,(uint32_t)val}}); intc[val]=id; return id; };
    auto ptrOf=[&](uint32_t sc,uint32_t ty)->uint32_t{ uint64_t k=((uint64_t)sc<<32)|ty; auto it=ptr.find(k); if(it!=ptr.end())return it->second; uint32_t id=nid(); newTypes.push_back({32,{0,id,sc,ty}}); ptr[k]=id; return id; };
    const uint32_t GLSL_FRACT=10;
    uint32_t c_ru=fconst(ru), c_rv=fconst(rv), c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
    uint32_t ratevec=nid(); newConsts.push_back({0x2c,{0,tV2,ratevec,c_ru,c_rv}});
    uint32_t pt=nid(),t=nid(),off=nid(),offf=nid(),newUv=nid();
    std::vector<Inst> body = {
        {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,t,pt}},   // time = globalUniforms.time
        {142,{0,tV2,off,ratevec,t}},                       // off  = vec2(ru,rv)*time
        {12,{0,tV2,offf,glsl,GLSL_FRACT,off}},             // offf = fract(off)  (one tile)
        {129,{0,tV2,newUv,coord,offf}},                    // newUv= coord + offf
    };
    std::vector<Inst> out; bool addedG=false;
    for (size_t k=0;k<insts.size();++k){
        if (!addedG && insts[k].op==54){ for(auto&t2:newTypes)out.push_back(t2); for(auto&c:newConsts)out.push_back(c); addedG=true; }
        if ((int)k==sampIdx){ for(auto&b:body) out.push_back(b); Inst s=insts[k]; s.w[4]=newUv; out.push_back(s); continue; }
        out.push_back(insts[k]);
    }
    if (!addedG) return {};
    std::vector<uint32_t> words={0x07230203u,version,generator,bound,0u};
    for(auto&t2:out){ uint32_t hdr=((uint32_t)t2.w.size()<<16)|(uint32_t)t2.op; words.push_back(hdr); for(size_t j=1;j<t2.w.size();++j)words.push_back(t2.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(),words.data(),mod.size());
    return mod;
}

// FLIPBOOK by EXACT per-frame FULL 2x3 MATRIX REPLAY in the forward FRAGMENT — the "stop guessing, use the desktop's
// own data" fix. mats = N frames × 6 = [a,b,c,d,e,f] copied VERBATIM from the mat.sanim track (the SAME values the
// desktop's animate() plays). frame = floor(fract(time/loopSec)*N) (frame-SNAP, like the desktop); the base-color
// sample UV becomes (a·u+b·v+c, d·u+e·v+f). Uses ALL frames (no cell-skipping subsample = no flashing) and the FULL
// matrix (so a scroll, a sprite-cell atlas AND a 2x2 SCALE/dust all replay exactly as the source authored them).
// cellClamp (optional, 6 floats {umin,vmin,umax,vmax,padU,padV}) = SPRITESHEET close-up seam fix: clamp the sampled
// UV to the CURRENT frame's cell (the base-UV span pushed through the same frame matrix) inset by pad — the bilinear/
// ASTC-block footprint can never pull the neighbour cell in. Replaces the old cook-side 8-texel UV RESCALE, which
// squeezed the whole frame (25% content loss on a 63px-wide knife cell = "u fucked 2 things"); a clamp pins only
// edge-touching samples and leaves the frame interior pixel-exact.
inline std::vector<uint8_t> editFragUvMatrixReplay(const uint8_t* sd, uint32_t spvLen, const std::vector<float>& mats, int N, float loopSec, const float* cellClamp=nullptr){
    using namespace detail;
    if (N < 2 || (int)mats.size() < N*6) return {};
    size_t nw=spvLen/4; auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5;i<nw;){ uint32_t ins=W(i),wc=ins>>16,op=ins&0xffff; if(!wc)break; Inst t; t.op=(int)op; for(uint32_t k=0;k<wc;++k)t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }
    uint32_t tFloat=0,tInt=0,tV2=0,tV3=0,glsl=0,gu=0; int timeIdx=-1;
    std::map<int64_t,uint32_t> fltc; std::map<int32_t,uint32_t> intc; std::map<uint64_t,uint32_t> ptr;
    std::map<uint32_t,std::string> names;
    auto fround=[](float v){ return (int64_t)llround((double)v*1e6); };
    for (auto& t:insts){ auto&w=t.w;
        if (t.op==5) names[w[1]]=wstr(w,2);
        else if (t.op==6){ if (wstr(w,3)=="time") timeIdx=(int)w[2]; }
        else if (t.op==11 && wstr(w,2).find("GLSL")!=std::string::npos) glsl=w[1];
        else if (t.op==22 && w[2]==32) tFloat=w[1];
        else if (t.op==21 && w[2]==32 && (w.size()<4 || w[3]==1)) tInt=w[1]; }   // SIGNED int32 only (OpConvertFToS/SClamp require it)
    for (auto& t:insts){ auto&w=t.w; if (t.op==23 && w[2]==tFloat && w[3]==2) tV2=w[1]; else if (t.op==23 && w[2]==tFloat && w[3]==3) tV3=w[1]; else if (t.op==32) ptr[((uint64_t)w[2]<<32)|w[3]]=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==43 && w[1]==tInt && tInt){ int32_t iv; memcpy(&iv,&w[3],4); intc[iv]=w[2]; } else if (t.op==43 && w[1]==tFloat){ float fv; memcpy(&fv,&w[3],4); fltc[fround(fv)]=w[2]; } }
    for (auto& kv:names) if (kv.second=="globalUniforms") gu=kv.first;
    if (!tFloat||!tV2||!glsl||!gu||timeIdx<0) return {};
    int sampIdx=-1; uint32_t coord=0;
    for (size_t k=0;k<insts.size();++k){ if (insts[k].op==87 && insts[k].w.size()>=5){ sampIdx=(int)k; coord=insts[k].w[4]; break; } }
    if (sampIdx<0) return {};
    // ONE ordered stream (types/consts/private vars) — each def before use (the array type needs the length
    // CONSTANT first, so the old separate newTypes-then-newConsts emission would forward-reference).
    std::vector<Inst> newGlobals;
    auto nid=[&](){ return bound++; };
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    auto fconst=[&](float val)->uint32_t{ int64_t k=fround(val); auto it=fltc.find(k); if(it!=fltc.end())return it->second; uint32_t id=nid(); newGlobals.push_back({43,{0,tFloat,id,fbits(val)}}); fltc[k]=id; return id; };
    if (!tInt){ tInt=nid(); newGlobals.push_back({21,{0,tInt,32,1}}); }   // SIGNED int32 (create when the module lacks one)
    auto iconst=[&](int32_t val)->uint32_t{ auto it=intc.find(val); if(it!=intc.end())return it->second; uint32_t id=nid(); newGlobals.push_back({43,{0,tInt,id,(uint32_t)val}}); intc[val]=id; return id; };
    auto ptrOf=[&](uint32_t sc,uint32_t ty)->uint32_t{ uint64_t k=((uint64_t)sc<<32)|ty; auto it=ptr.find(k); if(it!=ptr.end())return it->second; uint32_t id=nid(); newGlobals.push_back({32,{0,id,sc,ty}}); ptr[k]=id; return id; };
    if (!tV3){ tV3=nid(); newGlobals.push_back({23,{0,tV3,tFloat,3}}); }   // OpTypeVector float 3 (if absent)
    const uint32_t GLSL_FRACT=10, GLSL_SCLAMP=45;
    if (loopSec<=1e-4f) loopSec=1.f;
    uint32_t c_invloop=fconst(1.0f/loopSec), c_N=fconst((float)N), c_one=fconst(1.0f), c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
    std::vector<uint32_t> row0(N), row1(N);   // per frame: row0=(a,b,c), row1=(d,e,f)
    for (int i=0;i<N;i++){
        uint32_t a=fconst(mats[(size_t)i*6+0]),b=fconst(mats[(size_t)i*6+1]),c=fconst(mats[(size_t)i*6+2]);
        uint32_t d=fconst(mats[(size_t)i*6+3]),e=fconst(mats[(size_t)i*6+4]),f=fconst(mats[(size_t)i*6+5]);
        uint32_t r0=nid(); newGlobals.push_back({0x2c,{0,tV3,r0,a,b,c}}); row0[i]=r0;
        uint32_t r1=nid(); newGlobals.push_back({0x2c,{0,tV3,r1,d,e,f}}); row1[i]=r1; }
    // CONSTANT-ARRAY replay (the zen-tree "device sticks" fix, same encoding as editFragTintReplay): the old
    // emitter UNROLLED N step-select rows (~14N ALU words — dust's 151 frames ≈ 2100 fragment instructions)
    // which desktop GPUs ran but the Quest driver executed as GARBAGE (stuck/wrong-tempo cards, garbage hues).
    // Emit the SAME N keys VERBATIM as two OpConstantComposite vec3[N] Private arrays and do ONE dynamically-
    // indexed load per row — frame-snap semantics identical, every key byte-exact, ~4N data words of growth.
    uint32_t cLen=iconst(N), c_i0=iconst(0), c_iN1=iconst(N-1);
    uint32_t tArr=nid(); newGlobals.push_back({28,{0,tArr,tV3,cLen}});                    // OpTypeArray vec3[N]
    uint32_t arr0=nid();
    { Inst a; a.op=0x2c; a.w.push_back(0); a.w.push_back(tArr); a.w.push_back(arr0);      // OpConstantComposite vec3[N]{row0...}
      for (int i=0;i<N;i++) a.w.push_back(row0[i]); newGlobals.push_back(std::move(a)); }
    uint32_t arr1=nid();
    { Inst a; a.op=0x2c; a.w.push_back(0); a.w.push_back(tArr); a.w.push_back(arr1);
      for (int i=0;i<N;i++) a.w.push_back(row1[i]); newGlobals.push_back(std::move(a)); }
    uint32_t pArr=ptrOf(6,tArr);
    uint32_t var0=nid(); newGlobals.push_back({59,{0,pArr,var0,6,arr0}});                 // OpVariable Private, initializer=keys
    uint32_t var1=nid(); newGlobals.push_back({59,{0,pArr,var1,6,arr1}});
    uint32_t pV3=ptrOf(6,tV3);
    uint32_t pt=nid(),t=nid(),tn=nid(),t01=nid(),fphase=nid(),idxF=nid(),idxC=nid(),ac0=nid(),acc0=nid(),ac1=nid(),acc1=nid();
    std::vector<Inst> body = {
        {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,t,pt}},        // time
        {133,{0,tFloat,tn,t,c_invloop}},                        // time/loopSec
        {12,{0,tFloat,t01,glsl,GLSL_FRACT,tn}},                 // fract -> [0,1)
        {133,{0,tFloat,fphase,t01,c_N}},                        // *N -> [0,N)
        {110,{0,tInt,idxF,fphase}},                             // OpConvertFToS
        {12,{0,tInt,idxC,glsl,GLSL_SCLAMP,idxF,c_i0,c_iN1}},    // clamp [0,N-1]
        {65,{0,pV3,ac0,var0,idxC}}, {61,{0,tV3,acc0,ac0}},      // row0 = M[idx].(a,b,c)  (frame-SNAP, exact key)
        {65,{0,pV3,ac1,var1,idxC}}, {61,{0,tV3,acc1,ac1}},      // row1 = M[idx].(d,e,f)
    };
    uint32_t uvw=nid(); body.push_back({80,{0,tV3,uvw,coord,c_one}});   // vec3(coord.x, coord.y, 1.0)  (OpCompositeConstruct vec2+scalar)
    uint32_t ux=nid(); body.push_back({148,{0,tFloat,ux,acc0,uvw}});    // dot(row0, uvw) = a·u+b·v+c
    uint32_t uy=nid(); body.push_back({148,{0,tFloat,uy,acc1,uvw}});    // dot(row1, uvw) = d·u+e·v+f
    if (cellClamp){   // clamp to the CURRENT frame's cell (base-UV corners through the SAME frame matrix) minus pad
        const uint32_t GLSL_FMIN=37, GLSL_FMAX=40, GLSL_FCLAMP=43;
        uint32_t iu0=fconst(cellClamp[0]), iv0=fconst(cellClamp[1]), iu1=fconst(cellClamp[2]), iv1=fconst(cellClamp[3]);
        uint32_t ipu=fconst(cellClamp[4]), ipv=fconst(cellClamp[5]);
        uint32_t cA=nid(); newGlobals.push_back({0x2c,{0,tV3,cA,iu0,iv0,c_one}});   // vec3(umin,vmin,1)
        uint32_t cB=nid(); newGlobals.push_back({0x2c,{0,tV3,cB,iu1,iv1,c_one}});   // vec3(umax,vmax,1)
        uint32_t c1x=nid(); body.push_back({148,{0,tFloat,c1x,acc0,cA}});          // cell corner 1 = M·(umin,vmin)
        uint32_t c1y=nid(); body.push_back({148,{0,tFloat,c1y,acc1,cA}});
        uint32_t c2x=nid(); body.push_back({148,{0,tFloat,c2x,acc0,cB}});          // cell corner 2 = M·(umax,vmax)
        uint32_t c2y=nid(); body.push_back({148,{0,tFloat,c2y,acc1,cB}});
        uint32_t lox=nid(); body.push_back({12,{0,tFloat,lox,glsl,GLSL_FMIN,c1x,c2x}});   // min/max = flip-safe (negative scale)
        uint32_t hix=nid(); body.push_back({12,{0,tFloat,hix,glsl,GLSL_FMAX,c1x,c2x}});
        uint32_t loy=nid(); body.push_back({12,{0,tFloat,loy,glsl,GLSL_FMIN,c1y,c2y}});
        uint32_t hiy=nid(); body.push_back({12,{0,tFloat,hiy,glsl,GLSL_FMAX,c1y,c2y}});
        uint32_t loxp=nid(); body.push_back({129,{0,tFloat,loxp,lox,ipu}});        // lo+pad
        uint32_t hixm=nid(); body.push_back({131,{0,tFloat,hixm,hix,ipu}});        // hi-pad
        uint32_t loyp=nid(); body.push_back({129,{0,tFloat,loyp,loy,ipv}});
        uint32_t hiym=nid(); body.push_back({131,{0,tFloat,hiym,hiy,ipv}});
        uint32_t ux2=nid(); body.push_back({12,{0,tFloat,ux2,glsl,GLSL_FCLAMP,ux,loxp,hixm}});
        uint32_t uy2=nid(); body.push_back({12,{0,tFloat,uy2,glsl,GLSL_FCLAMP,uy,loyp,hiym}});
        ux=ux2; uy=uy2;
    }
    // WRAP the scrolled UV with fract() to match the desktop's REPEAT sampler. A SCROLL (dust c=0.729,
    // fog c=5.8) pushes U past [0,1]; if the device sampler is CLAMP it lands OFF the (faint) sprite ->
    // invisible + looks static. fract() = seamless wrap regardless of sampler mode. No-op for sprite-cell
    // atlases (their UVs already lie in [0,1]) and for REPEAT samplers, so no regression to fog/steam.
    uint32_t uxf=nid(); body.push_back({12,{0,tFloat,uxf,glsl,GLSL_FRACT,ux}});
    uint32_t uyf=nid(); body.push_back({12,{0,tFloat,uyf,glsl,GLSL_FRACT,uy}});
    uint32_t newUv=nid(); body.push_back({80,{0,tV2,newUv,uxf,uyf}});   // vec2(fract(ux),fract(uy))
    std::vector<Inst> out; bool addedG=false;
    for (size_t k=0;k<insts.size();++k){
        if (!addedG && insts[k].op==54){ for(auto&g:newGlobals)out.push_back(g); addedG=true; }
        if ((int)k==sampIdx){ for(auto&b:body) out.push_back(b); Inst s=insts[k]; s.w[4]=newUv; out.push_back(s); continue; }
        out.push_back(insts[k]);
    }
    if (!addedG) return {};
    std::vector<uint32_t> words={0x07230203u,version,generator,bound,0u};
    for(auto&t2:out){ uint32_t hdr=((uint32_t)t2.w.size()<<16)|(uint32_t)t2.op; words.push_back(hdr); for(size_t j=1;j<t2.w.size();++j)words.push_back(t2.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(),words.data(),mod.size());
    return mod;
}

// FRAGMENT ALPHA FADE by EXACT per-frame replay — multiplies the base-color sample's ALPHA by the mat.sanim
// MaterialTint alpha curve (fade[N], e.g. fog 0..0.22), frame-SNAP selected at fract(time/loopSec)*N (same phase
// math as the flipbook). Ports V79's fog/dust OPACITY fade so the card fades IN then fully OUT (alpha→0) over the
// loop — which HIDES the one-way node-translate's teleport reset AND its low/underground tail (both occur while
// alpha≈0). Applied ON TOP of the FLIPBOOK UV replay module (re-parses & edits the SAME forward-frag SPIR-V).
inline std::vector<uint8_t> editFragAlphaFade(const uint8_t* sd, uint32_t spvLen, const std::vector<float>& fade, int N, float loopSec){
    using namespace detail;
    if (N < 2 || (int)fade.size() < N) return {};
    size_t nw=spvLen/4; auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5;i<nw;){ uint32_t ins=W(i),wc=ins>>16,op=ins&0xffff; if(!wc)break; Inst t; t.op=(int)op; for(uint32_t k=0;k<wc;++k)t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }
    uint32_t tFloat=0,tInt=0,tV4=0,glsl=0,gu=0; int timeIdx=-1;
    std::map<int64_t,uint32_t> fltc; std::map<int32_t,uint32_t> intc; std::map<uint64_t,uint32_t> ptr;
    std::map<uint32_t,std::string> names;
    auto fround=[](float v){ return (int64_t)llround((double)v*1e6); };
    for (auto& t:insts){ auto&w=t.w;
        if (t.op==5) names[w[1]]=wstr(w,2);
        else if (t.op==6){ if (wstr(w,3)=="time") timeIdx=(int)w[2]; }
        else if (t.op==11 && wstr(w,2).find("GLSL")!=std::string::npos) glsl=w[1];
        else if (t.op==22 && w[2]==32) tFloat=w[1];
        else if (t.op==21 && w[2]==32 && (w.size()<4 || w[3]==1)) tInt=w[1]; }   // SIGNED int32 only (OpConvertFToS/SClamp require it)
    for (auto& t:insts){ auto&w=t.w; if (t.op==23 && w[2]==tFloat && w[3]==4) tV4=w[1]; else if (t.op==32) ptr[((uint64_t)w[2]<<32)|w[3]]=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==43 && w[1]==tInt){ int32_t iv; memcpy(&iv,&w[3],4); intc[iv]=w[2]; } else if (t.op==43 && w[1]==tFloat){ float fv; memcpy(&fv,&w[3],4); fltc[fround(fv)]=w[2]; } }
    for (auto& kv:names) if (kv.second=="globalUniforms") gu=kv.first;
    if (!tFloat||!glsl||!gu||timeIdx<0) return {};
    // base-color sample = FIRST OpImageSampleImplicitLod (op 87); its vec4 result carries the alpha we fade.
    int sampIdx=-1; uint32_t sampRes=0;
    for (size_t k=0;k<insts.size();++k){ if (insts[k].op==87 && insts[k].w.size()>=5){ sampIdx=(int)k; sampRes=insts[k].w[2]; break; } }
    if (sampIdx<0 || !sampRes) return {};
    std::vector<Inst> newGlobals;   // ONE ordered stream (types/consts/private vars) — each def before use
    auto nid=[&](){ return bound++; };
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    auto fconst=[&](float val)->uint32_t{ int64_t k=fround(val); auto it=fltc.find(k); if(it!=fltc.end())return it->second; uint32_t id=nid(); newGlobals.push_back({43,{0,tFloat,id,fbits(val)}}); fltc[k]=id; return id; };
    if (!tInt){ tInt=nid(); newGlobals.push_back({21,{0,tInt,32,1}}); }   // SIGNED int32 (create when the module lacks one)
    auto iconst=[&](int32_t val)->uint32_t{ auto it=intc.find(val); if(it!=intc.end())return it->second; uint32_t id=nid(); newGlobals.push_back({43,{0,tInt,id,(uint32_t)val}}); intc[val]=id; return id; };
    auto ptrOf=[&](uint32_t sc,uint32_t ty)->uint32_t{ uint64_t k=((uint64_t)sc<<32)|ty; auto it=ptr.find(k); if(it!=ptr.end())return it->second; uint32_t id=nid(); newGlobals.push_back({32,{0,id,sc,ty}}); ptr[k]=id; return id; };
    if (!tV4){ tV4=nid(); newGlobals.push_back({23,{0,tV4,tFloat,4}}); }
    const uint32_t GLSL_FRACT=10, GLSL_SCLAMP=45;
    if (loopSec<=1e-4f) loopSec=1.f;
    uint32_t c_invloop=fconst(1.0f/loopSec), c_N=fconst((float)N), c_one=fconst(1.0f), c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
    std::vector<uint32_t> fc(N); for(int i=0;i<N;i++) fc[i]=fconst(fade[i]);
    // CONSTANT-ARRAY curve (same zen-tree "device sticks" encoding as editFragTintReplay/editFragUvMatrixReplay):
    // the old unrolled step-select chain (~5N fragment ALU ops) is the encoding class the Quest driver executes
    // as garbage on long curves. float[N] Private array + ONE dynamically-indexed load; keys byte-exact.
    uint32_t cLen=iconst(N), c_i0=iconst(0), c_iN1=iconst(N-1);
    uint32_t tArr=nid(); newGlobals.push_back({28,{0,tArr,tFloat,cLen}});               // OpTypeArray float[N]
    uint32_t arrId=nid();
    { Inst a; a.op=0x2c; a.w.push_back(0); a.w.push_back(tArr); a.w.push_back(arrId);   // OpConstantComposite float[N]{fade...}
      for (int i=0;i<N;i++) a.w.push_back(fc[i]); newGlobals.push_back(std::move(a)); }
    uint32_t pArr=ptrOf(6,tArr);
    uint32_t var=nid(); newGlobals.push_back({59,{0,pArr,var,6,arrId}});                // OpVariable Private, initializer=curve
    uint32_t pFl=ptrOf(6,tFloat);
    uint32_t pt=nid(),tt=nid(),tn=nid(),t01=nid(),fphase=nid(),idxF=nid(),idxC=nid(),ac2=nid(),acc=nid();
    std::vector<Inst> pre = {
        {65,{0,pUf,pt,gu,c_time}}, {61,{0,tFloat,tt,pt}},       // time
        {133,{0,tFloat,tn,tt,c_invloop}},                       // time/loopSec
        {12,{0,tFloat,t01,glsl,GLSL_FRACT,tn}},                 // fract -> [0,1)
        {133,{0,tFloat,fphase,t01,c_N}},                        // *N -> [0,N)
        {110,{0,tInt,idxF,fphase}},                             // OpConvertFToS
        {12,{0,tInt,idxC,glsl,GLSL_SCLAMP,idxF,c_i0,c_iN1}},    // clamp [0,N-1]
        {65,{0,pFl,ac2,var,idxC}},                              // OpAccessChain curve[idx]
        {61,{0,tFloat,acc,ac2}},                                // OpLoad (the EXACT authored key, frame-SNAP)
    };
    uint32_t v4=nid(), faded=nid();
    std::vector<Inst> out; bool addedG=false;
    for (size_t k=0;k<insts.size();++k){
        if (!addedG && insts[k].op==54){ for(auto&g:newGlobals)out.push_back(g); addedG=true; }
        if ((int)k==sampIdx){
            for(auto&b:pre) out.push_back(b);
            out.push_back(insts[k]);                                       // the base-color sample (result = sampRes)
            out.push_back({0x50,{0,tV4,v4,c_one,c_one,c_one,acc}});        // OpCompositeConstruct vec4(1,1,1,fadeA)
            out.push_back({133,{0,tV4,faded,sampRes,v4}});                 // OpFMul: sampled·vec4 -> alpha *= fadeA
            continue;
        }
        if ((int)k>sampIdx){ Inst t2=insts[k]; for(size_t j=1;j<t2.w.size();++j) if(t2.w[j]==sampRes) t2.w[j]=faded; out.push_back(t2); continue; }
        out.push_back(insts[k]);
    }
    if (!addedG) return {};
    std::vector<uint32_t> words={0x07230203u,version,generator,bound,0u};
    for(auto&t2:out){ uint32_t hdr=((uint32_t)t2.w.size()<<16)|(uint32_t)t2.op; words.push_back(hdr); for(size_t j=1;j<t2.w.size();++j)words.push_back(t2.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(),words.data(),mod.size());
    return mod;
}

// TINTREPLAY in the FORWARD FRAGMENT: per-frame RGBA MaterialTint replay — color *= tint[floor(fract(time/loop)*N)].
// The RGBA generalization of editFragAlphaFade (same phase math, same sample-splice); ports V79's mat.sanim
// MaterialTint COLOR cycling (stinson fireworks flash colors, city window-light flicker) which the cook dropped —
// devices showed one static "generic" color. rgba = N*4 floats. The step(i,phase) weight is computed ONCE per
// frame and shared by all four channel accumulators.
inline std::vector<uint8_t> editFragTintReplay(const uint8_t* sd, uint32_t spvLen, const std::vector<float>& rgba, int N, float loopSec){
    using namespace detail;
    if (N < 2 || (int)rgba.size() < N*4) return {};
    size_t nw=spvLen/4; auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5;i<nw;){ uint32_t ins=W(i),wc=ins>>16,op=ins&0xffff; if(!wc)break; Inst t; t.op=(int)op; for(uint32_t k=0;k<wc;++k)t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }
    uint32_t tFloat=0,tInt=0,tV4=0,glsl=0,gu=0; int timeIdx=-1;
    std::map<int64_t,uint32_t> fltc; std::map<int32_t,uint32_t> intc; std::map<uint64_t,uint32_t> ptr;
    std::map<uint32_t,std::string> names;
    auto fround=[](float v){ return (int64_t)llround((double)v*1e6); };
    for (auto& t:insts){ auto&w=t.w;
        if (t.op==5) names[w[1]]=wstr(w,2);
        else if (t.op==6){ if (wstr(w,3)=="time") timeIdx=(int)w[2]; }
        else if (t.op==11 && wstr(w,2).find("GLSL")!=std::string::npos) glsl=w[1];
        else if (t.op==22 && w[2]==32) tFloat=w[1];
        else if (t.op==21 && w[2]==32 && (w.size()<4 || w[3]==1)) tInt=w[1]; }   // SIGNED int32 only (OpConvertFToS/SClamp require it)
    for (auto& t:insts){ auto&w=t.w; if (t.op==23 && w[2]==tFloat && w[3]==4) tV4=w[1]; else if (t.op==32) ptr[((uint64_t)w[2]<<32)|w[3]]=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==43 && w[1]==tInt){ int32_t iv; memcpy(&iv,&w[3],4); intc[iv]=w[2]; } else if (t.op==43 && w[1]==tFloat){ float fv; memcpy(&fv,&w[3],4); fltc[fround(fv)]=w[2]; } }
    for (auto& kv:names) if (kv.second=="globalUniforms") gu=kv.first;
    if (!tFloat||!tInt||!glsl||!gu||timeIdx<0) return {};
    int sampIdx=-1; uint32_t sampRes=0;   // base-color sample = FIRST OpImageSampleImplicitLod
    for (size_t k=0;k<insts.size();++k){ if (insts[k].op==87 && insts[k].w.size()>=5){ sampIdx=(int)k; sampRes=insts[k].w[2]; break; } }
    if (sampIdx<0 || !sampRes) return {};
    // CONSTANT-ARRAY curve (the device "sticks" fix — user-proven: the tint chain broke the mobile
    // driver, and the CURVE ENCODING was the problem, NOT the keys): the old emitter UNROLLED N steps
    // into ~16N ALU instructions per fragment (~42K words for the zen tree's 512-key cycle) which
    // desktop GPUs ran but the Quest driver executed as garbage stripes. Emit the SAME N keys VERBATIM
    // as an OpConstantComposite vec4 ARRAY in a Private variable and do ONE dynamically-indexed load —
    // exactly how a GLSL `const vec4 curve[N]` compiles. Frame-snap semantics identical; every key
    // byte-exact; module grows by ~4N data words instead of ~16N code words.
    std::vector<Inst> newGlobals;   // ONE ordered stream (types/consts/private vars) — each def before use
    auto nid=[&](){ return bound++; };
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    auto fconst=[&](float val)->uint32_t{ int64_t k=fround(val); auto it=fltc.find(k); if(it!=fltc.end())return it->second; uint32_t id=nid(); newGlobals.push_back({43,{0,tFloat,id,fbits(val)}}); fltc[k]=id; return id; };
    auto iconst=[&](int32_t val)->uint32_t{ auto it=intc.find(val); if(it!=intc.end())return it->second; uint32_t id=nid(); newGlobals.push_back({43,{0,tInt,id,(uint32_t)val}}); intc[val]=id; return id; };
    auto ptrOf=[&](uint32_t sc,uint32_t ty)->uint32_t{ uint64_t k=((uint64_t)sc<<32)|ty; auto it=ptr.find(k); if(it!=ptr.end())return it->second; uint32_t id=nid(); newGlobals.push_back({32,{0,id,sc,ty}}); ptr[k]=id; return id; };
    if (!tV4){ tV4=nid(); newGlobals.push_back({23,{0,tV4,tFloat,4}}); }
    const uint32_t GLSL_FRACT=10, GLSL_SCLAMP=45;
    if (loopSec<=1e-4f) loopSec=1.f;
    uint32_t c_invloop=fconst(1.0f/loopSec), c_N=fconst((float)N), c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
    uint32_t c_i0=iconst(0), c_iN1=iconst(N-1);
    // per-key vec4 constants -> array constant -> Private variable with initializer
    std::vector<uint32_t> keyIds((size_t)N);
    for (int i=0;i<N;i++){
        uint32_t r=fconst(rgba[(size_t)i*4]), g=fconst(rgba[(size_t)i*4+1]), b=fconst(rgba[(size_t)i*4+2]), a=fconst(rgba[(size_t)i*4+3]);
        uint32_t kc=nid(); newGlobals.push_back({44,{0,tV4,kc,r,g,b,a}}); keyIds[(size_t)i]=kc;   // OpConstantComposite vec4
    }
    uint32_t cLen=iconst(N);
    uint32_t tArr=nid(); newGlobals.push_back({28,{0,tArr,tV4,cLen}});                              // OpTypeArray vec4[N]
    uint32_t arrId=nid();
    { Inst arrC; arrC.op=44; arrC.w.push_back(0); arrC.w.push_back(tArr); arrC.w.push_back(arrId); // OpConstantComposite vec4[N]{keys...}
      for (int i=0;i<N;i++) arrC.w.push_back(keyIds[(size_t)i]);
      newGlobals.push_back(std::move(arrC)); }
    uint32_t pArr=ptrOf(6,tArr);                                                                    // Private ptr to array
    uint32_t var=nid(); newGlobals.push_back({59,{0,pArr,var,6,arrId}});                            // OpVariable Private, initializer=array
    uint32_t pV4=ptrOf(6,tV4);
    uint32_t pt2=nid(),tt=nid(),tn=nid(),t01=nid(),fphase=nid(),idxF=nid(),idxC=nid(),ac2=nid(),tintv=nid(),tinted=nid();
    std::vector<Inst> pre = {
        {65,{0,pUf,pt2,gu,c_time}}, {61,{0,tFloat,tt,pt2}},     // time
        {133,{0,tFloat,tn,tt,c_invloop}},                       // time/loopSec
        {12,{0,tFloat,t01,glsl,GLSL_FRACT,tn}},                 // fract -> [0,1)
        {133,{0,tFloat,fphase,t01,c_N}},                        // *N -> [0,N)
        {110,{0,tInt,idxF,fphase}},                             // OpConvertFToS
        {12,{0,tInt,idxC,glsl,GLSL_SCLAMP,idxF,c_i0,c_iN1}},    // clamp [0,N-1]
        {65,{0,pV4,ac2,var,idxC}},                              // OpAccessChain curve[idx]
        {61,{0,tV4,tintv,ac2}},                                 // OpLoad vec4 (the EXACT authored key)
    };
    std::vector<Inst> out; bool addedG=false;
    for (size_t k=0;k<insts.size();++k){
        if (!addedG && insts[k].op==54){ for(auto&t2:newGlobals)out.push_back(t2); addedG=true; }
        if ((int)k==sampIdx){
            for(auto&b:pre) out.push_back(b);
            out.push_back(insts[k]);                                            // the base-color sample (result = sampRes)
            out.push_back({133,{0,tV4,tinted,sampRes,tintv}});                  // OpFMul: sampled·curve[idx]
            continue;
        }
        if ((int)k>sampIdx){ Inst t2=insts[k]; for(size_t j=1;j<t2.w.size();++j) if(t2.w[j]==sampRes) t2.w[j]=tinted; out.push_back(t2); continue; }
        out.push_back(insts[k]);
    }
    if (!addedG) return {};
    std::vector<uint32_t> words={0x07230203u,version,generator,bound,0u};
    for(auto&t2:out){ uint32_t hdr=((uint32_t)t2.w.size()<<16)|(uint32_t)t2.op; words.push_back(hdr); for(size_t j=1;j<t2.w.size();++j)words.push_back(t2.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(),words.data(),mod.size());
    return mod;
}

// VERTEX-stage TINT REPLAY (the device leaf-color fix): BOTH fragment encodings of the tint curve (512-step
// unroll AND const-array indexed load) rendered as garbage "sticks" on the Quest driver while running perfectly
// on desktop — but the r25 plain-cutout FRAG is device-proven, and the getTime VERTEX edits (rot/osc/scale) are
// device-proven. So evaluate the curve PER-VERTEX and fold it into the color the frag already multiplies:
// find the Output block's member-0 (color) store `Out.color = v.vertexColor0` and multiply the stored value by
// curve[int(fract(time/loop)*N)] — same const-array data (EVERY key verbatim), executed 794 times instead of
// per-fragment, and the fragment module stays byte-identical to the working cutout.
inline std::vector<uint8_t> editVertTintReplay(const uint8_t* sd, uint32_t spvLen, const std::vector<float>& rgba, int N, float loopSec){
    using namespace detail;
    if (N < 2 || (int)rgba.size() < N*4) return {};
    size_t nw=spvLen/4; auto W=[&](size_t k){ return u32(sd,spvLen,(int64_t)k*4); };
    uint32_t version=W(1), generator=W(2), bound=W(3);
    std::vector<Inst> insts;
    for (size_t i=5;i<nw;){ uint32_t ins=W(i),wc=ins>>16,op=ins&0xffff; if(!wc)break; Inst t; t.op=(int)op; for(uint32_t k=0;k<wc;++k)t.w.push_back(W(i+k)); insts.push_back(std::move(t)); i+=wc; }
    uint32_t tFloat=0,tInt=0,tV4=0,glsl=0,gu=0; int timeIdx=-1;
    std::map<int64_t,uint32_t> fltc; std::map<int32_t,uint32_t> intc; std::map<uint64_t,uint32_t> ptr;
    std::map<uint32_t,std::string> names;
    auto fround=[](float v){ return (int64_t)llround((double)v*1e6); };
    for (auto& t:insts){ auto&w=t.w;
        if (t.op==5) names[w[1]]=wstr(w,2);
        else if (t.op==6){ if (wstr(w,3)=="time") timeIdx=(int)w[2]; }
        else if (t.op==11 && wstr(w,2).find("GLSL")!=std::string::npos) glsl=w[1];
        else if (t.op==22 && w[2]==32) tFloat=w[1];
        else if (t.op==21 && w[2]==32 && (w.size()<4 || w[3]==1)) tInt=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==23 && w[2]==tFloat && w[3]==4) tV4=w[1]; else if (t.op==32) ptr[((uint64_t)w[2]<<32)|w[3]]=w[1]; }
    for (auto& t:insts){ auto&w=t.w; if (t.op==43 && w[1]==tInt){ int32_t iv; memcpy(&iv,&w[3],4); intc[iv]=w[2]; } else if (t.op==43 && w[1]==tFloat){ float fv; memcpy(&fv,&w[3],4); fltc[fround(fv)]=w[2]; } }
    for (auto& kv:names) if (kv.second=="globalUniforms") gu=kv.first;
    if (!tFloat||!tInt||!glsl||!gu||timeIdx<0||!tV4) return {};
    // find the Output-class struct variable + the store to its member 0 (Out.color)
    std::set<uint32_t> outVars;
    for (auto& t:insts) if (t.op==59 && t.w.size()>=4 && t.w[3]==3) outVars.insert(t.w[2]);   // OpVariable Output
    uint32_t c0int=0; { auto it=intc.find(0); if(it!=intc.end()) c0int=it->second; }
    int storeIdx=-1; uint32_t storeVal=0, colorPtr=0;
    std::map<uint32_t,Inst*> byId;
    for (auto& t:insts) if (t.w.size()>=3 && (t.op==65)) byId[t.w[2]]=&t;   // access chains by result id
    for (size_t k=0;k<insts.size();++k){
        Inst& t=insts[k];
        if (t.op!=62 || t.w.size()<3) continue;   // OpStore
        auto it=byId.find(t.w[1]); if (it==byId.end()) continue;
        Inst& ac=*it->second;                     // OpAccessChain: [type,res,base,idx...]
        if (ac.w.size()<5) continue;
        if (!outVars.count(ac.w[3])) continue;
        if (c0int && ac.w[4]!=c0int) continue;    // member 0 = color (when int-0 const known; else first store wins)
        storeIdx=(int)k; storeVal=t.w[2]; colorPtr=t.w[1]; break;
    }
    if (storeIdx<0) return {};
    (void)colorPtr;
    std::vector<Inst> newGlobals;
    auto nid=[&](){ return bound++; };
    auto fbits=[](float f){ uint32_t u; memcpy(&u,&f,4); return u; };
    auto fconst=[&](float val)->uint32_t{ int64_t k=fround(val); auto it=fltc.find(k); if(it!=fltc.end())return it->second; uint32_t id=nid(); newGlobals.push_back({43,{0,tFloat,id,fbits(val)}}); fltc[k]=id; return id; };
    auto iconst=[&](int32_t val)->uint32_t{ auto it=intc.find(val); if(it!=intc.end())return it->second; uint32_t id=nid(); newGlobals.push_back({43,{0,tInt,id,(uint32_t)val}}); intc[val]=id; return id; };
    auto ptrOf=[&](uint32_t sc,uint32_t ty)->uint32_t{ uint64_t k=((uint64_t)sc<<32)|ty; auto it=ptr.find(k); if(it!=ptr.end())return it->second; uint32_t id=nid(); newGlobals.push_back({32,{0,id,sc,ty}}); ptr[k]=id; return id; };
    const uint32_t GLSL_FRACT=10, GLSL_SCLAMP=45;
    if (loopSec<=1e-4f) loopSec=1.f;
    uint32_t c_invloop=fconst(1.0f/loopSec), c_N=fconst((float)N), c_time=iconst(timeIdx), pUf=ptrOf(2,tFloat);
    uint32_t c_i0=iconst(0), c_iN1=iconst(N-1);
    std::vector<uint32_t> keyIds((size_t)N);
    for (int i=0;i<N;i++){
        uint32_t r=fconst(rgba[(size_t)i*4]), g=fconst(rgba[(size_t)i*4+1]), b=fconst(rgba[(size_t)i*4+2]), a=fconst(rgba[(size_t)i*4+3]);
        uint32_t kc=nid(); newGlobals.push_back({44,{0,tV4,kc,r,g,b,a}}); keyIds[(size_t)i]=kc;
    }
    uint32_t cLen=iconst(N);
    uint32_t tArr=nid(); newGlobals.push_back({28,{0,tArr,tV4,cLen}});
    uint32_t arrId=nid();
    { Inst arrC; arrC.op=44; arrC.w.push_back(0); arrC.w.push_back(tArr); arrC.w.push_back(arrId);
      for (int i=0;i<N;i++) arrC.w.push_back(keyIds[(size_t)i]);
      newGlobals.push_back(std::move(arrC)); }
    uint32_t pArr=ptrOf(6,tArr);
    uint32_t var=nid(); newGlobals.push_back({59,{0,pArr,var,6,arrId}});
    uint32_t pV4=ptrOf(6,tV4);
    uint32_t pt2=nid(),tt=nid(),tn=nid(),t01=nid(),fphase=nid(),idxF=nid(),idxC=nid(),ac2=nid(),tintv=nid(),tinted=nid();
    std::vector<Inst> pre = {
        {65,{0,pUf,pt2,gu,c_time}}, {61,{0,tFloat,tt,pt2}},
        {133,{0,tFloat,tn,tt,c_invloop}},
        {12,{0,tFloat,t01,glsl,GLSL_FRACT,tn}},
        {133,{0,tFloat,fphase,t01,c_N}},
        {110,{0,tInt,idxF,fphase}},
        {12,{0,tInt,idxC,glsl,GLSL_SCLAMP,idxF,c_i0,c_iN1}},
        {65,{0,pV4,ac2,var,idxC}},
        {61,{0,tV4,tintv,ac2}},
    };
    std::vector<Inst> out; bool addedG=false;
    for (size_t k=0;k<insts.size();++k){
        if (!addedG && insts[k].op==54){ for(auto&t2:newGlobals)out.push_back(t2); addedG=true; }
        if ((int)k==storeIdx){
            for(auto&b:pre) out.push_back(b);
            out.push_back({133,{0,tV4,tinted,storeVal,tintv}});   // OpFMul color·curve[idx]
            Inst st=insts[k]; st.w[2]=tinted; out.push_back(st);  // store the tinted color instead
            continue;
        }
        out.push_back(insts[k]);
    }
    if (!addedG) return {};
    std::vector<uint32_t> words={0x07230203u,version,generator,bound,0u};
    for(auto&t2:out){ uint32_t hdr=((uint32_t)t2.w.size()<<16)|(uint32_t)t2.op; words.push_back(hdr); for(size_t j=1;j<t2.w.size();++j)words.push_back(t2.w[j]); }
    std::vector<uint8_t> mod(words.size()*4); memcpy(mod.data(),words.data(),mod.size());
    return mod;
}

// ── GENERAL round-trip: compile arbitrary GLSL → SPIR-V (bundled tools/glslangValidator.exe) and swap it into a
//    RENDSHAD .surface's forward-FRAGMENT stage. This is the "transform ANY shader" path — instead of hand-patching
//    a fixed base with a getTime enum, the cook can emit a WHOLE target program (any fade/scroll/noise/cell/timing,
//    even ones needing extra samplers) as GLSL and compile it 1:1. Returns {} on any failure so the caller can fall
//    back to the SPIR-V-edit path. glslangValidator located via $HSR_GLSLANG or tools/glslangValidator.exe (CWD). ──
inline std::vector<uint8_t> compileGlslToSpv(const std::string& glsl, char stage /*'f'|'v'*/){
    std::string gv = "tools/glslangValidator.exe";
    if (const char* e = std::getenv("HSR_GLSLANG")) gv = e;
    const char* tmp = std::getenv("TEMP"); if (!tmp) tmp = std::getenv("TMP"); if (!tmp) tmp = ".";
    static int seq = 0; char base[600];
    snprintf(base, sizeof base, "%s/_sg_%d_%d", tmp, (int)(size_t)&glsl & 0xffff, seq++);
    std::string src = std::string(base) + (stage=='v' ? ".vert" : ".frag");
    std::string out = std::string(base) + ".spv";
    { FILE* f = fopen(src.c_str(), "wb"); if (!f) return {}; fwrite(glsl.data(), 1, glsl.size(), f); fclose(f); }
    char cmd[1800];
    snprintf(cmd, sizeof cmd, "\"%s\" -V --target-env vulkan1.0 -S %s \"%s\" -o \"%s\" >nul 2>&1",
             gv.c_str(), stage=='v' ? "vert" : "frag", src.c_str(), out.c_str());
    int rc = system(cmd);
    std::vector<uint8_t> spv;
    if (rc == 0) { FILE* f = fopen(out.c_str(), "rb");
        if (f) { fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
                 if (n>0){ spv.resize((size_t)n); size_t rd=fread(spv.data(),1,(size_t)n,f); if(rd!=(size_t)n) spv.clear(); } fclose(f); } }
    remove(src.c_str()); remove(out.c_str());
    if (spv.size()>=20 && *(const uint32_t*)spv.data()!=0x07230203u) spv.clear();   // must be valid SPIR-V
    return spv;
}
// Swap a compiled SPIR-V module into the base .surface's forward-FRAG slot (append at EOF + repoint the FlatBuffer
// uoffset) — identical mechanism to generate()/editFragCutout. Returns {} if the slot can't be found.
inline std::vector<uint8_t> swapFragSpv(const std::vector<uint8_t>& src, const std::vector<uint8_t>& fragSpv){
    using namespace detail;
    const uint8_t* d = src.data(); size_t N = src.size();
    int64_t slot, spvOff; uint32_t spvLen;
    if (fragSpv.size() < 20 || !findFwdFragSpv(d, N, slot, spvOff, spvLen)) return {};
    std::vector<uint8_t> o = src;
    while (o.size() % 4) o.push_back(0);
    uint32_t nv = (uint32_t)o.size(), modLen = (uint32_t)fragSpv.size();
    o.insert(o.end(), (uint8_t*)&modLen, (uint8_t*)&modLen + 4);
    o.insert(o.end(), fragSpv.begin(), fragSpv.end());
    uint32_t rel = nv - (uint32_t)slot; memcpy(o.data() + slot, &rel, 4);   // repoint stage uoffset -> new module
    return o;
}
// Convenience: compile a whole FRAGMENT GLSL program and swap it into src. {} on failure (caller falls back).
inline std::vector<uint8_t> transformFragGlsl(const std::vector<uint8_t>& src, const std::string& fragGlsl){
    auto spv = compileGlslToSpv(fragGlsl, 'f');
    if (spv.empty()) return {};
    return swapFragSpv(src, spv);
}

// Generate an animated shader from a stock RENDSHAD `src`. Returns the new .surface.bin bytes (empty on failure).
//   ROTATE: p0=omega(rad/s), axis | OSCILLATE: p0=amp(rad), p1=period(s), axis | UVSCROLL: p0=rateU, p1=rateV
// Animates EVERY vertex stage (all passes) so geometry/UV is consistent across the V205 multi-pass RenderGraph
// (forward + depth + shadow + motion-vector). Editing only the forward stage left depth/motion using the un-animated
// vertex on device -> depth-test/cull/motion mismatch = "animated meshes/textures don't render on device". (UV-scroll's
// depth-only stages lack inUv -> editVertModule returns {} -> harmlessly skipped, so only real position stages grow.)
inline std::vector<uint8_t> generate(const std::vector<uint8_t>& src, Mode mode, float p0, float p1=0, float ax=0, float ay=1, float az=0,
                                     const std::vector<float>& tframes = {}, int tN = 0,
                                     const std::vector<float>& fade = {}, float fadeLoop = 0.f,   // FLIPBOOK: opacity-fade curve (mat.sanim tint alpha) + its loop
                                     const float* fbCellClamp = nullptr){                         // FLIPBOOK replay: {umin,vmin,umax,vmax,padU,padV} per-frame cell clamp (spritesheet seam fix)
    using namespace detail;
    const uint8_t* d = src.data(); size_t N = src.size();
    if (mode==CUTOUT){   // alpha-test discard in EVERY color fragment stage (all passes/LOD techniques — the
        // single-stage edit left the plain-forward/LOD frags UN-CUT: distant balloons drew opaque rectangles)
        return editAllFragStages(src, [&](const uint8_t* sd, uint32_t L){ return editFragCutout(sd, L, p0>0.f ? p0 : 0.5f); });
    }
    if (mode==FOLIAGE){   // sharpen-alpha masked foliage (pair with the MATL masked/a2c flags); p0 = alpha cutoff
        return editAllFragStages(src, [&](const uint8_t* sd, uint32_t L){ return editFragFoliageSharpen(sd, L, p0>0.f ? p0 : 0.30f); });
    }
    if (mode==TINTREPLAY_VERT){   // per-frame RGBA MaterialTint replay in the forward VERTEX (skinned meshes: the
        // fragment tint edits — unroll AND const-array — both broke the Quest driver; vertex getTime edits are
        // device-proven, and the frag already multiplies base×vertexColor0, so the vert multiply = V79-exact)
        int64_t slot,spvOff; uint32_t spvLen;
        if (!detail::findFwdVertSpv(d,N,slot,spvOff,spvLen)) return {};
        std::vector<uint8_t> mod = editVertTintReplay(d+spvOff, spvLen, tframes, tN, p0);   // p0 = loopSec
        if (mod.empty()) return {};
        std::vector<uint8_t> o = src;
        while (o.size()%4) o.push_back(0);
        uint32_t nv=(uint32_t)o.size(), modLen=(uint32_t)mod.size();
        o.insert(o.end(),(uint8_t*)&modLen,(uint8_t*)&modLen+4);
        o.insert(o.end(),mod.begin(),mod.end());
        (void)slot; detail::repointAll(o, (size_t)nv, spvOff-4, nv);   // repoint EVERY pass entry sharing the old vert module
        return o;
    }
    if (mode==TINTREPLAY){   // per-frame RGBA MaterialTint replay in EVERY color fragment stage (all passes/LODs)
        return editAllFragStages(src, [&](const uint8_t* sd, uint32_t L){ return editFragTintReplay(sd, L, tframes, tN, p0); });   // p0 = loopSec, tframes = tN*4 RGBA
    }
    if ((mode==FLIPBOOK || mode==UVSCROLL) && !std::getenv("HSR_VERTFLIP")){   // DEFAULT: FRAGMENT-stage UV anim (animates on device; vertex-UV did not)
        // EVERY color fragment stage (all passes/LOD techniques): the single-stage edit left lower-technique
        // frags with the STATIC base texture -> distant cards froze/showed the raw sheet.
        return editAllFragStages(src, [&](const uint8_t* sd, uint32_t L)->std::vector<uint8_t>{
            std::vector<uint8_t> mod;
            if (mode==FLIPBOOK) {
                if (!tframes.empty() && tN>=2)   // EXACT per-frame MATRIX replay (p0=loopSec, tframes=N*6 verbatim source matrices) — matches the desktop
                    mod = editFragUvMatrixReplay(sd, L, tframes, tN, p0, fbCellClamp);
                else                              // legacy derived cols/rows grid (p0=cols p1=rows ax=frames ay=fps az=offsetFlag)
                    mod = editFragFlipbook(sd, L, (int)(p0+0.5f),(int)(p1+0.5f),(int)(ax+0.5f), ay, az>0.5f);
                // OPACITY FADE (fog/dust MaterialTint alpha) layered on the SAME forward-frag module — fades the card to 0
                // at the loop ends so the node-translate's teleport reset + underground tail are invisible. fadeLoop synced
                // to the translate period by the cooker so alpha≈0 exactly when the card resets/dips low.
                if (!mod.empty() && !fade.empty() && (int)fade.size()>=2 && fadeLoop>1e-4f) {
                    auto m2 = editFragAlphaFade(mod.data(), (uint32_t)mod.size(), fade, (int)fade.size(), fadeLoop);
                    if (!m2.empty()) mod = m2;
                }
            } else mod = editFragUvScroll(sd, L, p0, p1);   // UVSCROLL: p0=rateU, p1=rateV
            return mod;
        });
    }
    if (mode==ROTATE || mode==OSCILLATE || mode==ROTREPLAY){ float l=std::sqrt(ax*ax+ay*ay+az*az); if (l<=0.f) l=1.f; ax/=l; ay/=l; az/=l; }  // axis -> unit (UVSCROLL/FLIPBOOK/TRANSLATE use these args as params, not an axis)
    std::vector<VStage> stages;
    if (mode==FLIPBOOK){
        // FLIPBOOK edits ONLY the forward vertex stage (exactly like make_flipbook_shader.py). The UV-cell offset doesn't
        // affect depth/position, so the depth/shadow/motion stages don't need it — and injecting the floor/fmod body into
        // those stages (the unlitblend base HAS inUv there) corrupted them on device = the WHOLE-ENV render break.
        int64_t slot=0, spvOff=0; uint32_t spvLen=0;
        if (detail::findFwdVertSpv(d, N, slot, spvOff, spvLen)) stages.push_back({slot, spvOff, spvLen});
    } else {
        collectVertStages(d, N, stages);   // ROTATE/OSCILLATE move POSITION -> every stage must match (depth/motion/shadow)
    }
    if (stages.empty()) return {};
    std::vector<uint8_t> o = src; int edited = 0;
    std::vector<int64_t> done;   // dedup by physical module (shared modules edited ONCE; repointAll moves every sharer)
    for (const auto& st : stages){
        bool dup=false; for (int64_t v : done) if (v==st.spvOff){ dup=true; break; }
        if (dup) continue; done.push_back(st.spvOff);
        std::vector<uint8_t> mod = editVertModule(o.data() + st.spvOff, st.spvLen, mode, p0, p1, ax, ay, az, tframes, tN);
        if (mod.empty()) continue;     // this vertex stage lacks the needed input for this mode -> skip (harmless)
        while (o.size()%4) o.push_back(0);
        uint32_t nv=(uint32_t)o.size(), modLen=(uint32_t)mod.size();
        o.insert(o.end(),(uint8_t*)&modLen,(uint8_t*)&modLen+4);
        o.insert(o.end(),mod.begin(),mod.end());
        // repoint EVERY table field referencing the old module — the single-slot memcpy left any OTHER pass
        // entry sharing the module on the UN-EDITED original, and which entry the device reads is table-order
        // dependent (synthwave sky cylinder: SPIN shipped in both edited copies yet the device drew a stale
        // shared reference = static skybox; same bug class as the balloons' fragment-side fix).
        detail::repointAll(o, (size_t)nv, st.spvOff-4, nv);
        ++edited;
    }
    return edited ? o : std::vector<uint8_t>();
}

} // namespace shadergen
