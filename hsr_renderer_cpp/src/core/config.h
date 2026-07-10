// ── config.h — the editor/cooker CONFIG (replaces the user-facing HSR_ env flags) ──────────────────────────
// "Everything smarter, config-based, no HSR_ flags": a tiny key=value config file loaded next to the exe, with
// SMART AUTO-DETECTION of everything it can find (Android build-tools, debug keystore, a shell APK, the out dir).
// Normal use — load an env, edit, Cook + auto-sign — needs ZERO environment variables. The editor's Cook panel
// is the editing surface; values persist to hsr_editor.cfg. (Deep RE/debug toggles stay as dev-only env reads.)
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>     // _NSGetExecutablePath
  #include <unistd.h>
#else
  #include <unistd.h>          // readlink(/proc/self/exe)
#endif

struct AppConfig {
    // ── cook / sign ──
    std::string cookShellApk;     // the V203 shell APK the port splices into (Nuxd.apk)
    std::string cookOutDir = "cooker/out";
    std::string cookPackage = "com.environment.outerwilds";       // the cooked APK's own package
    bool        autoSign   = true;
    bool        emitSpoof  = true;                                 // also emit a haven2025-spoof for unrooted users
    std::string buildTools;       // Android build-tools dir (has zipalign.exe + apksigner.bat)
    std::string keystore;         // signing keystore
    std::string keyAlias = "myhome";
    std::string keyPass  = "android";
    // ── app ──
    float       uiScale  = 0.f;   // 0 = auto (monitor content scale)
    bool        audio    = true;
    int         lang     = 0;     // UI language (i18n::Lang: 0=English, 1=Chinese) — GitHub #10

    // The directory the EXE lives in (set by the app at startup). The build-tools AND the auto-generated debug
    // keystore can live right next to the exe, so a machine with NO Android SDK installed just needs the signing
    // tools dropped beside the exe — no env vars, no SDK install. exeRel() resolves a path relative to it.
    static inline std::string s_exeDir;
    static std::string exeRel(const std::string& rel) { return s_exeDir.empty() ? rel : (s_exeDir + "/" + rel); }

