#include "cook/navigation_floor_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testPlayableFoundationBeatsPanoramaArea() {
    require(navigationfloor::better(
                {true, true, 216.78, 9},
                {true, false, 1750791.55, 14}),
            "a compact start foundation must beat a giant scenery card");
}

void testOnlyAvailableOutdoorFoundationStillWorks() {
    require(navigationfloor::better(
                {true, false, 120000.0, 3},
                {false, true, 500.0, 4}),
            "a large outdoor floor carrying the start must beat disconnected geometry");
}

void testScoreAndStableIdRemainDeterministic() {
    require(navigationfloor::better(
                {true, true, 200.0, 8},
                {true, true, 100.0, 2}),
            "equally plausible foundations must retain geometry scoring");
    require(navigationfloor::better(
                {true, true, 100.0, 2},
                {true, true, 100.0, 8}),
            "equal scores must have a stable deterministic tie break");
}

}  // namespace

int main() {
    testPlayableFoundationBeatsPanoramaArea();
    testOnlyAvailableOutdoorFoundationStillWorks();
    testScoreAndStableIdRemainDeterministic();
    std::cout << "navigation floor policy tests passed\n";
    return 0;
}
