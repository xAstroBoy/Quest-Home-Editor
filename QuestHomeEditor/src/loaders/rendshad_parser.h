#pragma once
#include "core/types.h"
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

// RENDSHAD FlatBuffer parser — extracts SPIRV bytecode from shader assets
// Format: "SHAD" magic at +4, root FlatBuffer with vector of shader stages
// Each stage: stage type + SPIRV byte blob

struct SpirvBlob {
    u32 stageType = 0;
    std::vector<u32> code;
    std::string srcName;   // shader file this blob came from (for same-program pairing)
};

// Robust primary extractor: scan the whole RENDSHAD blob for SPIR-V modules by magic
// number and split them. A surface shader file packs MANY (vert,frag) variant modules
// back-to-back; the FlatBuffer walk below only reliably finds one of them for some envs
// (e.g. Haven's isotropictiled returned a single blob), so we scan for ALL of them. Each
// module's stage comes from its OpEntryPoint execution model.
inline bool scanSpirvModules(const std::vector<u8>& data, std::vector<SpirvBlob>& out) {
    static const u8 magic[4] = {0x03, 0x02, 0x23, 0x07};
    std::vector<size_t> pos;
    for (size_t p = 0; p + 20 <= data.size(); ++p) {
        if (memcmp(data.data() + p, magic, 4) != 0) continue;
        u32 version = *reinterpret_cast<const u32*>(data.data() + p + 4);
        u32 bound   = *reinterpret_cast<const u32*>(data.data() + p + 12);
        // Accept any SPIR-V 1.x version (0x0001xxYY) with a sane id bound.
        if ((version >> 16) == 1 && bound > 0 && bound < 1000000) pos.push_back(p);
    }
    for (size_t k = 0; k < pos.size(); ++k) {
        size_t start = pos[k];
        size_t maxEnd = (k + 1 < pos.size()) ? pos[k+1] : data.size();
        // CRITICAL: the bytes between this module and the next magic include FlatBuffer
        // structure (vector padding, offsets, vtables) that is NOT part of the SPIR-V.
        // Including it produces an invalid module that creates OK but is REJECTED at
        // pipeline creation (VK_ERROR_UNKNOWN). Walk the instruction stream to find the
        // module's exact end: stop at the first word that is not a valid instruction
        // header (wordCount==0 or an implausible opcode).
        const u32* w = reinterpret_cast<const u32*>(data.data() + start);
        size_t maxW = (maxEnd - start) / 4;
        size_t iw = 5;  // after the 5-word header
        while (iw < maxW) {
            u32 word = w[iw], op = word & 0xFFFF, wcnt = word >> 16;
            if (wcnt == 0 || op > 600 || iw + wcnt > maxW) break;
            iw += wcnt;
        }
        u32 byteSize = (u32)(iw * 4);
        if (byteSize < 200) continue;
        SpirvBlob blob;
        u32 wc = byteSize / 4;
        blob.code.resize(wc);
        memcpy(blob.code.data(), data.data() + start, byteSize);
        blob.stageType = 0xFFFFFFFFu;
        for (size_t wi = 5; wi + 1 < wc && wi < 60; ) {
            u32 word = blob.code[wi], op = word & 0xFFFF, w = word >> 16;
            if (w == 0) break;
            if (op == 15) { blob.stageType = blob.code[wi+1]; break; } // OpEntryPoint exec model
            wi += w;
        }
        // Only keep vertex(0) / fragment(4) modules.
        if (blob.stageType == 0 || blob.stageType == 4) out.push_back(std::move(blob));
    }
    return !out.empty();
}

