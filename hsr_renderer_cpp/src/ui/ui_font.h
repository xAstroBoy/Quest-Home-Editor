// ── ui_font.h — TTF glyph-atlas baking for the custom Blender-style editor UI ───────────────────────────────
// Loads a TTF (bundled Inter, falling back to a Windows system font so it ALWAYS finds one) and bakes an R8
// coverage atlas with stb_truetype's rect packer. ui_draw.h uploads `pixels` as a Vulkan R8 texture and samples
// it for text (white·coverage·tint). Mirrors how stb_image / stb_vorbis are vendored (impl in a separate TU).
#pragma once
#include "stb_truetype.h"
#include "core/config.h"   // AppConfig::s_exeDir — exe-relative bundled-font probing (cwd-independent)
#include "ui/i18n.h"       // i18n::utf8Next — the UI is UTF-8 (CJK translations, GitHub #10)
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <cstdio>

namespace ui {

struct Font {
    int   atlasW = 0, atlasH = 0;
    std::vector<uint8_t> pixels;          // R8 coverage atlas (atlasW*atlasH)
    stbtt_packedchar glyphs[224];         // codepoints 32..255 (ASCII + Latin-1 supplement) — fast path
    std::unordered_map<unsigned, stbtt_packedchar> ext;   // extra codepoints (CJK etc.) baked from a fallback font
    float pixelHeight = 0.f;
    float ascent = 0.f, descent = 0.f, lineGap = 0.f, lineHeight = 0.f;
    bool  ok = false;

    // extraCps = the exact non-ASCII codepoints the active languages need. Each is baked from the FIRST font in
    // [main, fallbacks...] that actually has a glyph for it — so Latin-Extended accents / Cyrillic come from the
    // main font, CJK Han from YaHei, Kana from a JP font, Hangul from Malgun, Arabic from Segoe/Arial, etc.
    // Empty -> ASCII-only (English), the original 1024² atlas.
    bool loadBytes(const std::vector<uint8_t>& ttf, float px, int aw = 1024, int ah = 1024,
                   const std::vector<unsigned>* extraCps = nullptr,
                   const std::vector<std::vector<uint8_t>>* fallbacks = nullptr) {
        if (ttf.size() < 4) return false;
        ext.clear();
        bool wantExt = extraCps && !extraCps->empty();
        if (wantExt) { aw = 2048; ah = 2048; }   // room for all script glyphs alongside ASCII
        pixelHeight = px; atlasW = aw; atlasH = ah;
        pixels.assign((size_t)aw * ah, 0);
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, pixels.data(), aw, ah, 0, 1, nullptr)) return false;
        stbtt_PackSetOversampling(&pc, 2, 2);   // crisp Latin at the base UI size
        int r = stbtt_PackFontRange(&pc, ttf.data(), 0, px, 32, 224, glyphs);
        if (wantExt) {
            // font list: main first, then each fallback. Init a fontinfo per font for glyph-coverage queries.
            std::vector<const std::vector<uint8_t>*> fonts; fonts.push_back(&ttf);
            if (fallbacks) for (auto& f : *fallbacks) if (f.size() > 4) fonts.push_back(&f);
            std::vector<stbtt_fontinfo> fi(fonts.size()); std::vector<char> fiok(fonts.size(), 0);
            for (size_t i = 0; i < fonts.size(); ++i)
                fiok[i] = stbtt_InitFont(&fi[i], fonts[i]->data(), stbtt_GetFontOffsetForIndex(fonts[i]->data(), 0)) ? 1 : 0;
            // assign each codepoint to the first font that has a glyph for it
            std::vector<std::vector<int>> groups(fonts.size());
            for (unsigned cp : *extraCps)
                for (size_t i = 0; i < fonts.size(); ++i)
                    if (fiok[i] && stbtt_FindGlyphIndex(&fi[i], (int)cp)) { groups[i].push_back((int)cp); break; }
            stbtt_PackSetOversampling(&pc, 1, 1);   // dense scripts — 1x keeps the atlas small
            for (size_t i = 0; i < fonts.size(); ++i) {
                if (groups[i].empty()) continue;
                std::vector<stbtt_packedchar> pk(groups[i].size());
                stbtt_pack_range rng{};
                rng.font_size = px; rng.first_unicode_codepoint_in_range = 0;
                rng.array_of_unicode_codepoints = groups[i].data(); rng.num_chars = (int)groups[i].size(); rng.chardata_for_range = pk.data();
                if (stbtt_PackFontRanges(&pc, fonts[i]->data(), 0, &rng, 1))
                    for (size_t k = 0; k < groups[i].size(); ++k) ext[(unsigned)groups[i][k]] = pk[k];
            }
        }
        stbtt_PackEnd(&pc);
        if (!r) return false;
        stbtt_fontinfo fi;
        if (stbtt_InitFont(&fi, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
            int a, d, l; stbtt_GetFontVMetrics(&fi, &a, &d, &l);
            float sc = stbtt_ScaleForPixelHeight(&fi, px);
            ascent = a * sc; descent = d * sc; lineGap = l * sc;
            lineHeight = (a - d + l) * sc;
        } else { ascent = px * 0.8f; descent = -px * 0.2f; lineHeight = px * 1.25f; }
        ok = true; return true;
    }

