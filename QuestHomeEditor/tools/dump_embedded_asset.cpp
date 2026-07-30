#include "cook/embedded_assets.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: dump_embedded_asset <asset-name> <output>\n");
        return 2;
    }
    std::vector<uint8_t> bytes;
    if (!embassets::get(argv[1], bytes)) {
        std::fprintf(stderr, "embedded asset not found: %s\n", argv[1]);
        return 3;
    }
    FILE* f = std::fopen(argv[2], "wb");
    if (!f) return 4;
    const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok ? 0 : 5;
}
