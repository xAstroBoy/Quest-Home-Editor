// ── ui_font.h — TTF glyph-atlas baking for the custom Blender-style editor UI ───────────────────────────────
// Loads a TTF (bundled Inter, falling back to a Windows system font so it ALWAYS finds one) and bakes an R8
// coverage atlas with stb_truetype's rect packer. ui_draw.h uploads `pixels` as a Vulkan R8 texture and samples
// it for text (white·coverage·tint). Mirrors how stb_image / stb_vorbis are vendored (impl in a separate TU).
#pragma once
#include "stb_truetype.h"
#include "core/config.h"   // AppConfig::s_exeDir — exe-relative bundled-font probing (cwd-independent)
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

// Load the UI font at `px`, trying the bundled TTFs under several path prefixes (cwd may be the source tree,
// build/, or ANYWHERE — so EXE-RELATIVE paths are probed too), then per-OS system fonts. On macOS the old list
// had ONLY Windows system fallbacks: launched outside the repo no font loaded, the glyph atlas stayed empty and
// the editor UI rendered NOTHING while the 3D env drew fine (GitHub issue #2 "menus dont show up on mac os").
// `mono` picks Consolas (numeric fields) over Inter.
inline bool loadUIFont(Font& f, float px, bool mono = false) {
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
    std::vector<uint8_t> ttf;
    // EXE-RELATIVE first: works no matter what the cwd is (Finder launch, AppImage, PATH). The packaged
    // builds ship the fonts beside the binary (fonts/ or third_party/fonts/) and macOS in Resources/.
    if (!AppConfig::s_exeDir.empty()) {
        const char* rel[] = { "fonts/", "third_party/fonts/", "../third_party/fonts/", "../Resources/fonts/" };
        const char* fn = mono ? "Consola.ttf" : "InterVariable.ttf";
        for (const char* r : rel) {
            std::string p = AppConfig::s_exeDir + "/" + r + fn;
            if (readFile(p.c_str(), ttf) && f.loadBytes(ttf, px)) return true;
        }
    }
    for (int i = 0; i < count; ++i) {
        if (readFile(list[i], ttf) && f.loadBytes(ttf, px)) return true;
    }
    fprintf(stderr, "[UI] FATAL-ish: no UI font found (bundled fonts missing AND no system fallback) — the editor UI cannot draw text\n");
    return false;
}

} // namespace ui