    // Full path of the running executable — portable (Win32 / macOS / Linux). "" if the OS can't say.
    static std::string exePath() {
#ifdef _WIN32
        char buf[1024] = {0}; DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
        return (n > 0 && n < sizeof buf) ? std::string(buf, n) : std::string();
#elif defined(__APPLE__)
        char buf[1024] = {0}; uint32_t n = sizeof buf;
        if (_NSGetExecutablePath(buf, &n) != 0) return "";
        std::error_code ec; auto canon = std::filesystem::canonical(buf, ec);
        return ec ? std::string(buf) : canon.string();
#else
        char buf[1024] = {0}; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        return n > 0 ? std::string(buf, (size_t)n) : std::string();
#endif
    }
    // Launch a program DETACHED with one argument (the editor uses it to spawn a fresh instance on an
    // APK/glTF). Win32: CreateProcess (inherits env). POSIX: shell background spawn (env inherits too).
    static bool launchDetached(const std::string& exe, const std::string& arg) {
        if (exe.empty()) return false;
#ifdef _WIN32
        std::string cmd = "\"" + exe + "\" \"" + arg + "\"";
        std::vector<char> cmdv(cmd.begin(), cmd.end()); cmdv.push_back(0);
        STARTUPINFOA si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        if (!CreateProcessA(exe.c_str(), cmdv.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return false;
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return true;
#else
        std::string cmd = "\"" + exe + "\" \"" + arg + "\" >/dev/null 2>&1 &";
        return std::system(cmd.c_str()) == 0;
#endif
    }

    // Scan the usual Android SDK locations for the NEWEST build-tools dir that has apksigner.
    static std::string detectBuildTools() {
        namespace fs = std::filesystem; std::error_code ec0; std::vector<std::string> roots;
        // 0) apksigner dropped DIRECTLY beside the exe -> that dir IS the build-tools dir.
        if (!s_exeDir.empty() && (fs::exists(fs::path(s_exeDir)/"apksigner.bat", ec0) || fs::exists(fs::path(s_exeDir)/"apksigner", ec0))) return s_exeDir;
        auto add = [&](const char* e){ if (const char* v = std::getenv(e)) roots.push_back(std::string(v) + "/build-tools"); };
        add("ANDROID_HOME"); add("ANDROID_SDK_ROOT");
        if (!s_exeDir.empty()) roots.push_back(s_exeDir + "/build-tools");   // a build-tools/ folder beside the exe
        if (const char* la = std::getenv("LOCALAPPDATA")) roots.push_back(std::string(la) + "/Android/Sdk/build-tools");
        if (const char* h = std::getenv("HOME")) {   // Linux / macOS default SDK locations
            roots.push_back(std::string(h) + "/Android/Sdk/build-tools");
            roots.push_back(std::string(h) + "/Library/Android/sdk/build-tools");
        }
        roots.push_back("C:/Android/build-tools");
        roots.push_back("C:/Android/Sdk/build-tools");
        std::string best, bestVer;
        for (auto& r : roots) {
            std::error_code ec; if (!fs::is_directory(r, ec)) continue;
            for (auto& e : fs::directory_iterator(r, ec)) {
                if (!e.is_directory()) continue;
                std::string ver = e.path().filename().string();
                if (!fs::exists(e.path() / "apksigner.bat", ec) && !fs::exists(e.path() / "apksigner", ec)) continue;   // Win .bat OR POSIX
                if (ver > bestVer) { bestVer = ver; best = e.path().string(); }   // lexical max ~ newest (e.g. 37 > 34)
            }
            if (!best.empty()) break;
        }
        return best;
    }
    static std::string firstExisting(std::initializer_list<const char*> cands) {
        namespace fs = std::filesystem; std::error_code ec;
        for (auto c : cands) if (fs::exists(c, ec)) return c;
        return "";
    }

    // Fill any empty field with a smart default.
    void autoDetect() {
        if (buildTools.empty())   buildTools  = detectBuildTools();
        if (keystore.empty())     keystore    = firstExisting({ "cooker/debug.keystore", "debug.keystore" });
        // cookShellApk is legacy: the Nuxd donor + manifest are EMBEDDED in the binary now (the cook is standalone),
        // so no Nuxd.apk needs to exist on disk. Left empty unless the user explicitly points at one (HSR_COOK_SHELL).
        // (Previously this probed a repo-relative dev path "Envs To check/..." that doesn't exist for end users.)
        std::error_code ec; std::filesystem::create_directories(cookOutDir, ec);
    }

    // ── tiny key=value persistence ──
    bool load(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string s(line); auto h = s.find('#'); if (h != std::string::npos) s = s.substr(0, h);
            auto eq = s.find('='); if (eq == std::string::npos) continue;
            std::string k = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
            set(k, v);
        }
        fclose(f); return true;
    }
    void save(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "wb"); if (!f) return;
        fprintf(f, "# HSR editor config (auto-saved). Delete to reset to auto-detected defaults.\n");
        fprintf(f, "cookShellApk = %s\n", cookShellApk.c_str());
        fprintf(f, "cookOutDir   = %s\n", cookOutDir.c_str());
        fprintf(f, "cookPackage  = %s\n", cookPackage.c_str());
        fprintf(f, "autoSign     = %d\n", autoSign ? 1 : 0);
        fprintf(f, "emitSpoof    = %d\n", emitSpoof ? 1 : 0);
        fprintf(f, "buildTools   = %s\n", buildTools.c_str());
        fprintf(f, "keystore     = %s\n", keystore.c_str());
        fprintf(f, "keyAlias     = %s\n", keyAlias.c_str());
        fprintf(f, "keyPass      = %s\n", keyPass.c_str());
        fprintf(f, "uiScale      = %g\n", uiScale);
        fprintf(f, "audio        = %d\n", audio ? 1 : 0);
        fprintf(f, "lang         = %d\n", lang);
        fclose(f);
    }

private:
    static std::string trim(std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n");
        return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    }
    void set(const std::string& k, const std::string& v) {
        if      (k == "cookShellApk") cookShellApk = v;
        else if (k == "cookOutDir")   cookOutDir = v;
        else if (k == "cookPackage")  cookPackage = v;
        else if (k == "autoSign")     autoSign = (v != "0");
        else if (k == "emitSpoof")    emitSpoof = (v != "0");
        else if (k == "buildTools")   buildTools = v;
        else if (k == "keystore")     keystore = v;
        else if (k == "keyAlias")     keyAlias = v;
        else if (k == "keyPass")      keyPass = v;
        else if (k == "uiScale")      uiScale = (float)atof(v.c_str());
        else if (k == "audio")        audio = (v != "0");
        else if (k == "lang")         lang = atoi(v.c_str());
    }
};
