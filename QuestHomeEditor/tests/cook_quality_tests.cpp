#include "cook/cook_quality.h"

#include <cassert>
#include <iostream>
#include <set>
#include <string>

using namespace hslcook;

int main() {
    const auto original = cookQualityOptions(CookQualityProfile::OriginalLossless);
    const auto quality = cookQualityOptions(CookQualityProfile::Quest3Quality);
    const auto extreme = cookQualityOptions(CookQualityProfile::ExtremeLab);
    const auto performance = cookQualityOptions(CookQualityProfile::Performance);

    assert(defaultCookQualityProfile() == CookQualityProfile::Quest3Quality);
    CookQualityProfile parsed = CookQualityProfile::OriginalLossless;
    assert(tryParseCookQuality("quest3", parsed) && parsed == CookQualityProfile::Quest3Quality);
    assert(tryParseCookQuality("lossless", parsed) && parsed == CookQualityProfile::OriginalLossless);
    assert(tryParseCookQuality("lab", parsed) && parsed == CookQualityProfile::ExtremeLab);
    assert(tryParseCookQuality("fast", parsed) && parsed == CookQualityProfile::Performance);
    assert(!tryParseCookQuality("unknown", parsed));
    assert(original.preserveSourceAstc && original.preserveMaskedSourceAstc);
    assert(!original.extendMissingMips);
    assert(quality.opaqueBlockWidth == 6 && quality.maxTextureEdge == 0);
    assert(extreme.opaqueBlockWidth == 4 && extreme.opaqueBlockHeight == 4);
    assert(extreme.maskedBlockWidth == 4 && extreme.maskedBlockHeight == 4);
    assert(extreme.maxTextureEdge == 0);
    assert(!extreme.preserveSourceAstc && !extreme.preserveMaskedSourceAstc);
    assert(quality.opaquePreset == CookEncoderPreset::Medium);
    assert(quality.preserveMaskedSourceAstc);
    assert(!quality.extendMissingMips);
    assert(extreme.opaquePreset == CookEncoderPreset::Exhaustive);
    assert(extreme.rotationKeyLimit > quality.rotationKeyLimit);
    assert(performance.maxTextureEdge == 2048);
    assert(performance.opaqueBlockWidth == 12);
    assert(performance.maskedBlockWidth == 8);
    assert(performance.opaquePreset == CookEncoderPreset::Fast);
    assert(performance.maskedPreset == CookEncoderPreset::Medium);
    assert(!performance.extendMissingMips);
    assert(performance.animationFps == 15.0f);
    assert(cookAstcSrgbFormatEnum(4, 4) == 16);
    assert(cookAstcSrgbFormatEnum(6, 6) == 18);
    assert(cookAstcSrgbFormatEnum(8, 8) == 20);
    assert(cookAstcSrgbFormatEnum(12, 12) == 24);
    assert(cookAstcSrgbFormatEnum(5, 5) == 0);

    std::set<uint64_t> keys;
    for (int i = 0; i < 4; ++i) {
        const auto p = cookQualityFromIndex(i);
        assert(cookQualityIndex(p) == i);
        assert(!cookQualityId(p).empty());
        assert(!cookQualityName(p).empty());
        assert(!cookQualityDescription(p).empty());
        keys.insert(cookQualityCachePolicyKey(cookQualityOptions(p)));
    }
    assert(keys.size() == 4);

    auto modified = quality;
    modified.maskedPreset = CookEncoderPreset::Medium;
    assert(cookQualityCachePolicyKey(modified) != cookQualityCachePolicyKey(quality));

    std::cout << "cook quality profiles: OK\n";
    return 0;
}