    // Advance for one codepoint (kerning ignored — packed atlas). Returns the x-advance in pixels.
    float advance(unsigned cp) const {
        if (cp >= 32 && cp <= 255) return glyphs[cp - 32].xadvance;
        auto it = ext.find(cp); if (it != ext.end()) return it->second.xadvance;
        return glyphs['?' - 32].xadvance;
    }
    // n = BYTE length (-1 = to NUL). UTF-8 aware.
    float textWidth(const char* s, int n = -1) const {
        if (!ok || !s) return 0.f;
        float w = 0.f; const char* p = s; const char* end = (n < 0) ? nullptr : s + n;
        while (*p && (!end || p < end)) { unsigned cp = i18n::utf8Next(p); if (cp == '\t') { w += advance(' ') * 4; continue; } w += advance(cp); }
        return w;
    }
    // Fill an aligned quad for codepoint cp, advancing the pen (x,y). CJK from ext, else '?' fallback.
    bool quad(unsigned cp, float* x, float* y, stbtt_aligned_quad* q) const {
        if (cp >= 32 && cp <= 255) { stbtt_GetPackedQuad(glyphs, atlasW, atlasH, (int)cp - 32, x, y, q, 1); return true; }
        auto it = ext.find(cp);
        if (it != ext.end()) { stbtt_packedchar one[1] = { it->second }; stbtt_GetPackedQuad(one, atlasW, atlasH, 0, x, y, q, 1); return true; }
        stbtt_GetPackedQuad(glyphs, atlasW, atlasH, (int)'?' - 32, x, y, q, 1); return true;
    }
};

inline bool readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* fp = fopen(path, "rb"); if (!fp) return false;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return false; }
    out.resize((size_t)n); size_t r = fread(out.data(), 1, (size_t)n, fp); fclose(fp);
    out.resize(r); return r > 0;
}

