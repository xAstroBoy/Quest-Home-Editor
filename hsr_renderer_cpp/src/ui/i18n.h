// ── i18n.h — runtime UI localization (GitHub issue #10, multilingual) ───────────────────────────────────────
// The custom editor UI funnels every visible string through ui::Context::textAligned / checkbox / tip, so a
// single tr() there translates the WHOLE UI with no per-call-site edits: the ENGLISH source string is the key.
// A string with no table entry falls through unchanged (English), so partial translations degrade gracefully.
// Language is picked in the top bar + persisted in the config. Chinese (Simplified) ships first (the issue's
// requester is a zh user); add a column to LANGS + a translation array to add more.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace i18n {

enum Lang { EN = 0, ZH = 1, LANG_COUNT };
inline int g_lang = EN;
inline const char* langLabel(int l) { return l == ZH ? "\xE4\xB8\xAD\xE6\x96\x87" : "EN"; }   // "中文" / "EN"
inline const char* langMenuName(int l) { return l == ZH ? "\xE4\xB8\xAD\xE6\x96\x87 (Chinese)" : "English"; }

// Decode one UTF-8 codepoint, advancing p past it. Malformed bytes pass through as Latin-1.
inline unsigned utf8Next(const char*& p) {
    unsigned c = (unsigned char)*p++;
    if (c < 0x80) return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    unsigned cp = c & (0x7F >> (n + 1));
    while (n-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    return cp;
}

// EN source -> per-language translations. Only entries a language actually has are used; missing = English.
struct Entry { const char* en; const char* zh; };
inline const Entry* table(size_t& n) {
    static const Entry T[] = {
        // ── top bar / title / global actions ──
        { "File", "\xE6\x96\x87\xE4\xBB\xB6" },                              // 文件
        { "Edit", "\xE7\xBC\x96\xE8\xBE\x91" },                              // 编辑
        { "Object", "\xE5\xAF\xB9\xE8\xB1\xA1" },                            // 对象
        { "View", "\xE8\xA7\x86\xE5\x9B\xBE" },                              // 视图
        { "Save", "\xE4\xBF\x9D\xE5\xAD\x98" },                              // 保存
        { "Load", "\xE5\x8A\xA0\xE8\xBD\xBD" },                              // 加载
        { "Open", "\xE6\x89\x93\xE5\xBC\x80" },                              // 打开
        { "Cook APK", "\xE7\x83\xAD\xE7\x83\xA4 APK" },                       // 烘焙 APK
        { "Blender ->", "\xE5\xAF\xBC\xE5\x87\xBA Blender" },                 // 导出 Blender
        { "-> Blender", "\xE5\xAF\xBC\xE5\x85\xA5 Blender" },                 // 导入 Blender
        { "Auto-save: On", "\xE8\x87\xAA\xE5\x8A\xA8\xE4\xBF\x9D\xE5\xAD\x98: \xE5\xBC\x80" },   // 自动保存: 开
        { "Auto-save: Off", "\xE8\x87\xAA\xE5\x8A\xA8\xE4\xBF\x9D\xE5\xAD\x98: \xE5\x85\xB3" },  // 自动保存: 关
        // ── viewport toolbar ──
        { "Viewport", "\xE8\xA7\x86\xE5\x8F\xA3" },                          // 视口
        { "Move", "\xE7\xA7\xBB\xE5\x8A\xA8" },                              // 移动
        { "Rotate", "\xE6\x97\x8B\xE8\xBD\xAC" },                            // 旋转
        { "Scale", "\xE7\xBC\xA9\xE6\x94\xBE" },                             // 缩放
        { "Local", "\xE6\x9C\xAC\xE5\x9C\xB0" },                             // 本地
        { "Global", "\xE5\x85\xA8\xE5\xB1\x80" },                            // 全局
        { "Spd", "\xE9\x80\x9F\xE5\xBA\xA6" },                               // 速度
        { "X-ray", "\xE9\x80\x8F\xE8\xA7\x86" },                             // 透视
        { "Audio: On", "\xE9\x9F\xB3\xE9\xA2\x91: \xE5\xBC\x80" },            // 音频: 开
        { "Audio: Off", "\xE9\x9F\xB3\xE9\xA2\x91: \xE5\x85\xB3" },           // 音频: 关
        { "Pin: On", "\xE5\x9B\xBA\xE5\xAE\x9A: \xE5\xBC\x80" },              // 固定: 开
        { "Pin: Off", "\xE5\x9B\xBA\xE5\xAE\x9A: \xE5\x85\xB3" },             // 固定: 关
        // ── outliner / right panel ──
        { "Outliner", "\xE5\xA4\xA7\xE7\xBA\xB2" },                          // 大纲
        { "+ Add", "+ \xE6\xB7\xBB\xE5\x8A\xA0" },                            // + 添加
        { "Search", "\xE6\x90\x9C\xE7\xB4\xA2" },                            // 搜索
        { "MESHES", "\xE7\xBD\x91\xE6\xA0\xBC" },                            // 网格
        { "(no selection)", "(\xE6\x9C\xAA\xE9\x80\x89\xE6\x8B\xA9)" },        // (未选择)
        // ── tabs ──
        { "Material", "\xE6\x9D\x90\xE8\xB4\xA8" },                          // 材质
        { "Anim", "\xE5\x8A\xA8\xE7\x94\xBB" },                              // 动画
        { "Physics", "\xE7\x89\xA9\xE7\x90\x86" },                           // 物理
        { "Scene", "\xE5\x9C\xBA\xE6\x99\xAF" },                             // 场景
        { "Cook", "\xE7\x83\xAD\xE7\x83\xA4" },                              // 烘焙
        // ── timeline ──
        { "Timeline", "\xE6\x97\xB6\xE9\x97\xB4\xE8\xBD\xB4" },               // 时间轴
        { "Pause", "\xE6\x9A\x82\xE5\x81\x9C" },                             // 暂停
        { "Play", "\xE6\x92\xAD\xE6\x94\xBE" },                              // 播放
        { "Quest: on", "Quest: \xE5\xBC\x80" },                              // Quest: 开
        { "Quest: off", "Quest: \xE5\x85\xB3" },                             // Quest: 关
        // ── cook panel ──
        { "Install to headset after cook (auto)", "\xE7\x83\xAD\xE7\x83\xA4\xE5\x90\x8E\xE8\x87\xAA\xE5\x8A\xA8\xE5\xAE\x89\xE8\xA3\x85\xE5\x88\xB0\xE5\xA4\xB4\xE6\x98\xBE" },  // 烘焙后自动安装到头显
        { "COOK  +  SIGN", "\xE7\x83\xAD\xE7\x83\xA4 + \xE7\xAD\xBE\xE5\x90\x8D" },                 // 烘焙 + 签名
        { "COOK + SIGN + INSTALL", "\xE7\x83\xAD\xE7\x83\xA4 + \xE7\xAD\xBE\xE5\x90\x8D + \xE5\xAE\x89\xE8\xA3\x85" },   // 烘焙 + 签名 + 安装
        { "Far clip (m)", "\xE8\xBF\x9C\xE8\xA3\x81\xE5\x89\xAA (m)" },        // 远裁剪 (m)
        { "Wi-Fi IP", "Wi-Fi IP" },
        { "Connect", "\xE8\xBF\x9E\xE6\x8E\xA5" },                            // 连接
        { "Device", "\xE8\xAE\xBE\xE5\xA4\x87" },                            // 设备
        { "Distance fog (preview + cook)", "\xE8\xB7\x9D\xE7\xA6\xBB\xE9\x9B\xBE (\xE9\xA2\x84\xE8\xA7\x88 + \xE7\x83\xAD\xE7\x83\xA4)" },  // 距离雾 (预览 + 烘焙)
        { "Restore original Haven 2025", "\xE6\x81\xA2\xE5\xA4\x8D\xE5\x8E\x9F\xE7\x89\x88 Haven 2025" },   // 恢复原版 Haven 2025
        { "INSTALL ONLY  (no re-cook if unchanged)", "\xE4\xBB\x85\xE5\xAE\x89\xE8\xA3\x85 (\xE6\x9C\xAA\xE6\x94\xB9\xE5\x8A\xA8\xE5\x88\x99\xE4\xB8\x8D\xE9\x87\x8D\xE7\x83\xA4)" },  // 仅安装 (未改动则不重烤)
        { "Shell restart after install: AUTO (only if no root)", "\xE5\xAE\x89\xE8\xA3\x85\xE5\x90\x8E\xE9\x87\x8D\xE5\x90\xAF Shell: \xE8\x87\xAA\xE5\x8A\xA8 (\xE4\xBB\x85\xE6\x97\xA0 root \xE6\x97\xB6)" },  // 安装后重启 Shell: 自动 (仅无 root 时)
        { "Shell restart after install: ALWAYS", "\xE5\xAE\x89\xE8\xA3\x85\xE5\x90\x8E\xE9\x87\x8D\xE5\x90\xAF Shell: \xE6\x80\xBB\xE6\x98\xAF" },   // 安装后重启 Shell: 总是
        { "Shell restart after install: NEVER", "\xE5\xAE\x89\xE8\xA3\x85\xE5\x90\x8E\xE9\x87\x8D\xE5\x90\xAF Shell: \xE4\xBB\x8E\xE4\xB8\x8D" },    // 安装后重启 Shell: 从不
        // ── common misc ──
        { "Language", "\xE8\xAF\xAD\xE8\xA8\x80" },                          // 语言
        { "UI language", "\xE7\x95\x8C\xE9\x9D\xA2\xE8\xAF\xAD\xE8\xA8\x80" },   // 界面语言
        { "Undo", "\xE6\x92\xA4\xE9\x94\x80" },                              // 撤销
        { "Redo", "\xE9\x87\x8D\xE5\x81\x9A" },                              // 重做
        { "Delete", "\xE5\x88\xA0\xE9\x99\xA4" },                            // 删除
        { "Duplicate", "\xE5\xA4\x8D\xE5\x88\xB6" },                         // 复制
        { "Hide", "\xE9\x9A\x90\xE8\x97\x8F" },                              // 隐藏
        { "Show", "\xE6\x98\xBE\xE7\xA4\xBA" },                              // 显示
        { "Reset", "\xE9\x87\x8D\xE7\xBD\xAE" },                             // 重置
        { "Position", "\xE4\xBD\x8D\xE7\xBD\xAE" },                          // 位置
        { "Rotation", "\xE6\x97\x8B\xE8\xBD\xAC" },                          // 旋转
        { "Color", "\xE9\xA2\x9C\xE8\x89\xB2" },                             // 颜色
    };
    n = sizeof(T) / sizeof(T[0]);
    return T;
}

// Built once per active language: EN string -> translated (or absent = keep English).
inline std::unordered_map<std::string, const char*>& activeMap() {
    static std::unordered_map<std::string, const char*> m;
    static int builtFor = -1;
    if (builtFor != g_lang) {
        m.clear(); builtFor = g_lang;
        if (g_lang != EN) {
            size_t n; const Entry* T = table(n);
            for (size_t i = 0; i < n; ++i) {
                const char* v = (g_lang == ZH) ? T[i].zh : nullptr;
                if (v && *v) m[T[i].en] = v;
            }
        }
    }
    return m;
}

// The one funnel: translate an English UI string to the active language, or return it unchanged.
inline const char* tr(const char* s) {
    if (g_lang == EN || !s || !*s) return s;
    auto& m = activeMap();
    auto it = m.find(s);
    return it == m.end() ? s : it->second;
}

// Every non-ASCII codepoint used by ANY translation (across all non-EN languages), so the font atlas can bake
// exactly the CJK glyphs the UI needs (a curated few hundred) instead of all of Unicode.
inline void collectExtraCodepoints(std::vector<unsigned>& out) {
    out.clear();
    std::unordered_map<unsigned, char> seen;
    size_t n; const Entry* T = table(n);
    auto scan = [&](const char* s) { if (!s) return; const char* p = s; while (*p) { unsigned cp = utf8Next(p); if (cp >= 0x80 && !seen.count(cp)) { seen[cp] = 1; out.push_back(cp); } } };
    for (size_t i = 0; i < n; ++i) scan(T[i].zh);
    // language menu labels ("中文") also need glyphs even before any table string is shown
    scan(langLabel(ZH)); scan(langMenuName(ZH));
}

} // namespace i18n
