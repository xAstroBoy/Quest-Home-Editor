// ── glb2apk.h — a dropped .glb/.gltf model -> a Quest Home *source* APK ────────────────────────────
// The V79 loader only speaks "environment APK": every loader keys on assets/scene.zip. A raw model is
// not one, so a dropped .glb/.gltf is repacked here into the exact nesting the loader expects, and the
// drop then continues down the NORMAL APK path as if that APK had been dropped in the first place:
//
//   <out>.apk  (zip)
//     assets/scene.zip                (zip, STORED)
//       _WORLD_MODEL.gltf.ovrscene    (zip, STORED)
//         V9.gltf                     glTF JSON
//         V9.bin                      the single repacked geometry buffer
//         tex<i>.png / tex<i>.jpg     textures, referenced by uri
//       _BACKGROUND_LOOP.<ext>        optional ambient audio loop
//
// This runs BEFORE the loaders and produces what they then consume (glb2apk -> scene_loader ->
// gltf_loader), which is why it lives in io/ next to gltf_export/gltf_import (file in, file out) and
// not in loaders/ (bytes in, in-memory scene out).
//
// What it fixes on the way through:
//   * Wrong-UV black meshes — the loader samples the base texture with TEXCOORD_0 ONLY. A model that
//     binds it to another UV set (baseColorTexture.texCoord = 3/7/...) renders black, so each
//     primitive's REAL UV set is folded into TEXCOORD_0.
//   * Embedded textures — the loader only decodes images that have a `uri`, so GLB-embedded images are
//     externalized to real PNG/JPEG files inside the .ovrscene.
//   * The buffer is repacked into one V9.bin (node hierarchy, skins and animations are preserved).
//
// Texture policy: PNG and JPEG only. Anything else (WebP, KTX/ASTC, GIF, BMP, DDS) is a hard error
// rather than a silently blank texture — re-export the model with PNG/JPEG textures.
//
// Header-only, and needs only tinyjson + miniz (both already linked by the app), so the converter
// compiles INTO the exe: no interpreter, no external tool, no side files to ship.
#pragma once
#include "../core/tinyjson.h"
#include "miniz.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace glb2apk {

using u8 = uint8_t;
namespace fs = std::filesystem;

struct Options { std::string input, output, audio; };
struct Result  { size_t meshes = 0; int textures = 0, uvFixes = 0; bool audio = false; size_t bytes = 0; };

