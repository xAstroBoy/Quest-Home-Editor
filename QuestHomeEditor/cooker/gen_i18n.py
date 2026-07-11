#!/usr/bin/env python3
# Bakes the editor's UI translations into src/ui/i18n.h (GitHub #10).
#
# THE TRANSLATION DATA LIVES IN  lang/strings.py  (the single source of truth). This generator only
# reads that folder and emits a pure-ASCII C++ header (UTF-8 as \xNN escapes) that is compiled into
# the binary -> the shipped tool has ZERO runtime/external dependency (no .json/.po at runtime, no lib).
#
# It also SCANS the source for every translatable UI string (labels/buttons/tabs/tooltips) and reports
# any that are missing from lang/strings.py, so "every string is in there" is enforceable, not a hope.
#
# Add a language / string: edit lang/strings.py, then re-run  python cooker/gen_i18n.py .
import os, re, importlib.util

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ── load the translation data from the lang/ folder ──
_spec = importlib.util.spec_from_file_location("lang_strings", os.path.join(ROOT, "lang", "strings.py"))
_lang = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(_lang)
LANGS = _lang.LANGS
T = _lang.T

def esc(s):
    return ''.join('\\x%02x' % b for b in s.encode('utf-8'))

# ══════════════════════════════════════════════════════════════════════════════════════════════════
#  COVERAGE SCANNER — pull every UI string literal the editor shows the user, so we can prove the
#  table covers them. UI text funnels through ui::Context widgets (button/tab/label/tip/textAligned/
#  checkbox/toggle) which all call i18n::tr(). We extract the string-literal arguments to those calls
#  (paren-balanced, adjacent literals concatenated), plus the string arrays that feed them.
# ══════════════════════════════════════════════════════════════════════════════════════════════════
UI_SOURCES = ["src/ui/editor.h", "src/ui/ui_core.h"]
FUNNELS = ("tip", "button", "tab", "label", "textAligned", "checkbox", "toggle", "labelR", "tabR")

def _read_cstr(text, i):
    """text[i]=='\"' -> (decoded_python_str, index_after_closing_quote)."""
    assert text[i] == '"'; i += 1; out = []
    while i < len(text):
        c = text[i]
        if c == '\\':
            n = text[i+1] if i+1 < len(text) else ''
            out.append({'n':'\n','t':'\t','"':'"','\\':'\\','r':'\r','0':'\0'}.get(n, n)); i += 2
        elif c == '"':
            return ''.join(out), i+1
        else:
            out.append(c); i += 1
    return ''.join(out), i

def _adjacent_literals(text, i):
    """From index i, read one-or-more adjacent \"...\" literals (C concatenation), skipping whitespace
    and line-continuations/comments between them. Returns (concatenated, index_after)."""
    parts = []
    while i < len(text):
        while i < len(text) and text[i] in ' \t\r\n': i += 1
        if i+1 < len(text) and text[i] == '/' and text[i+1] == '/':      # line comment between literals
            while i < len(text) and text[i] != '\n': i += 1
            continue
        if i < len(text) and text[i] == '"':
            s, i = _read_cstr(text, i); parts.append(s); continue
        break
    return ''.join(parts), i

def _looks_like_ui_text(s):
    if not s or '%' in s: return False                       # skip empty + printf format strings
    if not any(ch.isalpha() for ch in s): return False       # must contain a letter
    if s.startswith('#') or s.startswith('apk://'): return False
    # skip path/id/env-var-ish tokens (lowercase, no spaces, has / _ . or ::)
    if ' ' not in s and re.fullmatch(r'[a-z0-9_./:\-]+', s): return False
    if ' ' not in s and s.isupper() and len(s) <= 3: return False   # tiny axis pills X/Y/Z handled by table
    return True

def scan_ui_strings():
    found = {}   # string -> first "file:line"
    callre = re.compile(r'\b(?:cx|self)?\.?(' + '|'.join(FUNNELS) + r')\s*\(')
    arrre  = re.compile(r'const\s+char\s*\*\s*\w+\s*\[\s*\]\s*=\s*\{')
    for rel in UI_SOURCES:
        p = os.path.join(ROOT, rel)
        if not os.path.isfile(p): continue
        text = open(p, encoding='utf-8').read()
        def note(s, pos):
            if _looks_like_ui_text(s) and s not in found:
                found[s] = "%s:%d" % (rel, text.count('\n', 0, pos) + 1)
        # 1) string literals inside funnel-call argument lists (paren-balanced)
        for m in callre.finditer(text):
            i = m.end(); depth = 1
            while i < len(text) and depth:
                c = text[i]
                if c == '"':
                    s, j = _adjacent_literals(text, i); note(s, i); i = j; continue
                if c == "'":                                   # char literal
                    i += 3 if text[i+1] == '\\' else 2; continue
                if c == '(': depth += 1
                elif c == ')': depth -= 1
                elif c == ';' and depth == 1: break
                i += 1
        # 2) string arrays that feed funnels (menus[], tabs[], *Tips[], ops[], ...)
        for m in arrre.finditer(text):
            i = m.end(); depth = 1
            while i < len(text) and depth:
                c = text[i]
                if c == '"':
                    s, j = _adjacent_literals(text, i); note(s, i); i = j; continue
                if c == '{': depth += 1
                elif c == '}': depth -= 1
                i += 1
    return found

# ── generate ──
codes = [c for c, _, _ in LANGS]
idx = {c: i for i, c in enumerate(codes)}

