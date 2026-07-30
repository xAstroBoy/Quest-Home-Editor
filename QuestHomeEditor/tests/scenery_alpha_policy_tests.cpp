#include "cook/scenery_alpha_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

sceneryalpha::Input legacyCard(const char* name, float opaque, float mid) {
    sceneryalpha::Input in;
    in.name = name;
    in.blend = true;
    in.cutoutShaderAvailable = true;
    in.opaqueFraction = opaque;
    in.midAlphaFraction = mid;
    in.extentX = 1200.f;
    in.extentY = 700.f;
    in.extentZ = 80.f;
    return in;
}

void testLargeSolidSceneryWritesDepth() {
    auto farm = legacyCard("environment.farm_MAT_alpha", 0.54f, 0.07f);
    auto city = legacyCard("environment.city2_MAT_alpha", 0.58f, 0.14f);
    require(sceneryalpha::classify(farm) ==
                sceneryalpha::Route::SolidBodyCutout,
            "large legacy farm card must not be mistaken for atmosphere");
    require(sceneryalpha::classify(city) ==
                sceneryalpha::Route::SolidBodyCutout,
            "large legacy city card must write stable depth");
}

void testGenuineAtmosphereRemainsBlend() {
    auto fog = legacyCard("environment.fogC_MAT_alpha", 0.31f, 0.08f);
    auto cloud = legacyCard("environment.Cloud_MAT_alpha", 0.25f, 0.18f);
    require(sceneryalpha::classify(fog) == sceneryalpha::Route::Blend,
            "giant fog sheet must retain soft blending");
    require(sceneryalpha::classify(cloud) == sceneryalpha::Route::Blend,
            "cloud card must retain soft blending");
}

void testLayeredPanoramasRetainFeatheredBlend() {
    auto foreground =
        legacyCard("M_vista_atlas_FG_alpha", 0.61f, 0.08f);
    auto background =
        legacyCard("environment_mattepainting_BG_alpha", 0.57f, 0.12f);
    auto backdrop =
        legacyCard("mountain_backdrop_alpha", 0.49f, 0.04f);
    require(sceneryalpha::classify(foreground) == sceneryalpha::Route::Blend,
            "vista atlas foreground must keep its feathered overlap");
    require(sceneryalpha::classify(background) == sceneryalpha::Route::Blend,
            "matte-painting background must keep its feathered overlap");
    require(sceneryalpha::classify(backdrop) == sceneryalpha::Route::Blend,
            "panorama backdrop must not expose its polygon boundary");
}

void testHorizontalOverlayRemainsBlend() {
    auto pond = legacyCard("pond_color_overlay", 0.57f, 0.03f);
    pond.extentX = 30.f;
    pond.extentY = 0.2f;
    pond.extentZ = 25.f;
    require(sceneryalpha::classify(pond) == sceneryalpha::Route::Blend,
            "coplanar pond overlay must not depth-write");
}

void testHardMaskAndAnimatedEffectRouting() {
    auto mask = legacyCard("FoliageDistance_AlphaMask", 0.02f, 0.20f);
    require(sceneryalpha::classify(mask) ==
                sceneryalpha::Route::NamedMaskCutout,
            "authored alpha-mask semantic must write depth");

    auto animated = legacyCard("animated_scenery", 0.60f, 0.10f);
    animated.animatedEffect = true;
    require(sceneryalpha::classify(animated) == sceneryalpha::Route::Blend,
            "animated soft effect must not be reclassified as a solid card");
}

}  // namespace

int main() {
    require(!sceneryalpha::allowsAtlasCrop("M_vista_atlas_FG_alpha"),
            "shared Vista panorama atlases must retain authored borders and mips");
    require(!sceneryalpha::allowsAtlasCrop("distant_mattePainting_backdrop"),
            "matte-painting backdrops must retain their shared source atlas");
    require(sceneryalpha::allowsAtlasCrop("balloon_design_atlas"),
            "ordinary design atlases may still use the sharp sub-rect optimization");

    testLargeSolidSceneryWritesDepth();
    testGenuineAtmosphereRemainsBlend();
    testLayeredPanoramasRetainFeatheredBlend();
    testHorizontalOverlayRemainsBlend();
    testHardMaskAndAnimatedEffectRouting();
    std::cout << "scenery alpha policy tests passed\n";
    return 0;
}
