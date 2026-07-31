#include "cook/standalone_haven_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    constexpr auto combined = standalonehaven::plan(false);
    require(combined.nuxdManifestIdentity,
            "standalone Haven must use the device-proven Nuxd combined identity");
    require(!combined.footprintManifestIdentity,
            "standalone Haven must use Haven's combined identity");

    constexpr auto footprint = standalonehaven::plan(true);
    require(!footprint.nuxdManifestIdentity,
            "footprint compatibility must not inherit Nuxd identity");
    require(footprint.footprintManifestIdentity,
            "explicit footprint compatibility must preserve footprint type");

    std::cout << "standalone Haven policy tests passed\n";
    return 0;
}