// ── helpers ─────────────────────────────────────────────────────────────────
inline std::vector<u8> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline bool writeFile(const std::string& p, const u8* d, size_t n) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write((const char*)d, (std::streamsize)n);
    return (bool)f;
}
[[noreturn]] inline void fail(const std::string& m) { throw std::runtime_error(m); }
inline std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
inline bool endsWithCI(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && lower(s.substr(s.size() - suf.size())) == lower(suf);
}
inline int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62; if (c == '/') return 63; return -1;
}
inline std::vector<u8> b64decode(const std::string& s) {
    std::vector<u8> o; int val = 0, bits = -8;
    for (char c : s) { if (c == '=') break; int d = b64v(c); if (d < 0) continue;
        val = (val << 6) + d; bits += 6; if (bits >= 0) { o.push_back((u8)((val >> bits) & 0xFF)); bits -= 8; } }
    return o;
}
inline std::string sniffImage(const std::vector<u8>& b) {
    auto has = [&](std::initializer_list<int> sig, size_t off = 0) {
        if (b.size() < off + sig.size()) return false;
        size_t i = off; for (int v : sig) { if (b[i++] != (u8)v) return false; } return true;
    };
    if (has({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return "png";
    if (has({0xFF, 0xD8, 0xFF})) return "jpg";
    if (b.size() >= 12 && memcmp(b.data(), "RIFF", 4) == 0 && memcmp(b.data() + 8, "WEBP", 4) == 0) return "webp";
    if (has({0xAB, 'K', 'T', 'X', ' ', '1', '1'})) return "ktx";
    if (b.size() >= 12 && memcmp(b.data() + 4, "KTX 20", 6) == 0) return "ktx2";
    if (has({'G', 'I', 'F', '8'})) return "gif";
    if (has({'B', 'M'})) return "bmp";
    if (has({'D', 'D', 'S', ' '})) return "dds";
    return "unknown";
}

struct Gltf { tinyjson::Value root; std::vector<std::vector<u8>> buffers; std::string dir; };

// Parse a .glb (12-byte header + 4-byte-aligned JSON/BIN chunks) or a plain .gltf.
inline void loadContainer(const std::string& path, Gltf& g) {
    std::vector<u8> raw = readFile(path);
    if (raw.empty()) fail("cannot read input file: " + path);
    g.dir = fs::path(path).parent_path().string();
    std::string json; std::vector<u8> glbBin;
    bool isGlb = raw.size() > 12 && raw[0] == 'g' && raw[1] == 'l' && raw[2] == 'T' && raw[3] == 'F';
    if (isGlb) {
        size_t off = 12;
        while (off + 8 <= raw.size()) {
            uint32_t len, typ; memcpy(&len, &raw[off], 4); memcpy(&typ, &raw[off + 4], 4); off += 8;
            if (off + len > raw.size()) break;
            if (typ == 0x4E4F534A)      json.assign((char*)&raw[off], len);                              // 'JSON'
            else if (typ == 0x004E4942) glbBin.assign(raw.begin() + off, raw.begin() + off + len);       // 'BIN'
            off += len + ((len % 4) ? (4 - len % 4) : 0);
        }
        if (json.empty()) fail("GLB has no JSON chunk");
    } else json.assign((char*)raw.data(), raw.size());

    try { g.root = tinyjson::parse(json); } catch (...) { fail("failed to parse glTF JSON"); }
    if (!g.root.has("meshes")) fail("glTF has no meshes");

    if (g.root.has("buffers")) {
        const auto& B = g.root["buffers"];
        for (size_t i = 0; i < B.size(); ++i) {
            std::string uri = B[i].has("uri") ? B[i]["uri"].asString() : "";
            if (uri.empty()) {
                if (isGlb && i == 0) g.buffers.push_back(glbBin);
                else fail("buffer[" + std::to_string(i) + "] has no uri and is not the GLB binary chunk");
            } else if (uri.rfind("data:", 0) == 0) {
                size_t c = uri.find("base64,");
                g.buffers.push_back(c != std::string::npos ? b64decode(uri.substr(c + 7)) : std::vector<u8>());
            } else {
                std::vector<u8> b = readFile((fs::path(g.dir) / uri).string());
                if (b.empty()) fail("cannot read external buffer: " + uri);
                g.buffers.push_back(std::move(b));
            }
        }
    }
}
inline std::vector<u8> imageBytes(Gltf& g, const tinyjson::Value& im, int& srcBufferView) {
    srcBufferView = -1;
    if (im.has("uri")) {
        std::string uri = im["uri"].asString();
        if (uri.rfind("data:", 0) == 0) { size_t c = uri.find("base64,");
            return c != std::string::npos ? b64decode(uri.substr(c + 7)) : std::vector<u8>(); }
        return readFile((fs::path(g.dir) / uri).string());
    }
    if (im.has("bufferView")) {
        int bvi = (int)im["bufferView"].asInt(); srcBufferView = bvi;
        const auto& BV = g.root["bufferViews"][(size_t)bvi];
        int buf = (int)BV["buffer"].asInt();
        size_t off = BV.has("byteOffset") ? (size_t)BV["byteOffset"].asInt() : 0;
        size_t len = (size_t)BV["byteLength"].asInt();
        if (buf < 0 || buf >= (int)g.buffers.size() || off + len > g.buffers[buf].size())
            fail("image bufferView out of range");
        return std::vector<u8>(g.buffers[buf].begin() + off, g.buffers[buf].begin() + off + len);
    }
    return {};
}

inline void jsonEscape(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break; case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break; case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break; case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default: if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; } else out += (char)c;
        }
    }
    out += '"';
}
inline void serialize(const tinyjson::Value& v, std::string& out) {
    using T = tinyjson::Type;
    switch (v.type) {
        case T::Null:   out += "null"; break;
        case T::Bool:   out += v.boolVal ? "true" : "false"; break;
        case T::Int:    out += std::to_string(v.intVal); break;
        case T::Float: { char b[32]; snprintf(b, sizeof b, "%.9g", v.floatVal); out += b; break; }
        case T::String: jsonEscape(v.strVal, out); break;
        case T::Array:  { out += '['; for (size_t i = 0; i < v.arrVal.size(); ++i) { if (i) out += ','; serialize(v.arrVal[i], out); } out += ']'; break; }
        case T::Object: { out += '{'; bool first = true; for (const auto& kv : v.objVal) { if (!first) out += ','; first = false; jsonEscape(kv.first, out); out += ':'; serialize(kv.second, out); } out += '}'; break; }
    }
}
inline void remapBufferViews(tinyjson::Value& v, const std::vector<int>& map) {
    if (v.isObject()) {
        for (auto& kv : v.objVal) {
            if (kv.first == "bufferView" && kv.second.isInt()) {
                int old = (int)kv.second.intVal;
                if (old < 0 || old >= (int)map.size() || map[old] < 0) fail("accessor references a texture bufferView (unexpected)");
                kv.second.intVal = map[old];
            } else remapBufferViews(kv.second, map);
        }
    } else if (v.isArray()) for (auto& e : v.arrVal) remapBufferViews(e, map);
}
inline tinyjson::Value vInt(int64_t i)            { tinyjson::Value v(tinyjson::Type::Int);    v.intVal = i; return v; }
inline tinyjson::Value vStr(const std::string& s) { tinyjson::Value v(tinyjson::Type::String); v.strVal = s; return v; }

