// ============================================================================
//  Quest-Ctl AIO — NeoZygisk module (one module, ALL libshell.so patches)
// ----------------------------------------------------------------------------
//  Consolidates every previously-separate Quest VR-shell module into ONE clean
//  Zygisk module that patches libshell.so live inside com.oculus.vrshell:
//
//    1. FAR-CLIP REMOVAL  — kill the VR-portal camera's hardcoded 5000 far plane.
//    2. UNLIMITED JUMP    — numAirJumps -> 9999 every locomotion update (ON by
//       default, baked in, applied on shell start) + optional FLY.
//    3. PLAYER CONTROL    — teleport / walk / rotate (via the shell's own
//       ShellApp message queue) + locomotion-output overrides. Driven by props
//       the questctl CLI sets (hsr.tp / hsr.walk / hsr.rot).
//    4. ENV/DEBUG toggles — verbose ShellHsr logs + in-headset ImGui debug-UI.
//
//  The env-control "keep the headset worn + select env" feature is pure root
//  (service.sh + questctl CLI), NOT a libshell patch — see README.
//
//  ──────────────────────────────────────────────────────────────────────────
//  ⚠ EVERY libshell address is located by BYTE-PATTERN SCAN of the loaded
//  libshell.so (embedded in VrShell.apk) — NOT a hardcoded file offset. The user
//  is moving to a NEW shell build; offsets break, pattern scans still find the
//  site. PC-relative targets (ADRP/ADD/LDR-literal that materialize globals) are
//  NOT pattern-matched on their immediate bytes (those re-encode per build) —
//  instead we find the *referencing function* by a unique signature, then DECODE
//  the live ADRP/ADD(or LDR) immediates at a known in-function offset to compute
//  the global's runtime address. Each signature is documented inline.
//
//  ⚠ NeoZygisk lesson: it relocates/hides the module .so after specialize, so any
//  module .data/.bss address baked into an in-shell trampoline goes STALE ->
//  SIGSEGV. Trampolines therefore touch only scratch regs and bake only VALUES,
//  and the player-ctl command block lives in an INDEPENDENT mmap page (never
//  module memory), its 64-bit address baked as an LDR-literal .quad.
//
//  Disk is NEVER modified (mprotect in-memory only); Magisk re-applies each boot.
//  Fail-safe: a missing signature -> that feature no-ops, shell runs stock.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cinttypes>
#include <exception>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <android/log.h>

#include "zygisk.hpp"

#define LOG_TAG "QuestCtlAIO"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================================
//  SIGNATURES  (all derived from V205.2 libshell.so; wildcards = PC-rel / imm)
// ============================================================================

// ── FEATURE 1: far-clip — fn__EFA9D0 (per-frame ViewCameraManager update).
//    kFarSig sits at fn+0x1C; the prologue at fn+0 disambiguates the generic sig
//    (93 raw hits) to the one real function. On re-scan, accept the hook stub
//    (0x58000050 = LDR X16,[PC,#8]) as a valid prologue too.
//      prologue : ff 83 01 d1  fd 7b 04 a9  f4 4f 05 a9  fd 03 01 91
//      kFarSig  : 08 cc 40 b9  09 5c 40 f9  08 79 1f 12  08 cc 00 b9   (@fn+0x1C)
static const uint8_t kFarPrologue[16] = { 0xff,0x83,0x01,0xd1, 0xfd,0x7b,0x04,0xa9, 0xf4,0x4f,0x05,0xa9, 0xfd,0x03,0x01,0x91 };
static const uint8_t kFarSig[16]      = { 0x08,0xcc,0x40,0xb9, 0x09,0x5c,0x40,0xf9, 0x08,0x79,0x1f,0x12, 0x08,0xcc,0x00,0xb9 };
static const int     kFarSigOff       = 0x1C;

// ── FEATURE 2 + 3: locomotion — SlideLocomotionController_update.
//    UNIQUE run at fn+0x34 (the two ??-b4 are CBZ branch immediates, PC-rel):
//      37 c0 40 f9 ?? ?? ?? b4 36 c8 40 f9 f4 03 01 aa ?? ?? ?? b4 9a d6 40 f9 03 3c 00 f9
//      = LDR X23,[X1,#0x180]; CBZ X23,*; LDR X22,[X1,#0x190]; MOV X20,X1;
//        CBZ X22,*; LDR X26,[X20,#0x1A8]; STR X3,[X0,#0x78]
//    Func start = hit - 0x34, confirmed by entry word SUB SP,SP,#0x160.
//    IDA-confirmed fields: config=*(a2+400); numAirJumps=*(config+44) (compared
//    > airJumpCount to gate each mid-air jump); jumpInitialVelocity=*(config+32);
//    locomotion output a3+144=newPos, a3+176=per-frame move delta (v127).
static const char*    kLocoSig    = "37 c0 40 f9 ?? ?? ?? b4 36 c8 40 f9 f4 03 01 aa ?? ?? ?? b4 9a d6 40 f9 03 3c 00 f9";
static const int      kLocoSigOff = 0x34;
static const uint32_t kLocoProlog = 0xD10583FFu;   // SUB SP,SP,#0x160 (loco fn entry word)

// ── FEATURE 3: ShellApp teleport — nativeTeleportToCoordinates.
//    Brackets the g_ShellApp ADRP/LDR pair (?? ?? ?? 90 = ADRP, ?? ?? 40 f9 = LDR):
//      e8 16 40 f9 a8 83 1d f8 ?? ?? ?? 90 ?? ?? 40 f9 ?? ?? 00 b4 f6 03 00 aa
//      = LDR X8,[X23,#stackguard]; STUR X8,[X29,#-0x28]; ADRP X8,g_ShellApp@PAGE;
//        LDR X24,[X8,#g_ShellApp@PAGEOFF]; CBZ X24,*; MOV X22,X0
//    The match address points at the first LDR; ADRP is at match+8, LDR at match+12.
//    Decode them to resolve g_ShellApp's runtime address (no fixed offset).
static const char*    kTpSig     = "e8 16 40 f9 a8 83 1d f8 ?? ?? ?? 90 ?? ?? 40 f9 ?? ?? 00 b4 f6 03 00 aa";
static const int      kTpAdrpOff = 8;              // ADRP is at (match + 8)

// ── FEATURE 3: TypedMessageQueue post — unique prologue (3-way disambiguated):
//      ff 83 01 d1 fd 7b 02 a9 f7 1b 00 f9 f6 57 04 a9 f4 4f 05 a9 fd 83 00 91
//      57 d0 3b d5 f4 03 02 aa e8 16 40 f9 a8 83 1f f8 08 fc df 08 88 09 00 37
static const char*    kTmqSig = "ff 83 01 d1 fd 7b 02 a9 f7 1b 00 f9 f6 57 04 a9 f4 4f 05 a9 fd 83 00 91 57 d0 3b d5 f4 03 02 aa e8 16 40 f9 a8 83 1f f8 08 fc df 08 88 09 00 37";

// ── FEATURE 4: verbose logs — Log_levelEnabled (resolves the logLevel global).
//    Unique prologue; the ADRP that materializes the logLevel byte is at fn+0x30,
//    its LDRB at fn+0x34. Original module wrote 3->5 to unhide DEBUG logs.
//      fd 7b be a9 f3 0b 00 f9 fd 03 00 91 ?? ?? ?? 90 08 01 2d 91
static const char*    kLogSig     = "fd 7b be a9 f3 0b 00 f9 fd 03 00 91 ?? ?? ?? 90 08 01 2d 91";
static const int      kLogAdrpOff = 0x30;          // ADRP@fn+0x30, LDR(B)@fn+0x34

// ── FEATURE 4: debug-UI — ImGuiRenderSystem_register (the ONLY fn referencing the
//    debugui DynamicConfig globals). First ADRL pair materializes the anchor
//    global (debugui.enabled, 0x29C59D0): ADRP@fn+0x14, ADD@fn+0x18. The other 3
//    debugui globals sit at fixed deltas from it within the same data block.
//      fd 7b bd a9 f5 0b 00 f9 f4 4f 02 a9 fd 03 00 91 ca ad 8e 52
//      ?? ?? ?? 90 ?? ?? ?? 91 ?? ?? ?? 90 ?? ?? ?? 91 09 01 80 52
static const char*    kImguiSig     = "fd 7b bd a9 f5 0b 00 f9 f4 4f 02 a9 fd 03 00 91 ca ad 8e 52 ?? ?? ?? 90 ?? ?? ?? 91 ?? ?? ?? 90 ?? ?? ?? 91 09 01 80 52";
static const int      kImguiAdrpOff = 0x14;        // ADRP@fn+0x14, ADD@fn+0x18 -> debugui.enabled global
static const int      kDuEnabledD   = 0x000;       // debugui.enabled       (0x29C59D0)
static const int      kDuConnectD   = 0x0D0;       // debugui.connectOnStart (0x29C5AA0)
static const int      kDuServerD    = 0x130;       // debugui.serverEnabled  (0x29C5B00)
static const int      kDuPortD      = 0x190;       // debugui.localPort      (0x29C5B60)

