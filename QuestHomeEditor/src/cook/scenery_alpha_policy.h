#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace sceneryalpha {

enum class Route {
    Blend,
    AuthoredCutout,
    BimodalCutout,
    SolidBodyCutout,
    NamedMaskCutout,
};

struct Input {
    std::string name;
    bool authoredCutout = false;
    bool blend = false;
    bool additive = false;
    bool cutoutShaderAvailable = false;
    bool farDepthRemapped = false;
    bool animatedEffect = false;
    float opaqueFraction = 0.f;
    float midAlphaFraction = 1.f;
    float extentX = 0.f;
    float extentY = 0.f;
    float extentZ = 0.f;
};

inline std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline bool isAtmosphericName(const std::string& name) {
    const std::string lower = lowercase(name);
    return lower.find("fog") != std::string::npos ||
           lower.find("mist") != std::string::npos ||
           lower.find("haze") != std::string::npos ||
           lower.find("smoke") != std::string::npos ||
           lower.find("cloud") != std::string::npos;
}

inline bool isLayeredPanoramaName(const std::string& name) {
    const std::string lower = lowercase(name);
    return lower.find("vista_atlas") != std::string::npos ||
           lower.find("mattepainting") != std::string::npos ||
           lower.find("matte_painting") != std::string::npos ||
           lower.find("panorama") != std::string::npos ||
           lower.find("backdrop") != std::string::npos;
}

// Panorama cards deliberately share one authored atlas and its border texels.
// Cropping every card into a separate texture changes the mip chain and removes
// neighbouring/border samples, which exposes dark triangular seams at distance.
inline bool allowsAtlasCrop(const std::string& name) {
    return !isLayeredPanoramaName(name);
}

inline Route classify(const Input& in) {
    if (in.authoredCutout && !in.additive && in.cutoutShaderAvailable &&
        !in.farDepthRemapped)
        return Route::AuthoredCutout;

    if (!in.blend || in.additive || !in.cutoutShaderAvailable ||
        in.farDepthRemapped)
        return Route::Blend;

    // Layered vista/matte cards deliberately overlap with feathered alpha.
    // Turning an individually mostly-opaque layer into a depth-writing cutout
    // exposes its polygon boundary as dark seams, holes or triangles.
    if (isLayeredPanoramaName(in.name))
        return Route::Blend;

    const bool bimodal = in.midAlphaFraction < 0.05f;
    const bool hasOpaque = in.opaqueFraction > 0.004f;
    const bool solidBody = in.opaqueFraction > 0.25f &&
                           in.midAlphaFraction < 0.18f &&
                           !in.animatedEffect;

    bool flatHorizontal =
        in.extentY < 0.20f * in.extentX &&
        in.extentY < 0.20f * in.extentZ;

    // Size does not identify atmosphere: legacy cities, farms and landscapes
    // are also kilometre-scale cards. Only genuine atmospheric semantics keep
    // a giant soft sheet in the no-depth-write blend pass.
    if (isAtmosphericName(in.name) &&
        (in.extentX > 300.f || in.extentZ > 300.f) &&
        in.midAlphaFraction > 0.05f)
        flatHorizontal = true;

    const std::string lower = lowercase(in.name);
    const bool namedMask = lower.find("alphamask") != std::string::npos &&
                           hasOpaque && !in.animatedEffect;

    if (flatHorizontal) return Route::Blend;
    if (bimodal && hasOpaque) return Route::BimodalCutout;
    if (solidBody) return Route::SolidBodyCutout;
    if (namedMask) return Route::NamedMaskCutout;
    return Route::Blend;
}

inline bool writesDepth(Route route) { return route != Route::Blend; }
inline bool usesLowCutoff(Route route) {
    return route == Route::BimodalCutout ||
           route == Route::SolidBodyCutout ||
           route == Route::NamedMaskCutout;
}

inline const char* label(Route route) {
    switch (route) {
        case Route::AuthoredCutout: return "CUTOUT(authored)";
        case Route::BimodalCutout: return "CUTOUT(bimodal)";
        case Route::SolidBodyCutout: return "CUTOUT(solid-body)";
        case Route::NamedMaskCutout: return "CUTOUT(named-mask)";
        case Route::Blend:
        default: return "blend";
    }
}

}  // namespace sceneryalpha
