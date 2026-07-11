#pragma once
#include "core/types.h"
#include <vector>
#include <cstring>

// Parses content/assets.manifest — ASMH v2 binary with FlatBuffer encoding.
//
// Binary layout:
//   +0:  "ASMH" magic
//   +4:  version u32 (= 2)
//   +8:  padding u64 (zeros)
//   +16: root_table_offset u32 — offset of root FlatBuffer table from position 16
//
// Root table uses standard FlatBuffer vtable layout.  One of its fields is
// the CONTENT vector: each element holds (pkg u64, ing u64, tgt u32, path str).
//
// CONTENT entry layout (relative to element table start):
//   +0:  soffset i32
//   +4:  pkg_hash u64
//   +12: ing u64
//   +20: tgt u32
//   +24: zeros u64
//   +32: path_str_ref i32 — signed offset from this field to FlatBuffer string header
//
// FlatBuffer vector elements store SIGNED offsets relative to the element's
// own position: table_address = element_position + i32(element_position).
//
// Strategy: walk vtable fields; the first field whose first entry has a valid
// ing (>= 2^32) and a path string containing "meta/" is the CONTENT field.

inline bool parseAsmh(const std::vector<u8>& data, AssetMap& out) {
    if (data.size() < 0x40) return false;
    if (memcmp(data.data(), "ASMH", 4) != 0) return false;

    const u32 SZ = (u32)data.size();

    auto u16at = [&](u32 o) -> u16 {
        if (o + 2 > SZ) return 0;
        u16 v; memcpy(&v, data.data() + o, 2); return v;
    };
    auto u32at = [&](u32 o) -> u32 {
        if (o + 4 > SZ) return 0;
        u32 v; memcpy(&v, data.data() + o, 4); return v;
    };
    auto i32at = [&](u32 o) -> i32 {
        if (o + 4 > SZ) return 0;
        i32 v; memcpy(&v, data.data() + o, 4); return v;
    };
    auto u64at = [&](u32 o) -> u64 {
        if (o + 8 > SZ) return 0;
        u64 v; memcpy(&v, data.data() + o, 8); return v;
    };
    auto fbStr = [&](u32 o) -> std::string {
        if (o + 4 > SZ) return {};
        u32 len = u32at(o);
        if (!len || len > 1024 || o + 4 + len > SZ) return {};
        return {reinterpret_cast<const char*>(data.data() + o + 4), len};
    };

    // Navigate to the root FlatBuffer table.
    // Offset 16 stores a u32 that, added to 16, gives the root table position.
    u32 rootOff = u32at(16);
    u32 rootTablePos = rootOff + 16;
    if (rootTablePos + 4 > SZ) return false;

    i32 soff = i32at(rootTablePos);
    u32 vtable = (u32)((i32)rootTablePos - soff);
    if (vtable + 4 > SZ) return false;

    u16 vtSize  = u16at(vtable);
    u32 nFields = (vtSize > 4) ? (vtSize - 4) / 2 : 0;

    // Scan all vtable fields for the CONTENT vector.
    // Identified by: first entry has ing >= 2^32 and path containing "meta/".
    for (u32 fi = 0; fi < nFields && out.empty(); ++fi) {
        u32 foffPos = vtable + 4 + fi * 2;
        if (foffPos + 2 > SZ) break;
        u16 fieldOff = u16at(foffPos);
        if (!fieldOff) continue;

        u32 fieldAbs = rootTablePos + fieldOff;
        if (fieldAbs + 4 > SZ) continue;
        u32 vecRel = u32at(fieldAbs);
        if (!vecRel) continue;
        u32 vecAbs = fieldAbs + vecRel;
        if (vecAbs + 4 > SZ) continue;

        u32 count = u32at(vecAbs);
        if (count == 0 || count > 10000) continue;

        // Validate first element as a CONTENT entry.
        u32 e0pos = vecAbs + 4;
        if (e0pos + 4 > SZ) continue;
        i32 rel0 = i32at(e0pos);
        u32 t0 = (u32)((i32)e0pos + rel0);
        if (t0 + 36 > SZ) continue;

        u64 ing0 = u64at(t0 + 12);
        if (ing0 < 0x100000000ULL) continue;  // must be a large random-looking ID

        i32 pref0 = i32at(t0 + 32);
        if (pref0 <= 0 || (u32)pref0 > SZ) continue;   // string offset is within the (possibly large) manifest, not a fixed 4096
        std::string path0 = fbStr((u32)((i32)(t0 + 32) + pref0));
        if (path0.empty() || path0.find("meta/") == std::string::npos) continue;

        // This field is the CONTENT section — parse all entries.
        for (u32 i = 0; i < count; ++i) {
            u32 ep = vecAbs + 4 + i * 4;
            if (ep + 4 > SZ) continue;
            i32 rel = i32at(ep);
            u32 t = (u32)((i32)ep + rel);
            if (t + 36 > SZ) continue;

            u64 pkg = u64at(t + 4);
            u64 ing = u64at(t + 12);
            u32 tgt = u32at(t + 20);
            if (ing < 0x100000000ULL) continue;

            i32 pref = i32at(t + 32);
            if (pref <= 0 || (u32)pref > SZ) continue;
            std::string path = fbStr((u32)((i32)(t + 32) + pref));
            if (path.empty() || path.find("meta/") == std::string::npos) continue;

            // Insert with real pkg AND with pkg=0 so lookups succeed regardless
            // of which packageOrRemoteId the HSTF JSON stores.
            AssetKey k; k.ing = ing; k.tgt = tgt;
            k.pkg = pkg; out[k] = path;
            k.pkg = 0;   out[k] = path;
        }
    }

    return !out.empty();
}