struct ZipEntry { std::string name; std::vector<u8> data; bool store; };
inline std::vector<u8> buildZip(const std::vector<ZipEntry>& entries) {
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_heap(&z, 0, 0)) fail("zip writer init failed");
    for (const auto& e : entries) {
        mz_uint flags = e.store ? (mz_uint)MZ_NO_COMPRESSION : (mz_uint)MZ_DEFAULT_COMPRESSION;
        if (!mz_zip_writer_add_mem(&z, e.name.c_str(), e.data.data(), e.data.size(), flags)) fail("zip add failed: " + e.name);
    }
    void* buf = nullptr; size_t sz = 0;
    if (!mz_zip_writer_finalize_heap_archive(&z, &buf, &sz)) fail("zip finalize failed");
    std::vector<u8> out((u8*)buf, (u8*)buf + sz);
    mz_zip_writer_end(&z); mz_free(buf);
    return out;
}

// ── the conversion ──────────────────────────────────────────────────────────
// Throws std::runtime_error(message) on any failure — UI code should call convertForDrop() instead.
inline Result convert(const Options& opt) {
    if (opt.input.empty()) fail("no input file");
    if (!endsWithCI(opt.input, ".glb") && !endsWithCI(opt.input, ".gltf")) fail("input must be .glb or .gltf");
    std::string out = opt.output.empty() ? (fs::path(opt.input).stem().string() + ".apk") : opt.output;

    Gltf g; loadContainer(opt.input, g);
    Result R;

    // 1. Externalize every image to a real PNG/JPEG file inside the .ovrscene.
    std::vector<ZipEntry> ovr; std::set<int> imageBVs;
    if (g.root.has("images")) {
        auto& imgs = g.root["images"];
        for (size_t i = 0; i < imgs.size(); ++i) {
            int srcBV = -1;
            std::vector<u8> bytes = imageBytes(g, imgs[i], srcBV);
            if (bytes.empty()) fail("image[" + std::to_string(i) + "] has no readable data");
            std::string fmt = sniffImage(bytes);
            if (fmt != "png" && fmt != "jpg")
                fail("unsupported texture format in image[" + std::to_string(i) + "]: " + fmt +
                     " - only PNG and JPEG are supported. Re-export the model with PNG/JPEG textures.");
            std::string fname = "tex" + std::to_string(i) + (fmt == "png" ? ".png" : ".jpg");
            ovr.push_back({ fname, std::move(bytes), true });
            if (srcBV >= 0) imageBVs.insert(srcBV);
            tinyjson::Value im(tinyjson::Type::Object); im.objVal["uri"] = vStr(fname);
            imgs[i] = std::move(im); R.textures++;
        }
    }

    // 2. Repack every non-image bufferView into one 4-byte-aligned V9.bin.
    std::vector<u8> newBin; std::vector<int> bvMap;
    tinyjson::Value newBVs(tinyjson::Type::Array);
    if (g.root.has("bufferViews")) {
        const auto& BV = g.root["bufferViews"]; bvMap.assign(BV.size(), -1);
        for (size_t i = 0; i < BV.size(); ++i) {
            if (imageBVs.count((int)i)) continue;   // the image's bytes now live as a standalone file
            const auto& bv = BV[i];
            int buf = (int)bv["buffer"].asInt();
            size_t off = bv.has("byteOffset") ? (size_t)bv["byteOffset"].asInt() : 0;
            size_t len = (size_t)bv["byteLength"].asInt();
            if (buf < 0 || buf >= (int)g.buffers.size() || off + len > g.buffers[buf].size())
                fail("bufferView[" + std::to_string(i) + "] out of range");
            while (newBin.size() % 4) newBin.push_back(0);
            size_t newOff = newBin.size();
            newBin.insert(newBin.end(), g.buffers[buf].begin() + off, g.buffers[buf].begin() + off + len);
            tinyjson::Value nv(tinyjson::Type::Object);
            nv.objVal["buffer"] = vInt(0); nv.objVal["byteOffset"] = vInt((int64_t)newOff); nv.objVal["byteLength"] = vInt((int64_t)len);
            if (bv.has("byteStride")) nv.objVal["byteStride"] = vInt(bv["byteStride"].asInt());
            if (bv.has("target"))     nv.objVal["target"]     = vInt(bv["target"].asInt());
            bvMap[i] = (int)newBVs.arrVal.size();
            newBVs.arrVal.push_back(std::move(nv));
        }
    }
    g.root["bufferViews"] = std::move(newBVs);
    if (g.root.has("accessors")) remapBufferViews(g.root["accessors"], bvMap);

    tinyjson::Value buffers(tinyjson::Type::Array);
    tinyjson::Value b0(tinyjson::Type::Object);
    b0.objVal["byteLength"] = vInt((int64_t)newBin.size()); b0.objVal["uri"] = vStr("V9.bin");
    buffers.arrVal.push_back(std::move(b0));
    g.root["buffers"] = std::move(buffers);

    // 3. Fold each primitive's REAL UV set into TEXCOORD_0 (the only one the loader reads).
    auto matTexCoord = [&](int matIdx) -> int {
        if (matIdx < 0 || !g.root.has("materials")) return 0;
        const auto& M = g.root["materials"]; if ((size_t)matIdx >= M.size()) return 0;
        const auto& m = M[matIdx];
        if (m.has("pbrMetallicRoughness") && m["pbrMetallicRoughness"].has("baseColorTexture")) {
            const auto& t = m["pbrMetallicRoughness"]["baseColorTexture"];
            return t.has("texCoord") ? (int)t["texCoord"].asInt() : 0;
        }
        if (m.has("emissiveTexture")) { const auto& t = m["emissiveTexture"]; return t.has("texCoord") ? (int)t["texCoord"].asInt() : 0; }
        return 0;
    };
    auto& meshes = g.root["meshes"]; R.meshes = meshes.size();
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        if (!meshes[mi].has("primitives")) continue;
        auto& prims = meshes[mi]["primitives"];
        for (size_t pi = 0; pi < prims.size(); ++pi) {
            auto& pr = prims[pi];
            if (!pr.has("attributes") || !pr.has("material")) continue;
            int tc = matTexCoord((int)pr["material"].asInt());
            if (tc <= 0) continue;
            std::string want = "TEXCOORD_" + std::to_string(tc);
            auto& at = pr["attributes"];
            if (at.has(want)) { at.objVal["TEXCOORD_0"] = at[want]; R.uvFixes++; }
        }
    }

    // 4. Nest the zips: .ovrscene -> scene.zip -> .apk
    std::string gltfOut; serialize(g.root, gltfOut);
    ovr.push_back({ "V9.gltf", std::vector<u8>(gltfOut.begin(), gltfOut.end()), false });
    ovr.push_back({ "V9.bin",  std::move(newBin), false });
    std::vector<u8> ovrZip = buildZip(ovr);

    std::vector<ZipEntry> scene;
    scene.push_back({ "_WORLD_MODEL.gltf.ovrscene", std::move(ovrZip), true });
    if (!opt.audio.empty()) {
        std::vector<u8> ab = readFile(opt.audio);
        if (ab.empty()) fail("cannot read audio file: " + opt.audio);
        std::string ext = lower(fs::path(opt.audio).extension().string());
        if (ext != ".ogg" && ext != ".wav" && ext != ".mp3" && ext != ".flac") fail("audio must be .ogg/.wav/.mp3/.flac");
        scene.push_back({ "_BACKGROUND_LOOP" + ext, std::move(ab), true });
        R.audio = true;
    }
    std::vector<u8> sceneZip = buildZip(scene);

    std::vector<ZipEntry> apk; apk.push_back({ "assets/scene.zip", std::move(sceneZip), true });
    std::vector<u8> apkBytes = buildZip(apk);
    std::error_code ec; fs::create_directories(fs::path(out).parent_path(), ec);
    if (!writeFile(out, apkBytes.data(), apkBytes.size())) fail("cannot write output: " + out);
    R.bytes = apkBytes.size();
    return R;
}

