#include "loaders/gltf_material_rules.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

void testEmissiveTextureDoesNotInheritBlackBaseRgb() {
    gltfmaterial::TextureTintInput in;
    in.usesEmissiveTexture = true;
    in.hasBaseColorFactor = true;
    in.baseColorFactor = {0.f, 0.f, 0.f, 0.75f};
    in.hasEmissiveFactor = true;
    in.emissiveFactor = {1.f, 0.5f, 0.25f};
    const auto tint = gltfmaterial::selectedTextureTint(in);
    require(near(tint[0], 1.f) && near(tint[1], 0.5f) && near(tint[2], 0.25f),
            "emissive texture uses emissive RGB factor");
    require(near(tint[3], 0.75f), "emissive texture preserves surface alpha");
}

void testBaseTextureKeepsBaseFactorPrecedence() {
    gltfmaterial::TextureTintInput in;
    in.usesBaseColorTexture = true;
    in.usesEmissiveTexture = true;
    in.hasBaseColorFactor = true;
    in.baseColorFactor = {0.1f, 0.2f, 0.3f, 0.4f};
    in.hasEmissiveFactor = true;
    in.emissiveFactor = {1.f, 1.f, 1.f};
    const auto tint = gltfmaterial::selectedTextureTint(in);
    require(near(tint[0], 0.1f) && near(tint[1], 0.2f)
                && near(tint[2], 0.3f) && near(tint[3], 0.4f),
            "base-colour texture keeps its authored base factor");
}

void testMissingEmissiveFactorFallsBackToVisibleIdentity() {
    gltfmaterial::TextureTintInput in;
    in.usesEmissiveTexture = true;
    const auto tint = gltfmaterial::selectedTextureTint(in);
    require(near(tint[0], 1.f) && near(tint[1], 1.f)
                && near(tint[2], 1.f) && near(tint[3], 1.f),
            "legacy emissive-only texture remains visible without an explicit factor");
}

}  // namespace

int main() {
    testEmissiveTextureDoesNotInheritBlackBaseRgb();
    testBaseTextureKeepsBaseFactorPrecedence();
    testMissingEmissiveFactorFallsBackToVisibleIdentity();
    std::cout << "gltf material rule tests passed\n";
    return 0;
}