// ── FEATURE 5: CRASHPROOF — AssetManager_initLoadedAsset (per-asset init dispatch,
//    runs on HSR:Async). Wrap it in a C++ try/catch so a Meta initializer that
//    THROWS std::length_error (unguarded std::string resize — the lakeside skinned
//    tree) is caught at the function boundary, logged, and turned into the SAME
//    graceful "asset failed" return (low byte 0xDB = (uint8_t)-25) that the
//    No-AssetInitializer path returns. Env then loads MINUS the bad mesh instead of
//    crash-looping (terminate). The throw is EARLY — before the SharedMutex lock and
//    dependency-map writes — so catching at the boundary holds no lock / mutates no
//    map = safe. fn__D8522C (the caller) only LOGS a non-zero return and continues.
//    Entry sig (48 bytes, ALL fixed — no PC-relative imm in the prologue, so no
//    wildcards): the full register-save prologue + 0x1D0 frame + TPIDR_EL0 stack
//    guard read + `ldr x8,[x1]` disambiguates from 4 sibling fns sharing the prologue.
//      fd 7b ba a9 fc 6f 01 a9 fd 03 00 91 fa 67 02 a9
//      f8 5f 03 a9 f6 57 04 a9 f4 4f 05 a9 ff 43 07 d1
//      5c d0 3b d5 88 17 40 f9 a8 83 1e f8 28 00 40 f9
static const uint8_t kInitAssetSig[48] = {
    0xfd,0x7b,0xba,0xa9, 0xfc,0x6f,0x01,0xa9, 0xfd,0x03,0x00,0x91, 0xfa,0x67,0x02,0xa9,
    0xf8,0x5f,0x03,0xa9, 0xf6,0x57,0x04,0xa9, 0xf4,0x4f,0x05,0xa9, 0xff,0x43,0x07,0xd1,
    0x5c,0xd0,0x3b,0xd5, 0x88,0x17,0x40,0xf9, 0xa8,0x83,0x1e,0xf8, 0x28,0x00,0x40,0xf9 };

// ============================================================================
//  Player-control command block — lives in an mmap'd page (NOT module .data).
//  Layout baked into the loco trampoline's byte offsets; do not reorder.
// ============================================================================
struct CmdBlock {
    volatile uint32_t tp_pending;   // +0  : 1 = apply teleport delta this frame (auto-clears)
    volatile float    tp[3];        // +4  : target x,y,z
    volatile uint32_t walk_on;      // +16 : 1 = force the per-frame move delta
    volatile float    walk[2];      // +20 : fwd, strafe
    volatile uint32_t tick;         // +28 : trampoline increments each loco-update call
    volatile float    curpos[3];    // +32 : trampoline copies a3+144 here each frame (live player pos)
    volatile uint64_t a3_addr;      // +48 : trampoline stores a3 here (probe jump-button bytes from the worker)
    volatile uint32_t moon_on;      // +56 : 1 = moonjump active
    volatile uint32_t moon_off;     // +60 : (reserved)
    volatile float    moon_rise;    // +64 : per-frame vertical lift while latched
    volatile uint32_t moon_timer;   // +68 : countdown; trampoline lifts while >0, refreshed each jump pulse
    volatile uint32_t moon_hold;    // +72 : frames to keep lifting after each jump pulse (bridges the gaps)
    volatile uint64_t cfg_addr;     // +80 : trampoline stores config=*(a2+400) here (probe numAirJumps/jumpInitVel)
    volatile uint64_t cc_addr;      // +88 : trampoline stores char-controller v10=*(*(a2+384)+240) (probe vy field)
    volatile uint64_t a2_addr;      // +96 : trampoline stores a2 (loco input/context) — probe jump mode-gate flags
    volatile uint64_t pc_addr;      // +104: ccmove probe stores a1 (PhysxCharacterController) — find jump-velocity slot
    volatile float    pc_y32;       // +112: a1+32.y snapshot at move() entry
    volatile float    pc_y64;       // +116: a1+64.y snapshot
    volatile float    pc_y96;       // +120: a1+96.y (velocity) snapshot
    volatile uint32_t jbtn;         // +124: a2 button-candidate bytes [144,160,192,208] (synchronous probe)
    volatile uint64_t hpi_addr;     // +128: HsrPlayerInput component (captured in vf4 hook) — raw button states
    volatile uint32_t jbtn_off;     // +136: HsrPlayerInput offset of the JUMP button's pressed byte (once found)
};

// ============================================================================
//  Property helpers — hsr.<name> or persist.hsr.<name>
// ============================================================================
static bool prop_on(const char* name, bool dflt) {
    char buf[PROP_VALUE_MAX] = {0}, k[96];
    snprintf(k, sizeof k, "hsr.%s", name);
    if (__system_property_get(k, buf) <= 0) { snprintf(k, sizeof k, "persist.hsr.%s", name); __system_property_get(k, buf); }
    if (!buf[0]) return dflt;
    char c = buf[0]; return c=='1'||c=='t'||c=='T'||c=='y'||c=='Y';
}
static float prop_f(const char* name, float dflt, float lo, float hi) {
    char buf[PROP_VALUE_MAX] = {0}, k[96];
    snprintf(k, sizeof k, "hsr.%s", name);
    if (__system_property_get(k, buf) <= 0) { snprintf(k, sizeof k, "persist.hsr.%s", name); __system_property_get(k, buf); }
    if (!buf[0]) return dflt;
    float v = strtof(buf, nullptr);
    return (v > lo && v < hi) ? v : dflt;
}

static void wr32(uint8_t* p, uint32_t v) {
    size_t PG=(size_t)sysconf(_SC_PAGESIZE); uintptr_t pg=(uintptr_t)p & ~(uintptr_t)(PG-1);
    if (mprotect((void*)pg, PG, PROT_READ|PROT_WRITE)==0) *(volatile uint32_t*)p = v;
}
static void wr8(uint8_t* p, uint8_t v) {
    size_t PG=(size_t)sysconf(_SC_PAGESIZE); uintptr_t pg=(uintptr_t)p & ~(uintptr_t)(PG-1);
    if (mprotect((void*)pg, PG, PROT_READ|PROT_WRITE)==0) *(volatile uint8_t*)p = v;
}

// ============================================================================
//  ARM64 instruction decode — resolve a PC-relative global from live bytes.
// ============================================================================
static uint64_t adrp_page(uint8_t* pc) {            // ADRP at *pc -> the page base it computes
    uint32_t w = *(uint32_t*)pc;
    int64_t immlo = (w >> 29) & 3, immhi = (w >> 5) & 0x7FFFF;
    int64_t imm = (immhi << 2) | immlo;
    if (imm & (1LL<<20)) imm -= (1LL<<21);          // sign-extend 21 bits
    return ((uint64_t)pc & ~0xFFFULL) + ((uint64_t)imm << 12);
}
static uint32_t add_imm(uint32_t w)  { return (w >> 10) & 0xFFF; }                 // ADD (imm)  unscaled
static uint32_t ldr_uoff(uint32_t w) {                                            // LDR/LDRB (uimm) -> scaled byte offset
    uint32_t imm12 = (w >> 10) & 0xFFF;
    uint32_t sz = (w >> 30) & 3;                    // size field -> element scale (LDRB sz=0)
    return imm12 << sz;
}
// Resolve the global addressed by an ADRP at adrp_pc followed by ADD/LDR at adrp_pc+4.
static uint8_t* resolve_adrp_global(uint8_t* adrp_pc) {
    uint64_t page = adrp_page(adrp_pc);
    uint32_t nextw = *(uint32_t*)(adrp_pc + 4);
    uint32_t off;
    if ((nextw & 0x7F800000u) == 0x11000000u || (nextw & 0x7F800000u) == 0x91000000u)
        off = add_imm(nextw);                       // ADD Xd, Xn, #imm (32/64-bit)
    else
        off = ldr_uoff(nextw);                      // LDR/LDRB Xt, [Xn, #imm]
    return (uint8_t*)(page + off);
}