// ── the drag-and-drop entry point (all main.cpp needs) ──────────────────────
// Is this dropped path a raw model — i.e. NOT an environment APK — that must be converted first?
inline bool isModel(const std::string& path) {
    return endsWithCI(path, ".glb") || endsWithCI(path, ".gltf");
}
// Convert a dropped model into a source APK and return its path; "" on failure, with `err` filled in
// for the status bar. The APK lands in the temp dir, so the user's model is never written next to —
// or over — anything of theirs. Never throws.
inline std::string convertForDrop(const std::string& model, std::string& err) {
    std::error_code ec;
    fs::path out = fs::temp_directory_path(ec) / "QuestHomeEditor";
    out /= fs::path(model).stem().string() + ".apk";

    Options opt; opt.input = model; opt.output = out.string();
    Result r;
    try { r = convert(opt); }
    catch (const std::exception& e) { err = e.what(); fprintf(stderr, "[GLB2APK] FAILED: %s\n", err.c_str()); return ""; }
    catch (...)                     { err = "unknown error"; fprintf(stderr, "[GLB2APK] FAILED: %s\n", err.c_str()); return ""; }

    fprintf(stderr, "[GLB2APK] %s -> %s (%zu meshes, %d textures, %d UV remaps, %.1f MB)\n",
            model.c_str(), opt.output.c_str(), r.meshes, r.textures, r.uvFixes, r.bytes / 1048576.0);
    return opt.output;
}

} // namespace glb2apk
