#pragma once

#include <cstdint>
#include <string_view>

namespace hslcook {

// Central quality policy for every cook entry point.  Keep this header independent
// from astcenc so the editor, CLI and unit tests all consume the same plan.
enum class CookQualityProfile : uint8_t {
    OriginalLossless = 0,
    Quest3Quality = 1,
    ExtremeLab = 2,
    Performance = 3,
};

enum class CookEncoderPreset : uint8_t {
    Fast = 0,
    Medium = 1,
    Thorough = 2,
    Exhaustive = 3,
};

struct CookQualityOptions {
    CookQualityProfile profile = CookQualityProfile::Quest3Quality;
    CookEncoderPreset opaquePreset = CookEncoderPreset::Thorough;
    CookEncoderPreset maskedPreset = CookEncoderPreset::Thorough;
    int opaqueBlockWidth = 6;
    int opaqueBlockHeight = 6;
    int maskedBlockWidth = 6;
    int maskedBlockHeight = 6;
    int maxTextureEdge = 0;          // 0 = keep authored resolution
    bool preserveSourceAstc = true;
    bool preserveMaskedSourceAstc = false;
    bool extendMissingMips = true;
    bool preserveFullMipChain = true;
    float animationFps = 30.0f;
    int animationFrameCap = 0;       // 0 = no profile cap
    int rotationKeyLimit = 128;
};

inline constexpr CookQualityOptions cookQualityOptions(CookQualityProfile p) {
    switch (p) {
        case CookQualityProfile::OriginalLossless:
            return {p,
                    CookEncoderPreset::Thorough, CookEncoderPreset::Thorough,
                    6, 6, 6, 6, 0,
                    true, true, false, true,
                    30.0f, 0, 128};
        case CookQualityProfile::ExtremeLab:
            return {p,
                    CookEncoderPreset::Exhaustive, CookEncoderPreset::Exhaustive,
                    4, 4, 4, 4, 0,
                    false, false, true, true,
                    60.0f, 0, 256};
        case CookQualityProfile::Performance:
            return {p,
                    CookEncoderPreset::Fast, CookEncoderPreset::Medium,
                    12, 12, 8, 8, 2048,
                    true, true, false, true,
                    15.0f, 450, 64};
        case CookQualityProfile::Quest3Quality:
        default:
            return {CookQualityProfile::Quest3Quality,
                    CookEncoderPreset::Medium, CookEncoderPreset::Thorough,
                    6, 6, 6, 6, 0,
                    true, true, false, true,
                    30.0f, 0, 128};
    }
}

inline constexpr CookQualityProfile defaultCookQualityProfile() {
    return CookQualityProfile::Quest3Quality;
}

inline constexpr bool tryParseCookQuality(std::string_view id, CookQualityProfile& out) {
    if (id == "original" || id == "lossless") {
        out = CookQualityProfile::OriginalLossless;
        return true;
    }
    if (id == "quest3" || id == "quality") {
        out = CookQualityProfile::Quest3Quality;
        return true;
    }
    if (id == "extreme" || id == "lab") {
        out = CookQualityProfile::ExtremeLab;
        return true;
    }
    if (id == "performance" || id == "fast") {
        out = CookQualityProfile::Performance;
        return true;
    }
    return false;
}

inline constexpr std::string_view cookQualityId(CookQualityProfile p) {
    switch (p) {
        case CookQualityProfile::OriginalLossless: return "original";
        case CookQualityProfile::ExtremeLab:       return "extreme";
        case CookQualityProfile::Performance:      return "performance";
        case CookQualityProfile::Quest3Quality:
        default:                                    return "quest3";
    }
}

inline constexpr std::string_view cookQualityName(CookQualityProfile p) {
    switch (p) {
        case CookQualityProfile::OriginalLossless: return "Original / Lossless";
        case CookQualityProfile::ExtremeLab:       return "Extreme / Lab";
        case CookQualityProfile::Performance:      return "Performance";
        case CookQualityProfile::Quest3Quality:
        default:                                    return "Quest 3 Quality";
    }
}

inline constexpr std::string_view cookQualityDescription(CookQualityProfile p) {
    switch (p) {
        case CookQualityProfile::OriginalLossless:
            return "Keeps valid authored ASTC payloads byte-for-byte.";
        case CookQualityProfile::ExtremeLab:
            return "Full-resolution ASTC 4x4 with exhaustive encoding; largest and slowest.";
        case CookQualityProfile::Performance:
            return "Lower memory and faster loading for very large Homes.";
        case CookQualityProfile::Quest3Quality:
        default:
            return "Recommended: authored detail preserved, edits encoded at high quality.";
    }
}

inline constexpr bool isRecommendedCookQuality(CookQualityProfile p) {
    return p == CookQualityProfile::Quest3Quality;
}

inline constexpr CookQualityProfile cookQualityFromIndex(int index) {
    return index <= 0 ? CookQualityProfile::OriginalLossless
         : index == 1 ? CookQualityProfile::Quest3Quality
         : index == 2 ? CookQualityProfile::ExtremeLab
                      : CookQualityProfile::Performance;
}

inline constexpr int cookQualityIndex(CookQualityProfile p) {
    return static_cast<int>(p);
}

// RENDTXTR field 2 is Meta's full ASTC texture-format enum: it encodes both
// the block footprint and the colour space.  The cooker emits sRGB albedo, so
// use the sRGB member of every current v206 pair.  Unknown footprints must
// fail closed; mislabelling valid ASTC bytes makes the Quest decode them with
// the wrong block geometry (striped/noisy textures).
inline constexpr uint8_t cookAstcSrgbFormatEnum(int width, int height) {
    return width == 4  && height == 4  ? 16
         : width == 6  && height == 6  ? 18
         : width == 8  && height == 8  ? 20
         : width == 12 && height == 12 ? 24
                                      : 0;
}

// Bump when an encoder/cache-relevant policy changes.  This value is mixed into
// persistent cache keys, preventing e.g. Extreme from consuming Performance data.
inline constexpr uint32_t kCookQualityCacheSchema = 0x00020005u;

inline constexpr uint64_t cookQualityKeyMix(uint64_t h, uint64_t value) {
    return (h ^ value) * 1099511628211ull;
}

inline constexpr uint64_t cookQualityCachePolicyKey(const CookQualityOptions& q) {
    uint64_t key=1469598103934665603ull;
    key=cookQualityKeyMix(key,kCookQualityCacheSchema);
    key=cookQualityKeyMix(key,static_cast<uint64_t>(q.profile));
    key=cookQualityKeyMix(key,static_cast<uint64_t>(q.opaquePreset));
    key=cookQualityKeyMix(key,static_cast<uint64_t>(q.maskedPreset));
    key=cookQualityKeyMix(key,(uint64_t)(uint32_t)q.opaqueBlockWidth);
    key=cookQualityKeyMix(key,(uint64_t)(uint32_t)q.opaqueBlockHeight);
    key=cookQualityKeyMix(key,(uint64_t)(uint32_t)q.maskedBlockWidth);
    key=cookQualityKeyMix(key,(uint64_t)(uint32_t)q.maskedBlockHeight);
    key=cookQualityKeyMix(key,(uint64_t)(uint32_t)q.maxTextureEdge);
    key=cookQualityKeyMix(key,(uint64_t)q.preserveSourceAstc);
    key=cookQualityKeyMix(key,(uint64_t)q.preserveMaskedSourceAstc);
    key=cookQualityKeyMix(key,(uint64_t)q.extendMissingMips);
    key=cookQualityKeyMix(key,(uint64_t)q.preserveFullMipChain);
    return key;
}

} // namespace hslcook
