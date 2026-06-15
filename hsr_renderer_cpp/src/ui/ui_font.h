// ── ui_font.h — TTF glyph-atlas baking for the custom Blender-style editor UI ───────────────────────────────
// Loads a TTF (bundled Inter, falling back to a Windows system font so it ALWAYS finds one) and bakes an R8
// coverage atlas with stb_truetype's rect packer. ui_draw.h uploads `pixels` as a Vulkan R8 texture and samples
// it for text (white·coverage·tint). Mirrors how stb_image / stb_vorbis are vendored (impl in a separate TU).
#pragma once
#include "stb_truetype.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>

namespace ui {

struct Font {
    int   atlasW = 0, atlasH = 0;
    std::vector<uint8_t> pixels;          // R8 coverage atlas (atlasW*atlasH)
    stbtt_packedchar glyphs[224];         // codepoints 32..255 (ASCII + Latin-1 supplement)
    float pixelHeight = 0.f;
    float ascent = 0.f, descent = 0.f, lineGap = 0.f, lineHeight = 0.f;
    bool  ok = false;

    bool loadBytes(const std::vector<uint8_t>& ttf, float px, int aw = 1024, int ah = 1024) {
        if (ttf.size() < 4) return false;
        pixelHeight = px; atlasW = aw; atlasH = ah;
        pixels.assign((size_t)aw * ah, 0);
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, pixels.data(), aw, ah, 0, 1, nullptr)) return false;
        stbtt_PackSetOversampling(&pc, 2, 2);   // crisp at the base UI size
        int r = stbtt_PackFontRange(&pc, ttf.data(), 0, px, 32, 224, glyphs);
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
        if (cp < 32 || cp > 255) cp = '?';
        return glyphs[cp - 32].xadvance;
    }
    float textWidth(const char* s, int n = -1) const {
        if (!ok || !s) return 0.f;
        float w = 0.f; int i = 0;
        for (; s[i] && (n < 0 || i < n); ++i) w += advance((unsigned char)s[i]);
        return w;
    }
    // Fill an aligned quad for codepoint cp, advancing the pen (x,y). Returns false for whitespace-only no-draw.
    bool quad(unsigned cp, float* x, float* y, stbtt_aligned_quad* q) const {
        if (cp < 32 || cp > 255) cp = '?';
        stbtt_GetPackedQuad(glyphs, atlasW, atlasH, (int)cp - 32, x, y, q, 1);
        return true;
    }
};

inline bool readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* fp = fopen(path, "rb"); if (!fp) return false;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return false; }
    out.resize((size_t)n); size_t r = fread(out.data(), 1, (size_t)n, fp); fclose(fp);
    out.resize(r); return r > 0;
}

// Load the UI font at `px`, trying the bundled TTFs under several path prefixes (cwd may be the source tree or
// build/), then guaranteed Windows system fonts. `mono` picks Consolas (numeric fields) over Inter.
inline bool loadUIFont(Font& f, float px, bool mono = false) {
    static const char* sans[] = {
        "third_party/fonts/InterVariable.ttf", "../third_party/fonts/InterVariable.ttf",
        "hsr_renderer_cpp/third_party/fonts/InterVariable.ttf",
        "third_party/fonts/SegoeUI.ttf", "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf" };
    static const char* monos[] = {
        "third_party/fonts/Consola.ttf", "../third_party/fonts/Consola.ttf",
        "hsr_renderer_cpp/third_party/fonts/Consola.ttf",
        "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf" };
    const char* const* list = mono ? monos : sans;
    int count = mono ? (int)(sizeof(monos)/sizeof(monos[0])) : (int)(sizeof(sans)/sizeof(sans[0]));
    std::vector<uint8_t> ttf;
    for (int i = 0; i < count; ++i) {
        if (readFile(list[i], ttf) && f.loadBytes(ttf, px)) return true;
    }
    return false;
}

} // namespace ui