// FAITHFUL variant selection (reversed from the RENDSHAD format, NOT heuristics): the SHAD FlatBuffer
// holds a `passes` vector [ "forward", "forward_debug", ... ] and a `stages` vector (each stage = a
// SPIR-V byte blob). For COLOUR rendering libshell uses the **forward** pass; "forward_debug" is the
// in-engine texture-visualiser variant (its frag is the LARGEST, which a "pick largest" heuristic wrongly
// grabbed -> wrong/dark output). Stages are laid out 2-per-pass in pass order (vert, frag), so the
// forward pass's modules are stages[2*fwdIdx] (vert) + stages[2*fwdIdx+1] (frag). Extract ONLY those.
inline bool parseRendShadForward(const std::vector<u8>& data, std::vector<SpirvBlob>& out, bool* outTransparent=nullptr) {
    const u8* d = data.data(); const size_t N = data.size();
    auto rd16 = [&](size_t o)->u16{ return (o+2<=N)? *reinterpret_cast<const u16*>(d+o):0; };
    auto rdi32= [&](size_t o)->i32{ return (o+4<=N)? *reinterpret_cast<const i32*>(d+o):0; };
    auto rd32 = [&](size_t o)->u32{ return (o+4<=N)? *reinterpret_cast<const u32*>(d+o):0; };
    auto vtField = [&](size_t tbl, u32 fi)->size_t{
        if (tbl+4>N) return 0; i32 so=rdi32(tbl); long long vt=(long long)tbl-so;
        if (vt<0||(size_t)vt+4>N) return 0; u16 vs=rd16((size_t)vt); size_t sl=(size_t)vt+4+fi*2;
        if (sl+2>(size_t)vt+vs||sl+2>N) return 0; u16 fo=rd16(sl); return fo? tbl+fo:0; };
    auto vtNFields = [&](size_t tbl)->u32{
        if (tbl+4>N) return 0; i32 so=rdi32(tbl); long long vt=(long long)tbl-so;
        if (vt<0||(size_t)vt+4>N) return 0; u16 vs=rd16((size_t)vt); return vs>=4?(vs-4)/2:0; };
    auto strAt = [&](size_t p)->std::string{
        if (!p||p+4>N) return ""; size_t s=p+rd32(p); u32 ln=rd32(s);
        if (ln==0||ln>256||s+4+ln>N) return ""; return std::string((const char*)(d+s+4), ln); };
    size_t root = rd32(0); if (root+4>N) return false;
    u32 nRoot = vtNFields(root);
    // Find the passes vector (vector-of-tables whose element[0] has a string field "forward"...) and the
    // stages vector (vector-of-tables whose element has a >500-byte byte-vector = SPIR-V).
    size_t passesBase=0; u32 nPasses=0; int fwdIdx=-1;
    size_t stagesBase=0; u32 nStages=0;
    for (u32 fi=0; fi<nRoot; ++fi) {
        size_t fp=vtField(root,fi); if(!fp) continue; u32 uoff=rd32(fp); if(!uoff) continue;
        size_t vec=fp+uoff; if(vec+4>N) continue; u32 cnt=rd32(vec); if(cnt==0||cnt>64) continue;
        size_t base=vec+4; if(base+(size_t)cnt*4>N) continue;
        // element 0
        size_t e0=base+rd32(base);
        // passes? element has a string field naming a pass
        for (u32 ef=0; ef<vtNFields(e0) && ef<4; ++ef) {
            std::string s=strAt(vtField(e0,ef));
            if (s=="forward"||s=="forward_debug"||s.rfind("forward",0)==0) {
                passesBase=base; nPasses=cnt;
                // Prefer "forwardSkinned" when present — SKINNED surfaces carry forwardSkinned/forward/_debug
                // passes and skinned meshes draw forwardSkinned; loading plain "forward" gave the preview a
                // frag the mesh never runs (and missed shadergen's edits, which now land on forwardSkinned).
                int fwdPlain=-1, fwdSkin=-1;
                for (u32 pi=0; pi<cnt; ++pi){ size_t pe=base+pi*4; size_t pt=pe+rd32(pe);
                    for (u32 pf=0; pf<vtNFields(pt)&&pf<4; ++pf){ std::string pn=strAt(vtField(pt,pf));
                        if (pn=="forward"){ fwdPlain=(int)pi; break; }
                        else if (pn=="forwardSkinned"){ fwdSkin=(int)pi; break; } } }
                fwdIdx = (fwdSkin>=0) ? fwdSkin : fwdPlain;
                if (std::getenv("HSR_SHADDBG"))
                    fprintf(stderr, "    [PASSPICK] nPasses=%u fwdPlain=%d fwdSkin=%d -> fwdIdx=%d\n", cnt, fwdPlain, fwdSkin, fwdIdx);
                break;
            }
        }
        // stages? element has a large byte vector
        for (u32 ef=0; ef<vtNFields(e0) && ef<4; ++ef) {
            size_t sp=vtField(e0,ef); if(!sp) continue; u32 so=rd32(sp); size_t sv=sp+so;
            if (sv+4<=N){ u32 bc=rd32(sv); if (bc>500 && sv+4+bc<=N && bc<2000000){ stagesBase=base; nStages=cnt; break; } }
        }
    }
    if (fwdIdx<0 || nStages==0 || nPasses==0 || nStages != 2*nPasses) return false;
    auto stageSpirv = [&](u32 si, SpirvBlob& blob)->bool{
        if (si>=nStages) return false; size_t se=stagesBase+si*4; size_t st=se+rd32(se);
        // Pick the candidate whose data sits LATEST in the file (largest offset): shadergen's edited
        // modules (cutout/tint/flipbook) are APPENDED at EOF with the slot repointed — taking the FIRST
        // spirv-looking field returned the ORIGINAL un-edited frag, so the preview rendered cooked
        // CUTOUT foliage as SOLID QUADS ("flat / transparency incorrectly added") while the device
        // (which follows the repointed slot) discarded correctly. Preview must read the same module.
        size_t bestStart=0; u32 bestBc=0;
        for (u32 ef=0; ef<vtNFields(st)&&ef<6; ++ef){ size_t sp=vtField(st,ef); if(!sp) continue;
            u32 so=rd32(sp); size_t sv=sp+so; if(sv+4>N) continue; u32 bc=rd32(sv);
            if (bc>500 && sv+4+bc<=N && (bc%4)==0){ size_t start=sv+4;
                if (rd32(start)!=0x07230203u) continue;            // SPIR-V magic
                if (start > bestStart){ bestStart=start; bestBc=bc; } }
        }
        if (!bestStart) return false;
        blob.code.resize(bestBc/4); memcpy(blob.code.data(), d+bestStart, bestBc);
        blob.stageType=0xFFFFFFFFu;
        for (size_t wi=5; wi+1<blob.code.size() && wi<60;){ u32 w=blob.code[wi],op=w&0xFFFF,wc=w>>16;
            if(wc==0)break; if(op==15){blob.stageType=blob.code[wi+1];break;} wi+=wc; }
        if (std::getenv("HSR_SHADDBG")) {
            int kills=0; for (size_t wi=5; wi+1<blob.code.size();){ u32 w=blob.code[wi],op=w&0xFFFF,wc=w>>16; if(!wc)break; if(op==252)kills++; wi+=wc; }
            fprintf(stderr, "    [STAGEPICK] si=%u data@%zu len=%u exec=%u OpKill=%d\n", si, bestStart, bestBc, blob.stageType, kills);
        }
        return true;
    };
    // FAITHFUL transparent-pass detection (data-proven on calming, NOT a name heuristic): in the
    // RENDSHAD forward pass table, field 4 is PRESENT (=0xFFFFFFFF) for every OPAQUE forward shader
    // (isotropictiled*, unlit*, lightmap, rgbmasked, billboard, vegetation…) but OMITTED (default) for
    // the special blended-pass shaders — animatedfog + mattepaintingalpha (the "white fog"/"broken matte").
    // So f4-omitted = this surface's forward pass is alpha-blended. (Other transparents — wateroverlay/
    // waterflipbook/unlitalpha — share the OPAQUE pass-state; their blend comes from the MATL render-queue,
    // still handled by the name rules in scene_loader. This only ADDS blend, never removes it.)
    {
        size_t pe2 = passesBase + (size_t)fwdIdx*4; size_t pt2 = pe2 + rd32(pe2);
        if (outTransparent) *outTransparent = (vtField(pt2,4) == 0);
    }
    if (std::getenv("HSR_SHADDBG")) {   // dump the forward pass table fields (find the blend/pipeline-state field)
        size_t pe2 = passesBase + (size_t)fwdIdx*4; size_t pt2 = pe2 + rd32(pe2);
        u32 nf = vtNFields(pt2);
        fprintf(stderr, "    [SHADDBG] fwd pass tbl @%zu nFields=%u:", pt2, nf);
        for (u32 ff=0; ff<nf; ++ff) {
            size_t fp = vtField(pt2, ff);
            if (!fp) { fprintf(stderr, " f%u=-", ff); continue; }
            std::string s = strAt(fp); u32 vv = rd32(fp);
            if (!s.empty()) fprintf(stderr, " f%u='%s'", ff, s.c_str());
            else {
                // maybe a sub-table (offset to a vtable within bounds): dump its scalar fields
                size_t sub = fp + vv; u32 snf = (vv && sub<N) ? vtNFields(sub) : 0;
                if (snf>0 && snf<=16) { fprintf(stderr, " f%u={", ff);
                    for (u32 sf=0; sf<snf; ++sf){ size_t sfp=vtField(sub,sf); fprintf(stderr,"%s%u", sf?",":"", sfp?rd32(sfp):0u); }
                    fprintf(stderr, "}"); }
                else fprintf(stderr, " f%u=%u", ff, vv);
            }
        }
        fprintf(stderr, "\n");
    }
    SpirvBlob v,f;
    if (!stageSpirv((u32)(2*fwdIdx), v) || !stageSpirv((u32)(2*fwdIdx+1), f)) return false;
    // order may be (vert,frag) or (frag,vert) — keep both, tagged by their own exec model
    out.push_back(std::move(v)); out.push_back(std::move(f));
    return out.size()==2;
}