L = []
L.append("// AUTO-GENERATED by cooker/gen_i18n.py from lang/strings.py -- do not edit. Edit lang/strings.py + re-run.")
L.append("// Editor UI localization (GitHub #10). English is the default/key; other languages fall back to")
L.append("// English per-string. UI strings funnel through ui::Context (textAligned/checkbox/tip) -> tr().")
L.append("#pragma once")
L.append("#include <string>")
L.append("#include <unordered_map>")
L.append("#include <vector>")
L.append("#include <cstdint>")
L.append("")
L.append("namespace i18n {")
L.append("")
L.append("enum Lang { " + ", ".join(codes) + ", LANG_COUNT };")
L.append("")
L.append("// native menu name + primary script tag per language")
L.append("struct LangInfo { const char* code; const char* name; const char* script; };")
langinfo = ", ".join('{"%s","%s","%s"}' % (c, esc(n), s) for c, n, s in LANGS)
L.append("inline const LangInfo* langs(size_t& n) { static const LangInfo Larr[] = { %s }; n = LANG_COUNT; return Larr; }" % langinfo)
L.append("inline int g_lang = EN;")
L.append("inline const char* langName(int l) { size_t n; auto* Larr=langs(n); return (l>=0&&l<(int)n)?Larr[l].name:Larr[0].name; }")
L.append("inline const char* langScript(int l) { size_t n; auto* Larr=langs(n); return (l>=0&&l<(int)n)?Larr[l].script:Larr[0].script; }")
L.append("")
L.append("inline unsigned utf8Next(const char*& p) {")
L.append("    unsigned c = (unsigned char)*p++;")
L.append("    if (c < 0x80) return c;")
L.append("    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;")
L.append("    unsigned cp = c & (0x7F >> (n + 1));")
L.append("    while (n-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);")
L.append("    return cp;")
L.append("}")
L.append("")
L.append("// One row per translatable string: t[EN] = english key; t[<lang>] = translation or nullptr (=fall back).")
L.append("struct Entry { const char* t[LANG_COUNT]; };")
L.append("inline const Entry* table(size_t& n) {")
L.append("    static const Entry Tarr[] = {")
for en, tr in T.items():
    cells = ["nullptr"] * len(codes)
    cells[idx["EN"]] = '"%s"' % esc(en)
    for code, txt in tr.items():
        if code in idx and txt:
            cells[idx[code]] = '"%s"' % esc(txt)
    L.append("        {{ " + ", ".join(cells) + " }},")
L.append("    };")
L.append("    n = sizeof(Tarr) / sizeof(Tarr[0]);")
L.append("    return Tarr;")
L.append("}")
L.append("")
L.append("inline std::unordered_map<std::string, const char*>& activeMap() {")
L.append("    static std::unordered_map<std::string, const char*> m; static int builtFor = -1;")
L.append("    if (builtFor != g_lang) { m.clear(); builtFor = g_lang;")
L.append("        if (g_lang != EN) { size_t n; const Entry* Tarr = table(n);")
L.append("            for (size_t i=0;i<n;i++) { const char* v = Tarr[i].t[g_lang]; if (v && *v && Tarr[i].t[EN]) m[Tarr[i].t[EN]] = v; } } }")
L.append("    return m;")
L.append("}")
L.append("inline const char* tr(const char* s) {")
L.append("    if (g_lang == EN || !s || !*s) return s;")
L.append("    auto& m = activeMap(); auto it = m.find(s); return it == m.end() ? s : it->second;")
L.append("}")
L.append("")
L.append("// Every non-ASCII codepoint any translation uses, so the font atlas bakes exactly those glyphs.")
L.append("inline void collectExtraCodepoints(std::vector<unsigned>& out) {")
L.append("    out.clear(); std::unordered_map<unsigned,char> seen;")
L.append("    auto scan=[&](const char* s){ if(!s)return; const char* p=s; while(*p){ unsigned cp=utf8Next(p); if(cp>=0x80&&!seen.count(cp)){ seen[cp]=1; out.push_back(cp);} } };")
L.append("    size_t n; const Entry* Tarr = table(n); for (size_t i=0;i<n;i++) for (int l=1;l<LANG_COUNT;l++) scan(Tarr[i].t[l]);")
L.append("    size_t ln; auto* Larr=langs(ln); for (size_t i=0;i<ln;i++) scan(Larr[i].name);")
L.append("}")
L.append("")
L.append("} // namespace i18n")
L.append("")

out = "\n".join(L).replace("—", "--")   # em-dash -> ASCII so the header stays pure-ASCII
OUT = os.path.join(ROOT, "src", "ui", "i18n.h")
open(OUT, "w", newline="\n", encoding="ascii").write(out)
print("wrote", os.path.relpath(OUT, ROOT), "with", len(LANGS), "languages and", len(T), "strings")

# ── coverage report: which UI strings are NOT yet in lang/strings.py ──
found = scan_ui_strings()
missing = {s: loc for s, loc in found.items() if s not in T}
man = os.path.join(ROOT, "lang", "_untranslated.txt")
with open(man, "w", newline="\n", encoding="utf-8") as f:
    f.write("# UI strings found in the source that are NOT yet in lang/strings.py (add them there).\n")
    f.write("# %d missing of %d scanned UI strings.  Regenerate: python cooker/gen_i18n.py\n\n" % (len(missing), len(found)))
    for s in sorted(missing):
        f.write("%-14s | %s\n" % (missing[s], s.replace("\n", "\\n")))
print("coverage: %d/%d UI strings in the table; %d missing -> lang/_untranslated.txt"
      % (len(found) - len(missing), len(found), len(missing)))