// ============================================================================
//  Pattern scanner over the embedded libshell's exec segments
// ============================================================================
typedef uint8_t* (*scan_cb)(uint8_t* lo, uint8_t* hi);
static uint8_t* for_each_exec_seg(scan_cb cb) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return nullptr;
    char line[1024]; uint8_t* hit = nullptr;
    while (fgets(line, sizeof line, f) && !hit) {
        if (!strstr(line, "VrShell.apk") && !strstr(line, "libshell")) continue;
        uintptr_t lo = 0, hi = 0; char perms[8] = {0};
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %7s", &lo, &hi, perms) != 3 || hi <= lo || perms[2] != 'x') continue;
        mprotect((void*)lo, (size_t)(hi - lo), PROT_READ | PROT_EXEC);
        hit = cb((uint8_t*)lo, (uint8_t*)hi);
    }
    fclose(f);
    return hit;
}

// FEATURE 5 backtrace: the WHOLE libshell mapped range (any perm), for the
// frame-chain "is this return address inside libshell?" test. Captured lazily.
static uintptr_t g_libLo = 0, g_libHi = 0;
static void capture_lib_range() {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return;
    char line[1024]; uintptr_t lo = (uintptr_t)-1, hi = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "VrShell.apk") && !strstr(line, "libshell")) continue;
        uintptr_t a = 0, b = 0;
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &a, &b) != 2 || b <= a) continue;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    fclose(f);
    if (hi > lo) { g_libLo = lo; g_libHi = hi; }
}

// Parse a "AA BB ?? CC" pattern string into bytes[]/mask[] (mask 0 = wildcard).
static int parse_sig(const char* s, uint8_t* bytes, uint8_t* mask, int cap) {
    int n = 0;
    auto hex = [](char c)->int{ return (c>='0'&&c<='9')?c-'0':((c|0x20)>='a'&&(c|0x20)<='f')?(c|0x20)-'a'+10:-1; };
    for (const char* p = s; *p && n < cap; ) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') { bytes[n]=0; mask[n]=0; ++n; p += (p[1]=='?')?2:1; }
        else { int h=hex(p[0]), l=hex(p[1]); if (h<0||l<0) break; bytes[n]=(uint8_t)((h<<4)|l); mask[n]=0xFF; ++n; p+=2; }
    }
    return n;
}
// Generic masked scan of a single [lo,hi) range.
static uint8_t* scan_seg(uint8_t* lo, uint8_t* hi, const uint8_t* b, const uint8_t* m, int n) {
    if (n <= 0) return nullptr;
    uint8_t* end = hi - n;
    for (uint8_t* p = lo; p <= end; ++p) {
        int i = 0; for (; i < n; ++i) if (m[i] && p[i] != b[i]) break;
        if (i == n) return p;
    }
    return nullptr;
}

// ---- FEATURE 1 locator: kFarSig @ +0x1C, disambiguated by the prologue ----
static uint8_t* cb_find_far(uint8_t* lo, uint8_t* hi) {
    uint8_t* end = hi - sizeof kFarSig;
    for (uint8_t* p = lo; p <= end; ++p) {
        if (memcmp(p, kFarSig, sizeof kFarSig) != 0) continue;
        uint8_t* fn = p - kFarSigOff;
        if (fn >= lo && (memcmp(fn, kFarPrologue, sizeof kFarPrologue) == 0 || *(uint32_t*)fn == 0x58000050u)) return fn;
    }
    return nullptr;
}
static uint8_t* find_far() { return for_each_exec_seg(cb_find_far); }

// ---- FEATURE 5 locator: kInitAssetSig (48-byte fixed prologue, unique) ----
static uint8_t* cb_find_init_asset(uint8_t* lo, uint8_t* hi) {
    uint8_t* end = hi - sizeof kInitAssetSig;
    for (uint8_t* p = lo; p <= end; ++p)
        if (memcmp(p, kInitAssetSig, sizeof kInitAssetSig) == 0) return p;
    return nullptr;
}
static uint8_t* find_init_asset() { return for_each_exec_seg(cb_find_init_asset); }

// ---- FEATURE 2/3 locator: loco sig -> func start (hit - 0x34), confirm entry word ----
static uint8_t* cb_find_loco(uint8_t* lo, uint8_t* hi) {
    static uint8_t b[64], m[64]; static int n = 0; if (!n) n = parse_sig(kLocoSig, b, m, sizeof b);
    uint8_t* p = scan_seg(lo, hi, b, m, n);
    while (p) {
        uint8_t* fn = p - kLocoSigOff;
        if (fn >= lo && *(uint32_t*)fn == kLocoProlog) return fn;
        p = scan_seg(p + 1, hi, b, m, n);
    }
    return nullptr;
}
static uint8_t* find_loco() { return for_each_exec_seg(cb_find_loco); }

// ---- generic "find by sig string" (for tp / tmq / log / imgui anchors) ----
static const uint8_t* g_b; static const uint8_t* g_m; static int g_n;
static uint8_t* cb_find_generic(uint8_t* lo, uint8_t* hi) { return scan_seg(lo, hi, g_b, g_m, g_n); }
static uint8_t* find_sig(const char* sig) {
    static uint8_t b[128], m[128]; int n = parse_sig(sig, b, m, sizeof b);
    g_b = b; g_m = m; g_n = n; return for_each_exec_seg(cb_find_generic);
}