inline bool parseRendShad(const std::vector<u8>& data, std::vector<SpirvBlob>& out) {
    if (data.size() < 48) return false;
    if (memcmp(data.data() + 4, "SHAD", 4) != 0) return false;

    // FAITHFUL: select the `forward` (colour) pass variant from the RENDSHAD passes table.
    if (!std::getenv("HSR_ALLVARIANTS") && parseRendShadForward(data, out)) return true;
    out.clear();

    // Fallback: scan for all SPIR-V modules (handles multi-variant surface shaders).
    if (scanSpirvModules(data, out)) return true;

    u32 rootOff = *reinterpret_cast<const u32*>(data.data());

    auto fieldFo = [&](u32 tablePos, u32 fi) -> u16 {
        i32 soff = *reinterpret_cast<const i32*>(data.data() + tablePos);
        u32 vt = tablePos - soff;
        if (vt + 4 > data.size()) return 0;
        u16 vtSize = *reinterpret_cast<const u16*>(data.data() + vt);
        u32 idx = vt + 4 + fi * 2;
        if (idx + 2 > vt + vtSize) return 0;
        return *reinterpret_cast<const u16*>(data.data() + idx);
    };

    auto followVec = [&](u32 tablePos, u32 fi, u32& baseOut, u32& countOut) -> bool {
        u16 fo = fieldFo(tablePos, fi);
        if (!fo) return false;
        u32 fieldSlot = tablePos + fo;
        if (fieldSlot + 4 > data.size()) return false;
        u32 uoff = *reinterpret_cast<const u32*>(data.data() + fieldSlot);
        if (!uoff) return false;
        u32 vecPos = fieldSlot + uoff;
        if (vecPos + 4 > data.size()) return false;
        countOut = *reinterpret_cast<const u32*>(data.data() + vecPos);
        baseOut = vecPos + 4;
        return true;
    };

    auto followElem = [&](u32 elemBase, u32 i, u32& tableOut) -> bool {
        u32 slot = elemBase + i * 4;
        if (slot + 4 > data.size()) return false;
        u32 uoff = *reinterpret_cast<const u32*>(data.data() + slot);
        if (!uoff) return false;
        tableOut = slot + uoff;
        return true;
    };

    auto followByteVec = [&](u32 tablePos, u32 fi, u32& startOut, u32& countOut) -> bool {
        u16 fo = fieldFo(tablePos, fi);
        if (!fo) return false;
        u32 fieldSlot = tablePos + fo;
        if (fieldSlot + 4 > data.size()) return false;
        u32 uoff = *reinterpret_cast<const u32*>(data.data() + fieldSlot);
        if (!uoff) return false;
        u32 vecPos = fieldSlot + uoff;
        if (vecPos + 4 > data.size()) return false;
        countOut = *reinterpret_cast<const u32*>(data.data() + vecPos);
        startOut = vecPos + 4;
        return true;
    };

    // Debug: dump root vtable
    i32 rootSoff = *reinterpret_cast<const i32*>(data.data() + rootOff);
    u32 rootVt = rootOff - rootSoff;
    u16 rootVtSize = *reinterpret_cast<const u16*>(data.data() + rootVt);
    u32 nRootFields = (rootVtSize - 4) / 2;

    // Try each field that might contain shader stages
    // Based on libshell decomp: shader stages are in a vector field
    for (u32 fi = 0; fi < nRootFields && out.empty(); ++fi) {
        u32 base, count;
        if (!followVec(rootOff, fi, base, count) || count == 0) continue;
        if (count > 16) continue;

        for (u32 i = 0; i < count; ++i) {
            u32 elemTable;
            if (!followElem(base, i, elemTable)) continue;

            // Element vtable analysis
            i32 elemSoff = *reinterpret_cast<const i32*>(data.data() + elemTable);
            u32 elemVt = elemTable - elemSoff;
            u16 elemVtSize = *reinterpret_cast<const u16*>(data.data() + elemVt);
            u32 nElemFields = (elemVtSize - 4) / 2;

            // Try to find stage type and SPIRV data
            u32 stageType = 0;

            // Read stage type from field[2] (common for shader stages)
            u16 stFo = fieldFo(elemTable, 2);
            if (stFo) {
                stageType = *reinterpret_cast<const u32*>(data.data() + elemTable + stFo);
            }

            // Find SPIRV byte vector — try multiple fields
            u32 spirvStart = 0, spirvCount = 0;

            // For shader stages in libshell, the SPIRV is typically in fields 4, 5, or 3
            // Try byte vectors first (uoffset to byte blob)
            for (u32 bf = 0; bf < nElemFields && spirvCount == 0; ++bf) {
                u16 bfo = fieldFo(elemTable, bf);
                if (!bfo) continue;
                u32 fieldSlot = elemTable + bfo;
                u32 uoff = *reinterpret_cast<const u32*>(data.data() + fieldSlot);
                if (!uoff || uoff > data.size()) continue;
                u32 vecPos = fieldSlot + uoff;
                if (vecPos + 4 > data.size()) continue;
                u32 cnt = *reinterpret_cast<const u32*>(data.data() + vecPos);
                // Heuristic: SPIRV byte vector is typically > 500 bytes
                if (cnt > 500 && cnt < 1048576 && vecPos + 4 + cnt <= data.size()) {
                    spirvStart = vecPos + 4;
                    spirvCount = cnt;
                }
            }

            if (spirvCount >= 500) {
                SpirvBlob blob;
                blob.stageType = stageType;
                u32 wordCount = spirvCount / 4;
                blob.code.resize(wordCount);
                memcpy(blob.code.data(), data.data() + spirvStart, spirvCount);
                out.push_back(std::move(blob));
            }
        }
    }

    // Fallback: direct SPIRV magic scanning (used when FlatBuffer structure can't be parsed)
    if (out.empty()) {
        const u8 magic[] = {0x03, 0x02, 0x23, 0x07};
        std::vector<size_t> positions;
        for (size_t pos = 0; pos + 4 <= data.size(); ++pos) {
            if (memcmp(data.data() + pos, magic, 4) == 0) {
                // Verify SPIRV header
                if (pos + 20 <= data.size()) {
                    u32 version = *reinterpret_cast<const u32*>(data.data() + pos + 4);
                    u32 bound   = *reinterpret_cast<const u32*>(data.data() + pos + 12);
                    if (version == 0x00010000 && bound > 0 && bound < 100000) {
                        positions.push_back(pos);
                    }
                }
            }
        }

        for (size_t pi = 0; pi < positions.size(); ++pi) {
            size_t pos = positions[pi];
            size_t endPos = (pi + 1 < positions.size()) ? positions[pi+1] : data.size();
            u32 byteSize = (u32)(endPos - pos);

            if (byteSize >= 500 && byteSize < 1048576) {
                SpirvBlob blob;
                // Determine stage from SPIRV header before extracting
                // OpEntryPoint is typically at word 5-15
                u32 wordCount = byteSize / 4;
                blob.code.resize(wordCount);
                memcpy(blob.code.data(), data.data() + pos, byteSize);

                // Find execution model from SPIRV header
                for (size_t wi = 5; wi < wordCount && wi < 30; ) {
                    u32 word = blob.code[wi];
                    u32 op = word & 0xFFFF;
                    u32 wc = word >> 16;
                    if (wc == 0) break;
                    if (op == 15) { // OpEntryPoint
                        if (wi + 1 < wordCount) {
                            blob.stageType = blob.code[wi + 1];
                        }
                        break;
                    }
                    wi += wc;
                }

                out.push_back(std::move(blob));
            }
        }
    }

    return !out.empty();
}
