#include "device/quest_display_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void requireTarget(const questdisplay::DeviceIdentity& identity,
                   questdisplay::Model model, int width, int height,
                   const char* message) {
    const auto target = questdisplay::classify(identity);
    require(target.model == model, message);
    require(target.width == width && target.height == height, message);
}

void testQuestFamilyTargets() {
    requireTarget({"Meta", "Meta Quest 3", "eureka"},
                  questdisplay::Model::Quest3, 2800, 2800,
                  "Quest 3 must use the user-verified Home HD+ target");
    requireTarget({"Meta", "Meta Quest 3S", "panther"},
                  questdisplay::Model::Quest3S, 2800, 2800,
                  "Quest 3S has Quest 3-class rendering performance");
    requireTarget({"Oculus", "Quest 2", "hollywood"},
                  questdisplay::Model::Quest2, 2400, 2400,
                  "Quest 2 must use the reduced XR2 Gen 1 target");
    requireTarget({"Meta", "Quest Pro", "seacliff"},
                  questdisplay::Model::QuestPro, 2560, 2736,
                  "Quest Pro target changed unexpectedly");
    requireTarget({"Oculus", "Quest", "monterey"},
                  questdisplay::Model::Quest1, 2048, 2272,
                  "Quest 1 target changed unexpectedly");
}

void testCodenameAndCaseFallbacks() {
    requireTarget({"META", "unknown", "PANTHER"},
                  questdisplay::Model::Quest3S, 2800, 2800,
                  "Quest 3S codename detection must be case-insensitive");
    requireTarget({"oculus", "unknown", "EUREKA"},
                  questdisplay::Model::Quest3, 2800, 2800,
                  "Quest 3 codename detection must be case-insensitive");
}

void testUnknownDevicesAreNeverModified() {
    require(!questdisplay::classify({"Samsung", "SM-S928B", "e3q"}).recognized(),
            "non-Meta Android devices must not receive Quest properties");
    require(!questdisplay::classify({"Meta", "Future Headset", "future"}).recognized(),
            "unknown future Meta devices require an explicit reviewed profile");
}

}  // namespace

int main() {
    testQuestFamilyTargets();
    testCodenameAndCaseFallbacks();
    testUnknownDevicesAreNeverModified();
    std::cout << "quest display policy tests passed\n";
    return 0;
}