// ============================================================================
//  Trampoline install (overwrites fn[0..15] with an absolute jump to mmap'd code)
// ============================================================================
static bool install_hook(uint8_t* fn, const uint32_t* code, int words) {
    size_t PG = (size_t)sysconf(_SC_PAGESIZE);
    void* tramp = mmap(nullptr, PG, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return false;
    memcpy(tramp, code, (size_t)words*4);
    mprotect(tramp, PG, PROT_READ|PROT_EXEC);
    __builtin___clear_cache((char*)tramp, (char*)tramp + (size_t)words*4);
    uintptr_t tr = (uintptr_t)tramp;
    uint32_t jmp[4] = { 0x58000050u, 0xD61F0200u, (uint32_t)(tr & 0xFFFFFFFFu), (uint32_t)((uint64_t)tr >> 32) };
    uintptr_t a = (uintptr_t)fn & ~(uintptr_t)(PG-1);
    uintptr_t b = ((uintptr_t)fn + 16 + PG - 1) & ~(uintptr_t)(PG-1);
    if (mprotect((void*)a, b-a, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) return false;
    memcpy(fn, jmp, 16);
    mprotect((void*)a, b-a, PROT_READ|PROT_EXEC);
    __builtin___clear_cache((char*)fn, (char*)fn + 16);
    return true;
}

// arm64 encoders for the player-ctl detour
static inline uint32_t LDR_W (int rt,int rn,int off){ return 0xB9400000u | ((uint32_t)(off>>2)<<10) | ((uint32_t)rn<<5) | (uint32_t)rt; }
static inline uint32_t STR_W (int rt,int rn,int off){ return 0xB9000000u | ((uint32_t)(off>>2)<<10) | ((uint32_t)rn<<5) | (uint32_t)rt; }
static inline uint32_t CBZ_W (int rt,int n_ins){     return 0x34000000u | (((uint32_t)n_ins & 0x7FFFF)<<5) | (uint32_t)rt; }
static inline uint32_t LDR_XLIT(int rt,int n_words){ return 0x58000000u | (((uint32_t)n_words & 0x7FFFF)<<5) | (uint32_t)rt; }

// ============================================================================
//  FEATURE 1 — far-clip hook on fn__EFA9D0
// ============================================================================
static void apply_farclip(uint8_t* fn, float farVal) {
    uint32_t fb; { float ff = farVal; memcpy(&fb, &ff, 4); }
    uintptr_t cont = (uintptr_t)fn + 16;
    uint32_t code[18] = {
        0xF9405C09u,                                                 // LDR  X9,[X0,#184]   ; *(a1+184)
        0xB4000129u,                                                 // CBZ  X9,+9 (skip)
        0xF9400129u,                                                 // LDR  X9,[X9]         ; mgr
        0xB40000E9u,                                                 // CBZ  X9,+7 (skip)
        0xF9403D29u,                                                 // LDR  X9,[X9,#120]    ; cam
        0xB40000A9u,                                                 // CBZ  X9,+5 (skip)
        0x52800000u | (uint32_t)((fb & 0xFFFF) << 5) | 10u,          // MOVZ W10,#far_lo
        0x72A00000u | (uint32_t)(((fb >> 16) & 0xFFFF) << 5) | 10u,  // MOVK W10,#far_hi,LSL#16
        0xB900A52Au,                                                 // STR  W10,[X9,#164]   ; left eye far
        0xB900BD2Au,                                                 // STR  W10,[X9,#188]   ; right eye far
        0xD10183FFu,                                                 // (skip) relocated prologue: SUB SP,SP,#0x60
        0xA9047BFDu,                                                 // STP X29,X30,[SP,#0x40]
        0xA9054FF4u,                                                 // STP X20,X19,[SP,#0x50]
        0x910103FDu,                                                 // ADD X29,SP,#0x40
        0x58000049u,                                                 // LDR X9,[PC,#8]       ; cont
        0xD61F0120u,                                                 // BR  X9
        (uint32_t)(cont & 0xFFFFFFFFu),
        (uint32_t)((uint64_t)cont >> 32),
    };
    if (install_hook(fn, code, 18)) LOGI("FAR-CLIP killed: fn__EFA9D0@0x%" PRIxPTR " far=%.0f", (uintptr_t)fn, farVal);
    else                            LOGW("far-clip: install failed (left stock)");
}

// ============================================================================
//  MOONJUMP — mid-function hook in PhysxCharacterController::move at 0xA96718, RIGHT AFTER the gravity
//  recompute (0xa9670c) and BEFORE the displacement v125 = a1+32 + a1+64 + *(v3+80) is computed (0xa968ac).
//  v3 (the gravity/jump accumulator component) is live in X20 here; its vertical = v3+84. The game's own
//  jump writes +0.06 to v3+84 (probe-confirmed: +0.06 jump, -0.067 fall). While the jump button (a3+97) is
//  held, we ADD cmd.moon_rise to v3+84 here -> v125.y rises -> the capsule sweeps up via the game's OWN
//  move path = fly. Release -> we stop -> the game's gravity (untouched) resumes. The 4 instructions at
//  0xA96718 are plain reg-relative (LDR Q1,[X19,#96]; STRH WZR,[X8]; LDR X8,[X19,#408]; STR WZR,[X19,#576]),
//  position-independent, so relocating them is safe. We only touch X9/X10/X11/S1/S2 (scratch) + read X20.
// ============================================================================
static void apply_ccmove(uint8_t* loco, CmdBlock* cmd) {
    uint8_t* fn = loco - 0x18CCEA0 + 0xA96718;     // mid-move(), post-gravity, v3=X20
    uintptr_t cont = (uintptr_t)fn + 16;           // 0xA96728
    uint32_t c[32]; int n = 0;
    int ldrcmd=n++;                      // LDR X10,[PC,#cmdq]
    int bv3=n++;                         // CBZ X20, run        (v3 null -> skip)
    c[n++]=0xBD405681u;                  // LDR S1,[X20,#84]    v3+84 (accumulator.y)
    c[n++]=0xBD007141u;                  // STR S1,[X10,#112]   snapshot -> cmd.pc_y32
    c[n++]=LDR_W(9,10,56);               // W9 = cmd.moon_on
    int bon=n++;                         // CBZ W9, run
    c[n++]=0xF940414Bu;                  // LDR X11,[X10,#128]  hpi_addr (HsrPlayerInput)
    int ba3=n++;                         // CBZ X11, run
    c[n++]=0x39430969u;                  // LDRB W9,[X11,#194]  hpi+0xC2 = RAW jump button pressed (1 held, 0 released)
    int bheld=n++;                       // CBZ W9, run
    c[n++]=0xBD404142u;                  // LDR S2,[X10,#64]    cmd.moon_rise
    c[n++]=0x1E222821u;                  // FADD S1,S1,S2
    c[n++]=0xBD005681u;                  // STR S1,[X20,#84]    v3+84 += moon_rise  (the jump's OWN accumulator)
    int run=n;
    c[n++]=0x3DC01A61u; c[n++]=0x7900011Fu; c[n++]=0xF940CE68u; c[n++]=0xB902427Fu; // relocated 4 (reg-relative)
    c[n++]=0x58000049u;                  // LDR X9,[PC,#8]
    c[n++]=0xD61F0120u;                  // BR X9 -> 0xA96728
    c[n++]=(uint32_t)(cont & 0xFFFFFFFFu); c[n++]=(uint32_t)((uint64_t)cont>>32);   // .quad cont
    int cmdq=n; c[n++]=(uint32_t)((uintptr_t)cmd & 0xFFFFFFFFu); c[n++]=(uint32_t)((uint64_t)(uintptr_t)cmd>>32); // .quad cmd
    c[ldrcmd]=LDR_XLIT(10, cmdq-ldrcmd);
    c[bv3]  =0xB4000000u | (((uint32_t)(run-bv3)&0x7FFFFu)<<5) | 20u;     // CBZ X20, run
    c[bon]  =CBZ_W(9, run-bon);
    c[ba3]  =0xB4000000u | (((uint32_t)(run-ba3)&0x7FFFFu)<<5) | 11u;     // CBZ X11, run
    c[bheld]=CBZ_W(9, run-bheld);
    if (install_hook(fn, c, n)) LOGI("MOONJUMP-CC: move@0x%" PRIxPTR " hooked (v3+84 post-gravity)", (uintptr_t)fn);
    else                        LOGW("ccmove: install failed (left stock)");
}

// ============================================================================
//  INPUT capture — hook HsrPlayerInput_vf4 at loc_13F01B0 (X0 = the HsrPlayerInput component, which holds the
//  raw per-frame ButtonState array). Store it to cmd.hpi_addr so the worker can read the un-latched jump button
//  ("pressed" bytes at +0x50/+0xA0/+0xC2/+0xF2/+0x122/+0x152/+0x182/+0x1B2/+0x1E2) for a clean hold-to-fly gate.
//  The 4 reloc'd insns are reg-relative; we continue via X16 so the original's X9 (=*(X1+8)) survives to 0x13F01C0.
// ============================================================================
static void apply_hpi(uint8_t* loco, CmdBlock* cmd) {
    uint8_t* fn = loco - 0x18CCEA0 + 0x13F01B0;    // HsrPlayerInput_vf4 loc_13F01B0
    uintptr_t cont = (uintptr_t)fn + 16;           // 0x13F01C0
    uint32_t c[16]; int n = 0;
    int ldrcmd=n++;                      // LDR X10,[PC,#cmdq]
    c[n++]=0xF9004140u;                  // STR X0,[X10,#128]   cmd.hpi_addr = HsrPlayerInput
    c[n++]=0x79402028u;                  // LDRH W8,[X1,#0x10]   (reloc)
    c[n++]=0xF9400429u;                  // LDR X9,[X1,#8]
    c[n++]=0x5280004Au;                  // MOV W10,#2  (re-sets W10=2 as the original expects)
    c[n++]=0x79004008u;                  // STRH W8,[X0,#0x20]
    c[n++]=0x58000050u;                  // LDR X16,[PC,#8]   (X16, not X9 — preserve X9 for the original)
    c[n++]=0xD61F0200u;                  // BR X16 -> 0x13F01C0
    c[n++]=(uint32_t)(cont & 0xFFFFFFFFu); c[n++]=(uint32_t)((uint64_t)cont>>32);
    int cmdq=n; c[n++]=(uint32_t)((uintptr_t)cmd & 0xFFFFFFFFu); c[n++]=(uint32_t)((uint64_t)(uintptr_t)cmd>>32);
    c[ldrcmd]=LDR_XLIT(10, cmdq-ldrcmd);
    if (install_hook(fn, c, n)) LOGI("HPI: HsrPlayerInput_vf4@0x%" PRIxPTR " hooked (capture component)", (uintptr_t)fn);
    else                        LOGW("hpi: install failed (left stock)");
}

// ============================================================================
//  FEATURE 2 + 3 — locomotion detour: unlimited jump (default) + fly + player-ctl
//  POST-detour: run the ORIGINAL update, THEN (a) bake numAirJumps/jumpInitVel into
//  the config for jump/fly, and (b) overwrite the locomotion OUTPUT (a3+176 move
//  delta) for teleport/walk. config=*(a2+400); a2 saved in X19, a3 in X20.
// ============================================================================
static void apply_locomotion(uint8_t* lf, CmdBlock* cmd, bool wantJump, bool wantFly, float flyvel) {
    uintptr_t cont = (uintptr_t)lf + 16;
    uint32_t c[96]; int n = 0;
    // X19/X20 are callee-saved; the original update preserves them across its body, so we capture a2/a3
    // into X19/X20 before the BL and read them back after. The original prologue (relocated below) is what
    // pushes/restores X19/X20 — but since WE clobber them before that prologue runs, we save/restore X19/X20
    // ourselves on a small private frame here (X30 too, for our own RET).
    c[n++]=0xD10083FFu;                  // SUB SP,SP,#32   (32B frame: X30 + X19 + X20 must NOT overlap)
    c[n++]=0xA90053F3u;                  // STP X19,X20,[SP,#0]  X19@SP+0, X20@SP+8
    c[n++]=0xF9000BFEu;                  // STR X30,[SP,#16]  save real LR (was [SP,#8] -> X20 clobbered it = crash)
    c[n++]=0xAA0103F3u;                  // MOV X19,X1         X19 = a2 (controller state -> config)
    c[n++]=0xAA0203F4u;                  // MOV X20,X2         X20 = a3 (locomotion output)
    int bl=n++;                          // BL origcall (patched) — run the original update body
    // ---- post: config = *(a2+400) = *(X19+400); apply jump/fly each frame ----
    if (wantJump || wantFly) {
        c[n++]=0xF9400000u | (uint32_t)((400>>3)<<10) | (19u<<5) | 9u;  // LDR X9,[X19,#400]  config
        int cbzJ=n++;                    // CBZ X9, skip_cfg
        if (wantJump) { c[n++]=0x5284E1EAu; c[n++]=STR_W(10,9,44); }    // MOVZ W10,#9999 ; STR W10,[X9,#44]
        if (wantFly)  {
            uint32_t fvb; { float fv=flyvel; memcpy(&fvb,&fv,4); }
            c[n++]=0x52800000u | ((fvb & 0xFFFFu)<<5) | 11u;            // MOVZ W11,#flyvel_lo
            c[n++]=0x72A00000u | (((fvb>>16)&0xFFFFu)<<5) | 11u;        // MOVK W11,#flyvel_hi,LSL#16
            c[n++]=STR_W(11,9,32);                                      // STR W11,[X9,#32] jumpInitialVelocity
        }
        int skipJ=n; c[cbzJ]=CBZ_W(9, skipJ-cbzJ);
    }
    // ---- player-ctl: cmd is mmap'd; X10 = &cmd from an LDR-literal ----
    int ldr10=n++;                       // LDR X10,[PC,#cmdq] (patched)
    c[n++]=LDR_W(9,10,28); c[n++]=0x11000529u; c[n++]=STR_W(9,10,28); // cmd.tick++
    c[n++]=0xF9001954u;                  // STR X20,[X10,#48]   cmd.a3_addr = a3 (probe jump-button bytes)
    c[n++]=0xF940CA6Bu;                  // LDR X11,[X19,#400]  config = *(a2+400)
    c[n++]=0xF900294Bu;                  // STR X11,[X10,#80]   cmd.cfg_addr = config (probe numAirJumps/jumpInitVel)
    c[n++]=0xF940C26Bu;                  // LDR X11,[X19,#384]  v5 = *(a2+384)
    c[n++]=0xF940796Bu;                  // LDR X11,[X11,#240]  v10 = *(v5+240) = character controller
    c[n++]=0xF9002D4Bu;                  // STR X11,[X10,#88]   cmd.cc_addr = v10 (probe vy field)
    c[n++]=0xF9003153u;                  // STR X19,[X10,#96]   cmd.a2_addr = a2 (probe jump mode-gate flags)
    // SYNCHRONOUS a2 button probe (a2 is transient — read it here where X19=a2 is valid): pack 4 candidate
    // flag bytes into cmd.jbtn @124..127 = [a2+144, a2+160, a2+192, a2+208] to find which tracks the jump button.
    c[n++]=0x3902426Bu; c[n++]=0x3901F14Bu;   // LDRB W11,[X19,#144] ; STRB W11,[X10,#124]
    c[n++]=0x3902826Bu; c[n++]=0x3901F54Bu;   // LDRB W11,[X19,#160] ; STRB W11,[X10,#125]
    c[n++]=0x3903026Bu; c[n++]=0x3901F94Bu;   // LDRB W11,[X19,#192] ; STRB W11,[X10,#126]
    c[n++]=0x3903426Bu; c[n++]=0x3901FD4Bu;   // LDRB W11,[X19,#208] ; STRB W11,[X10,#127]
    c[n++]=LDR_W(11,20,144); c[n++]=STR_W(11,10,32);   // cmd.curpos[0] = a3+144 (live x)
    c[n++]=LDR_W(11,20,148); c[n++]=STR_W(11,10,36);   // cmd.curpos[1] = y
    c[n++]=LDR_W(11,20,152); c[n++]=STR_W(11,10,40);   // cmd.curpos[2] = z
    c[n++]=LDR_W(9,10,0);                // W9 = tp_pending
    int cbz=n++;                         // CBZ W9, skip
    // teleport: one-frame delta a3+176 = cmd.tp - a3+144(current) -> caller integrates -> instant (no fall)
    // (X10=&cmd, X20=a3). s12=cmd.tp.<axis>; s11=a3.<axis>(current); FSUB; store delta to a3+176/180/184.
    c[n++]=0xBD40054Cu; c[n++]=0xBD40928Bu; c[n++]=0x1E2B398Bu; c[n++]=0xBD00B28Bu; // x -> a3+176
    c[n++]=0xBD40094Cu; c[n++]=0xBD40968Bu; c[n++]=0x1E2B398Bu; c[n++]=0xBD00B68Bu; // y -> a3+180
    c[n++]=0xBD400D4Cu; c[n++]=0xBD409A8Bu; c[n++]=0x1E2B398Bu; c[n++]=0xBD00BA8Bu; // z -> a3+184
    c[n++]=STR_W(31,10,0);               // cmd.tp_pending = 0 (one-shot)
    int skip=n; c[cbz]=CBZ_W(9, skip-cbz);
    // walk: overwrite the OUTPUT per-frame move delta a3+176 the caller integrates
    c[n++]=LDR_W(9,10,16);               // W9 = walk_on
    int cbzw=n++;                        // CBZ W9, skip_walk
    c[n++]=LDR_W(11,10,20); c[n++]=STR_W(11,20,176);   // a3+176 delta.x = walk[0]
    c[n++]=STR_W(31,20,180);                           // a3+180 delta.y = 0
    c[n++]=LDR_W(11,10,24); c[n++]=STR_W(11,20,184);   // a3+184 delta.z = walk[1]
    int skipw=n; c[cbzw]=CBZ_W(9, skipw-cbzw);
    // ── MOONJUMP (post): pulse the game's OWN jump velocity. cc = *(*(a2+384)+240) = character controller;
    //    cc+76 = vertical velocity vy (probe-confirmed: +8 on jump, -10.97 falling). While the jump button
    //    a3+97 is held, set vy = jumpInitVel (config+32, =8.0) EVERY frame -> constant rise at the default
    //    jump strength; release -> we stop -> the game's gravity resumes. NO gravity/position hack. moon_on. ──
    c[n++]=LDR_W(9,10,56);               // W9 = cmd.moon_on
    int j1=n++;                          // CBZ W9, skip_moon
    c[n++]=0x39418689u;                  // LDRB W9,[X20,#97]   a3+97 (jump held)
    int j2=n++;                          // CBZ W9, skip_moon
    c[n++]=0xF940C26Bu;                  // LDR X11,[X19,#384]  v5 = *(a2+384)
    int j3=n++;                          // CBZ X11, skip_moon
    c[n++]=0xF940796Bu;                  // LDR X11,[X11,#240]  cc = *(v5+240) = character controller
    int j4=n++;                          // CBZ X11, skip_moon
    c[n++]=0xF940CA6Cu;                  // LDR X12,[X19,#400]  config = *(a2+400)
    int j5=n++;                          // CBZ X12, skip_moon
    c[n++]=0xB940218Du;                  // LDR W13,[X12,#32]   W13 = jumpInitVel (8.0)
    c[n++]=0xB9004D6Du;                  // STR W13,[X11,#76]   cc+76 (vy) = jumpInitVel  -> PULSE the jump
    int mSkip=n;
    c[j1]=CBZ_W(9, mSkip-j1);
    c[j2]=CBZ_W(9, mSkip-j2);
    c[j3]=0xB4000000u | (((uint32_t)(mSkip-j3)&0x7FFFFu)<<5) | 11u;   // CBZ X11, skip_moon
    c[j4]=0xB4000000u | (((uint32_t)(mSkip-j4)&0x7FFFFu)<<5) | 11u;   // CBZ X11, skip_moon
    c[j5]=0xB4000000u | (((uint32_t)(mSkip-j5)&0x7FFFFu)<<5) | 12u;   // CBZ X12, skip_moon
    c[n++]=0xF9400BFEu;                  // LDR X30,[SP,#16]    restore real LR
    c[n++]=0xA94053F3u;                  // LDP X19,X20,[SP,#0] restore X19,X20
    c[n++]=0x910083FFu;                  // ADD SP,SP,#32
    c[n++]=0xD65F03C0u;                  // RET -> the real caller
    int orig=n; c[bl]=0x94000000u | ((uint32_t)(orig-bl) & 0x3FFFFFFu);   // BL origcall
    memcpy(&c[n], lf, 16); n+=4;         // relocated prologue (4 SP-rel insns)
    c[n++]=0x58000049u;                  // LDR X9,[PC,#8]
    c[n++]=0xD61F0120u;                  // BR  X9 -> fn+16 (rest of original; its RET returns to 'post')
    c[n++]=(uint32_t)(cont&0xFFFFFFFFu); c[n++]=(uint32_t)((uint64_t)cont>>32);     // .quad fn+16
    int cmdq=n;
    c[n++]=(uint32_t)((uintptr_t)cmd&0xFFFFFFFFu); c[n++]=(uint32_t)((uint64_t)(uintptr_t)cmd>>32); // .quad cmd
    c[ldr10]=LDR_XLIT(10, cmdq-ldr10);
    if (install_hook(lf, c, n))
        LOGI("LOCOMOTION: update@0x%" PRIxPTR " hooked — unlimjump=%d fly=%d flyvel=%.1f player-ctl=armed",
             (uintptr_t)lf, wantJump?1:0, wantFly?1:0, flyvel);
    else
        LOGW("locomotion: install failed (left stock)");
}

// ============================================================================
//  FEATURE 5 — CRASHPROOF wrapper around AssetManager_initLoadedAsset.
//
//  We install a jump at the entry to our C++ wrapper cp_initLoadedAsset. The
//  wrapper BL-calls ORIG (a thunk that runs the relocated original prologue then
//  BRs to fn+16) inside a try/catch. Because the relocated prologue saves OUR
//  return address (x30, set by the BL) into the exact stack slot the original's
//  CFI describes, libshell's intact unwind tables let a std::length_error thrown
//  deep in a Meta initializer unwind across ORIG's frame into our catch. We log
//  + return 0xDB (low byte = (uint8_t)-25, the graceful "asset failed" code the
//  No-AssetInitializer path returns) so the env loads MINUS the bad mesh.
//
//  Signature: 10 args matching the IDA decompile (fastcall over x0..x7 + stack).
//  We only TOUCH a2 (**a2 = the asset obj; AssetId = 3 u64 @ +8) for logging; all
//  args are forwarded byte-identically to ORIG.
// ============================================================================
typedef uint64_t (*init_asset_t)(uint32_t, int64_t**, void**, int64_t,
                                 void*, size_t, int64_t, char, char, int64_t);
static init_asset_t g_orig_init_asset = nullptr;

// Walk the ARM64 frame-pointer chain from the live X29 and log every return
// address that lands inside libshell (= the throwing initializer + its callers).
static void log_libshell_backtrace() {
#if defined(__aarch64__)
    uintptr_t fp;
    __asm__ volatile("mov %0, x29" : "=r"(fp));   // our wrapper's frame pointer
    for (int i = 0; i < 16 && fp; ++i) {
        if (fp & 0xF) break;                                  // unaligned -> stop
        uintptr_t ret  = *(uintptr_t*)(fp + 8);
        uintptr_t next = *(uintptr_t*)(fp + 0);
        if (g_libLo && ret >= g_libLo && ret < g_libHi)
            LOGE("BT#%d libshell+0x%zx", i, (size_t)(ret - g_libLo));
        if (next <= fp) break;                                // chain must climb
        fp = next;
    }
#endif
}

extern "C" uint64_t cp_initLoadedAsset(uint32_t a1, int64_t** a2, void** a3, int64_t a4,
                                       void* src, size_t n, int64_t a7, char a8, char a9, int64_t a10) {
    try {
        return g_orig_init_asset(a1, a2, a3, a4, src, n, a7, a8, a9, a10);
    } catch (const std::exception& e) {
        uint64_t* asset = (a2 && *a2) ? (uint64_t*)**a2 : nullptr;   // **a2 = asset obj; AssetId = 3 u64 @ +8
        if (asset)
            LOGE("CRASHPROOF: asset-init threw '%s' -> SKIPPING. AssetId=%llx:%llx:%llx",
                 e.what(), (unsigned long long)asset[1], (unsigned long long)asset[2], (unsigned long long)asset[3]);
        else
            LOGE("CRASHPROOF: asset-init threw '%s' -> SKIPPING (no asset ptr)", e.what());
        log_libshell_backtrace();
        return 0xDBuLL;   // low byte = (uint8_t)-25 = graceful "asset failed"
    } catch (...) {
        LOGE("CRASHPROOF: asset-init threw UNKNOWN -> SKIPPING");
        log_libshell_backtrace();
        return 0xDBuLL;
    }
}

// Build the ORIG thunk (relocated prologue + BR fn+16) and redirect fn entry to
// cp_initLoadedAsset. Same mprotect/clear-cache mechanism as install_hook.
static void apply_crashproof(uint8_t* fn) {
    size_t PG = (size_t)sysconf(_SC_PAGESIZE);
    // ORIG thunk: 4 relocated prologue insns + LDR X16,[PC,#8] + BR X16 + .quad fn+16
    void* thunk = mmap(nullptr, PG, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (thunk == MAP_FAILED) { LOGW("crashproof: thunk mmap failed (left stock)"); return; }
    uint32_t t[8]; int tn = 0;
    memcpy(t, fn, 16); tn = 4;                   // relocated prologue (4 SP-rel insns, position-independent)
    t[tn++] = 0x58000050u;                       // LDR X16,[PC,#8]
    t[tn++] = 0xD61F0200u;                       // BR  X16
    uintptr_t cont = (uintptr_t)fn + 16;
    t[tn++] = (uint32_t)(cont & 0xFFFFFFFFu);
    t[tn++] = (uint32_t)((uint64_t)cont >> 32);
    memcpy(thunk, t, (size_t)tn * 4);
    mprotect(thunk, PG, PROT_READ|PROT_EXEC);
    __builtin___clear_cache((char*)thunk, (char*)thunk + tn * 4);
    g_orig_init_asset = (init_asset_t)thunk;

    // Overwrite fn[0..15] with an absolute jump to cp_initLoadedAsset.
    uintptr_t tw = (uintptr_t)&cp_initLoadedAsset;
    uint32_t jmp[4] = { 0x58000050u, 0xD61F0200u, (uint32_t)(tw & 0xFFFFFFFFu), (uint32_t)((uint64_t)tw >> 32) };
    uintptr_t a = (uintptr_t)fn & ~(uintptr_t)(PG-1);
    uintptr_t b = ((uintptr_t)fn + 16 + PG - 1) & ~(uintptr_t)(PG-1);
    if (mprotect((void*)a, b-a, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) { LOGW("crashproof: mprotect failed (left stock)"); return; }
    memcpy(fn, jmp, 16);
    mprotect((void*)a, b-a, PROT_READ|PROT_EXEC);
    __builtin___clear_cache((char*)fn, (char*)fn + 16);
    LOGI("CRASHPROOF: initLoadedAsset@0x%" PRIxPTR " wrapped (orig-thunk@%p) libshell[0x%" PRIxPTR "..0x%" PRIxPTR "]",
         (uintptr_t)fn, thunk, g_libLo, g_libHi);
}

// ============================================================================
//  FEATURE 3 — the GAME's own teleport: post a {x,y,z,yaw, type=35} message to
//  g_ShellApp's TypedMessageQueue with kind=98. g_ShellApp + TypedMessageQueue
//  are pattern-resolved; operator new via dlsym(_Znwm) (always exported in-process).
// ============================================================================
typedef void* (*op_new_t)(size_t);
typedef void  (*tmq_post_t)(void*, int*, void*);

static uint8_t*  g_shellApp_ptr = nullptr;     // &g_ShellApp (the global slot)
static op_new_t  g_op_new       = nullptr;
static tmq_post_t g_tmq_post    = nullptr;

static bool resolve_teleport() {
    uint8_t* tp = find_sig(kTpSig);
    if (!tp) { LOGW("teleport: nativeTeleportToCoordinates sig not found"); return false; }
    g_shellApp_ptr = resolve_adrp_global(tp + kTpAdrpOff);     // ADRP@tp+8, LDR@tp+12
    uint8_t* tmq = find_sig(kTmqSig);
    if (!tmq) { LOGW("teleport: TypedMessageQueue sig not found"); return false; }
    g_tmq_post = (tmq_post_t)tmq;
    g_op_new = (op_new_t)dlsym(RTLD_DEFAULT, "_Znwm");          // operator new(size_t)
    if (!g_op_new) { LOGW("teleport: dlsym(_Znwm) failed"); return false; }
    LOGI("teleport: &g_ShellApp@0x%" PRIxPTR " tmq@0x%" PRIxPTR " op_new@%p",
         (uintptr_t)g_shellApp_ptr, (uintptr_t)tmq, (void*)g_op_new);
    return true;
}
static void post_teleport(float x, float y, float z, float yaw) {
    if (!g_shellApp_ptr || !g_tmq_post || !g_op_new) return;
    uintptr_t app = *(volatile uintptr_t*)g_shellApp_ptr;       // g_ShellApp
    if (!app) return;
    void* queue = *(void**)(app + 16);                          // *(g_ShellApp+16)
    if (!queue) return;
    uint8_t* msg = (uint8_t*)g_op_new(128);
    if (!msg) return;
    memset(msg, 0, 128);
    *(float*)(msg+0)=x; *(float*)(msg+4)=y; *(float*)(msg+8)=z; *(float*)(msg+12)=yaw;
    *(int*)(msg+120)=35;                                        // message type
    int kind=98;
    g_tmq_post(queue, &kind, msg);                              // ShellApp consumes + frees msg
}

// ============================================================================
//  FEATURE 4 — verbose logs + debug-UI globals (pattern-resolved, no fixed offset)
// ============================================================================
static void apply_verbose() {
    uint8_t* fn = find_sig(kLogSig);
    if (!fn) { LOGW("verbose: Log_levelEnabled sig not found"); return; }
    uint8_t* g = resolve_adrp_global(fn + kLogAdrpOff);         // ADRP@fn+0x30, LDRB@fn+0x34
    wr8(g, 5);                                                  // logLevel byte 3 -> 5 (unhide DEBUG)
    LOGI("VERBOSE-LOGS: logLevel global@0x%" PRIxPTR " = 5", (uintptr_t)g);
}
static void apply_debugui() {
    uint8_t* fn = find_sig(kImguiSig);
    if (!fn) { LOGW("debugui: ImGuiRenderSystem_register sig not found"); return; }
    uint8_t* anchor = resolve_adrp_global(fn + kImguiAdrpOff);  // debugui.enabled global (0x29C59D0)
    wr32(anchor + kDuEnabledD + 8, 1);                          // debugui.enabled
    wr32(anchor + kDuServerD  + 8, 1);                          // debugui.serverEnabled
    wr32(anchor + kDuConnectD + 8, 1);                          // debugui.connectOnStart
    wr32(anchor + kDuPortD    + 8, 8888);                       // debugui.localPort
    LOGI("DEBUG-UI: anchor@0x%" PRIxPTR " enabled/server/connect=1 localPort=8888", (uintptr_t)anchor);
}

// FEATURE 5: install the crashproof wrapper NOW (synchronous). The asset-init
// dispatch is present the moment libshell is mapped, and the throwing env-load on
// HSR:Async happens within ~1s of the fork — so we MUST hook before returning to
// the app, ahead of any worker-thread scheduling delay. Returns true if hooked (or
// already hooked). Bounded short retry in case libshell isn't fully mapped yet.
static bool g_cp_done = false;
static bool install_crashproof_now() {
    if (g_cp_done) return true;
    for (int i = 0; i < 40; ++i) {                // ~2s max; normally hits first try
        uint8_t* af = find_init_asset();
        if (af) {
            if (*(volatile uint32_t*)af == 0x58000050u) { LOGW("crashproof already hooked — skipping"); }
            else { capture_lib_range(); apply_crashproof(af); }
            g_cp_done = true; return true;
        }
        usleep(50 * 1000);
    }
    return false;
}

// MOONJUMP high-frequency velocity writer: the locomotion hook writes vy (cc+76) but the character
// controller's physics step runs AFTER it and overwrites the value. This thread re-writes vy = jumpInitVel
// ~600Hz while the jump button (a3+97) is held, so writes land AFTER the physics step too -> velocity holds
// -> continuous rise at the game's own jump strength. cc/a3/cfg pointers are refreshed each frame by the hook.
static CmdBlock* g_moon_cmd = nullptr;
static void* moon_pulse_thread(void*) {
    LOGI("moonjump pulse thread started");
    for (;;) {
        CmdBlock* cmd = g_moon_cmd;
        if (cmd && cmd->moon_on && cmd->cc_addr && cmd->a3_addr
            && *(volatile uint8_t*)(cmd->a3_addr + 97)) {            // jump held
            float jiv = cmd->cfg_addr ? *(volatile float*)(cmd->cfg_addr + 32) : 8.0f;
            *(volatile float*)(cmd->cc_addr + 76) = jiv;             // vy = jumpInitVel
        }
        usleep(1500);   // ~666 Hz
    }
    return nullptr;
}

// ============================================================================
//  Worker — runs in com.oculus.vrshell after specialize.
// ============================================================================
static void* worker(void*) {
    LOGI("worker started — scanning embedded libshell for patch sites");

    bool wantJump = prop_on("unlimjump", true);       // FEATURE 2: ON by default
    bool wantFly  = prop_on("fly", false);
    float flyvel  = prop_f("flyvel", 8.0f, 0.1f, 200.0f);
    bool wantFar  = prop_on("farclip_on", true);      // FEATURE 1: ON by default
    float farVal  = prop_f("farclip", 150000.0f, 1000.0f, 1.0e9f);
    bool wantCP   = prop_on("crashproof", true);      // FEATURE 5: ON by default

    // command block for player-ctl (independent mmap page — never module memory)
    size_t PG = (size_t)sysconf(_SC_PAGESIZE);
    CmdBlock* cmd = (CmdBlock*)mmap(nullptr, PG, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (cmd == MAP_FAILED) cmd = nullptr; else memset((void*)cmd, 0, sizeof *cmd);
    (void)moon_pulse_thread; (void)g_moon_cmd;   // superseded by apply_ccmove (real motion-point hook)
    if (cmd) { cmd->moon_on = prop_on("moonjump", true) ? 1u : 0u;             // MOONJUMP on by default
               cmd->moon_rise = prop_f("moonrise", 0.050f, 0.0f, 10.0f);        // per-frame vertical lift (user-tuned natural climb; hsr.moonrise to adjust)
               cmd->moon_hold = (uint32_t)prop_f("moonhold", 8.0f, 0.0f, 600.0f); } // tap-window frames before fly kicks in

    bool doneFar = !wantFar, doneLoco = false, doneCP = !wantCP || g_cp_done;   // g_cp_done if hooked synchronously in postAppSpecialize
    for (int i = 0; i < 24000 && (!doneFar || !doneLoco || !doneCP); ++i) {
        // FEATURE 5 first: the asset-init dispatch exists from process start; hook it
        // BEFORE the env's HSR:Async loads assets so the bad-mesh throw is caught.
        if (!doneCP)   { uint8_t* af = find_init_asset(); if (af) {
            if (*(volatile uint32_t*)af == 0x58000050u) LOGW("crashproof already hooked — skipping");
            else { capture_lib_range(); apply_crashproof(af); }
            doneCP = true; } }
        if (!doneFar)  { uint8_t* fn = find_far();  if (fn) { apply_farclip(fn, farVal); doneFar = true; } }
        if (!doneLoco) { uint8_t* lf = find_loco(); if (lf) {
            if (*(volatile uint32_t*)lf == 0x58000050u) LOGW("loco already hooked — skipping");
            else if (cmd)                             { apply_locomotion(lf, cmd, wantJump, wantFly, flyvel);
                                                        apply_ccmove(lf, cmd); apply_hpi(lf, cmd); }   // motion hook + raw input capture
            else                                        LOGW("loco: cmd mmap failed — skipping");
            doneLoco = true; } }
        if (doneFar && doneLoco && doneCP) break;
        usleep(50 * 1000);
        if (i == 400) LOGI("waiting for VrShell render/locomotion code (env not up yet)…");
    }
    if (!doneFar)  LOGW("far-clip site not found in ~20min — left stock");
    if (!doneLoco) LOGW("locomotion site not found in ~20min — left stock");
    if (!doneCP)   LOGW("crashproof site not found in ~20min — left stock");

    // FEATURE 4 toggles (off by default).
    if (prop_on("verbose", false)) apply_verbose();
    if (prop_on("debugui", false)) apply_debugui();

    // FEATURE 3 — resolve the teleport plumbing and serve player-ctl prop commands.
    bool tpOk = resolve_teleport();
    char buf[PROP_VALUE_MAX]; int li=0; float wtgt[3]={0,0,0}; bool wact=false;
    for (;;) {
        if (!cmd) { usleep(500*1000); continue; }   // no command block -> nothing to drive
        if (tpOk) {
            buf[0]=0; if (__system_property_get("hsr.tp", buf)>0 && buf[0]) {
                float x,y,z,yaw=0.f;
                if (sscanf(buf,"%f %f %f %f",&x,&y,&z,&yaw)>=3) {
                    post_teleport(x,y,z,yaw); __system_property_set("hsr.tp","");
                    LOGI("teleport -> %.2f %.2f %.2f yaw %.2f", x,y,z,yaw);
                }
            }
            buf[0]=0; if (__system_property_get("hsr.rot", buf)>0 && buf[0]) {   // rotate = tp to curpos + yaw
                float yaw;
                if (cmd->tick!=0 && sscanf(buf,"%f",&yaw)==1) {
                    post_teleport(cmd->curpos[0],cmd->curpos[1],cmd->curpos[2],yaw);
                    __system_property_set("hsr.rot","");
                    LOGI("rotate -> yaw %.2f", yaw);
                }
            }
        }
        buf[0]=0; if (__system_property_get("hsr.walk", buf)>0) {
            if (!buf[0]) cmd->walk_on=0;
            else { float a,b; if (sscanf(buf,"%f %f",&a,&b)==2){ cmd->walk[0]=a; cmd->walk[1]=b; cmd->walk_on=(a||b)?1:0; } }
        }
        buf[0]=0; if (__system_property_get("hsr.moonrise", buf)>0 && buf[0]) { float r; if (sscanf(buf,"%f",&r)==1) cmd->moon_rise=r; }
        buf[0]=0; if (__system_property_get("hsr.moonhold", buf)>0 && buf[0]) { int h; if (sscanf(buf,"%d",&h)==1 && h>0) cmd->moon_hold=(uint32_t)h; }
        buf[0]=0; if (__system_property_get("hsr.moonjump", buf)>0 && buf[0]) cmd->moon_on = (buf[0]=='0')?0u:1u;
        // walk = accumulate a virtual target from the start pos + delta, teleport ~2.5 Hz (throttled)
        if (tpOk && cmd->walk_on && cmd->tick>0) {
            if (!wact){ wtgt[0]=cmd->curpos[0]; wtgt[1]=cmd->curpos[1]; wtgt[2]=cmd->curpos[2]; wact=true; }
            if ((li % 6)==0){ wtgt[0]+=cmd->walk[0]; wtgt[2]+=cmd->walk[1]; post_teleport(wtgt[0],wtgt[1],wtgt[2],0.f); }
        } else wact=false;
        // JUMP PROBE: log the jump-button byte region whenever it changes, to find the HELD flag offset.
        if (cmd->a3_addr) {
            volatile uint8_t* a3 = (volatile uint8_t*)cmd->a3_addr;
            static uint8_t l97 = 255, l100 = 255;
            uint8_t b97 = a3[97], b100 = a3[100];
            if (b97 != l97 || b100 != l100) {
                float* md = (float*)(a3 + 176);
                uint32_t naj=0; float jiv=0, thr=0;
                if (cmd->cfg_addr) { volatile uint8_t* cf=(volatile uint8_t*)cmd->cfg_addr;
                    naj=*(volatile uint32_t*)(cf+44); jiv=*(volatile float*)(cf+32); thr=*(volatile float*)(cf+36); }
                LOGI("JUMPDBG btn97=%u jthis99=%u jumped100=%u | numAirJumps=%u jumpInitVel=%.2f thresh=%.2f",
                     a3[97],a3[99],a3[100], naj,jiv,thr);
                LOGI("PCDBG v3+84 accumulator.y (pre-add) = %.4f  [moon_on=%u moon_rise=%.3f]",
                     cmd->pc_y32, cmd->moon_on, cmd->moon_rise);
                l97 = b97; l100 = b100;
            }
            if ((li % 6) == 0 && cmd->hpi_addr) {   // dump HsrPlayerInput button "pressed" bytes to find JUMP
                uint8_t* h = (uint8_t*)cmd->hpi_addr;
                LOGI("HPIBTN 50=%u A0=%u C2=%u F2=%u 122=%u 152=%u 182=%u 1B2=%u 1E2=%u",
                     h[0x50],h[0xA0],h[0xC2],h[0xF2],h[0x122],h[0x152],h[0x182],h[0x1B2],h[0x1E2]);
            }
        }
        ++li; usleep(66*1000);
    }
    return nullptr;
}

// ============================================================================
//  Zygisk module entry — target com.oculus.vrshell, spawn the worker.
// ============================================================================
class QuestCtlAIO : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override { api_ = api; env_ = env; }
    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        const char* nice = env_->GetStringUTFChars(args->nice_name, nullptr);
        target_ = nice && strncmp(nice, "com.oculus.vrshell", 18) == 0;  // prefix: inject EVERY vrshell + vrshell:child fork
        if (nice) env_->ReleaseStringUTFChars(args->nice_name, nice);
        if (!target_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
        if (!target_) return;
        // FEATURE 5 SYNCHRONOUS: hook the asset-init dispatch HERE — before control
        // returns to the app and before the HSR:Async thread starts loading the env —
        // so the bad-mesh throw is already wrapped (a worker-thread race could miss the
        // first async load). The worker re-checks as a fallback (sees the existing hook).
        if (prop_on("crashproof", true)) install_crashproof_now();
        pthread_t t; pthread_create(&t, nullptr, worker, nullptr); pthread_detach(t);
    }
private:
    zygisk::Api* api_ = nullptr; JNIEnv* env_ = nullptr; bool target_ = false;
};

REGISTER_ZYGISK_MODULE(QuestCtlAIO)

// ============================================================================
//  DT_NEEDED COMPANION MODE — the patch is delivered as a .so that libshell
//  REQUIRES (added to libshell.so's DT_NEEDED via LIEF; both shipped in the
//  overlay-mounted VrShell.apk). The dynamic linker loads THIS .so when it
//  loads libshell — inside vrshell, before libshell's own constructors — and
//  this ctor spawns the same worker that scans /proc/self/maps for libshell and
//  installs every hook (far-clip / unlim-jump / loco / crashproof). No Zygisk
//  injection, no static cave: the dependency .so patches the runtime libshell.
//  Gated to the vrshell process so it's inert if ever dlopened anywhere else.
// ============================================================================
static bool aio_in_vrshell() {
    char buf[256] = {0};
    FILE* f = fopen("/proc/self/cmdline", "re");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); (void)n;
    return strstr(buf, "com.oculus.vrshell") != nullptr;
}
__attribute__((constructor))
static void aio_companion_ctor() {
    static bool started = false;
    if (started) return;
    started = true;
    if (!aio_in_vrshell()) return;   // only act inside the vrshell host process
    LOGI("DT_NEEDED companion ctor — libshell loaded us; spawning patch worker");
    pthread_t t; pthread_create(&t, nullptr, worker, nullptr); pthread_detach(t);
}