// Load the UI font at `px`, trying the bundled TTFs under several path prefixes (cwd may be the source tree,
// build/, or ANYWHERE — so EXE-RELATIVE paths are probed too), then per-OS system fonts. On macOS the old list
// had ONLY Windows system fallbacks: launched outside the repo no font loaded, the glyph atlas stayed empty and
// the editor UI rendered NOTHING while the 3D env drew fine (GitHub issue #2 "menus dont show up on mac os").
// `mono` picks Consolas (numeric fields) over Inter.
// Resolve the first readable path from a list into `out`. Returns true on success.
inline bool firstReadable(const char* const* list, int count, std::vector<uint8_t>& out) {
    for (int i = 0; i < count; ++i) if (readFile(list[i], out)) return true;
    return false;
}
// System fonts covering the non-Latin scripts the translations use (Inter/Segoe have Latin+Cyrillic but no CJK
// / limited Arabic). Loaded as a fallback CHAIN; loadBytes picks per-codepoint the first font that has the glyph.
// Order = broad coverage first (Segoe/Arial: Cyrillic/Greek/Arabic), then Korean, Japanese, Chinese.
inline void loadFallbackFonts(std::vector<std::vector<uint8_t>>& out) {
    static const char* fonts[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/segoeui.ttf",   // Latin-ext, Cyrillic, Greek, Arabic (broad)
        "C:/Windows/Fonts/arial.ttf",     // Arabic + broad fallback
        "C:/Windows/Fonts/malgun.ttf",    // Korean (Hangul)
        "C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc",   // Japanese (Kana)
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc",   // Chinese (Han)
#elif defined(__APPLE__)
        "/System/Library/Fonts/SFArabic.ttf", "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",   // Korean
        "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/PingFang.ttc",   // JP/CN
#else
        "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",   // covers CN/JP/KR in one
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
#endif
    };
    // exe-relative bundled fallbacks first (packaged builds may ship a Noto)
    if (!AppConfig::s_exeDir.empty()) {
        const char* rel[] = { "fonts/NotoSansCJK.ttc", "fonts/NotoSans.ttf", "third_party/fonts/NotoSansCJK.ttc" };
        for (const char* r : rel) { std::vector<uint8_t> b; std::string p = AppConfig::s_exeDir + "/" + r; if (readFile(p.c_str(), b)) out.push_back(std::move(b)); }
    }
    for (const char* p : fonts) { std::vector<uint8_t> b; if (readFile(p, b)) out.push_back(std::move(b)); }
}

inline bool loadUIFont(Font& f, float px, bool mono = false, const std::vector<unsigned>* extraCps = nullptr) {
    static const char* sans[] = {
        "third_party/fonts/InterVariable.ttf", "../third_party/fonts/InterVariable.ttf",
        "hsr_renderer_cpp/third_party/fonts/InterVariable.ttf",
        "third_party/fonts/SegoeUI.ttf",
#ifdef _WIN32
        "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/SFNS.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    };
    static const char* monos[] = {
        "third_party/fonts/Consola.ttf", "../third_party/fonts/Consola.ttf",
        "hsr_renderer_cpp/third_party/fonts/Consola.ttf",
#ifdef _WIN32
        "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Monaco.ttf", "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Supplemental/Courier New.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
#endif
    };
    const char* const* list = mono ? monos : sans;
    int count = mono ? (int)(sizeof(monos)/sizeof(monos[0])) : (int)(sizeof(sans)/sizeof(sans[0]));
    // Resolve the main (Latin) TTF bytes first — EXE-RELATIVE, then system fallbacks. A non-EN UI also needs a
    // CJK font baked into the SAME atlas, so we can't loadBytes-and-return at the first probe like before; capture
    // the bytes, then do ONE loadBytes with the extra codepoints + CJK font.
    std::vector<uint8_t> ttf;
    if (!AppConfig::s_exeDir.empty()) {
        const char* rel[] = { "fonts/", "third_party/fonts/", "../third_party/fonts/", "../Resources/fonts/" };
        const char* fn = mono ? "Consola.ttf" : "InterVariable.ttf";
        for (const char* r : rel) { std::string p = AppConfig::s_exeDir + "/" + r + fn; if (readFile(p.c_str(), ttf)) break; }
    }
    if (ttf.empty()) firstReadable(list, count, ttf);
    if (ttf.empty()) {
        fprintf(stderr, "[UI] FATAL-ish: no UI font found (bundled fonts missing AND no system fallback) — the editor UI cannot draw text\n");
        return false;
    }
    std::vector<std::vector<uint8_t>> fallbacks;
    if (extraCps && !extraCps->empty()) {
        loadFallbackFonts(fallbacks);
        if (fallbacks.empty()) fprintf(stderr, "[UI] no script-fallback fonts found — some non-Latin glyphs may show '?'\n");
    }
    return f.loadBytes(ttf, px, 1024, 1024, extraCps, fallbacks.empty() ? nullptr : &fallbacks);
}

} // namespace ui
