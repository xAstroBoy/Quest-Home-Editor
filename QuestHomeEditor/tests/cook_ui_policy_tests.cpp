#include "ui/cook_ui_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testStatusClassification() {
    require(cookui::classifyStatus("Cook complete") == cookui::Severity::Success,
            "ordinary completion must be successful");
    require(cookui::classifyStatus("Scene is STALE") == cookui::Severity::Warning,
            "stale output must be a warning");
    require(cookui::classifyStatus("No device connected") == cookui::Severity::Warning,
            "a missing optional delivery device must be actionable, not fatal");
    require(cookui::classifyStatus("Install FAILED") == cookui::Severity::Error,
            "failed operations must be errors");
    require(cookui::classifyStatus("ABORTED: backup unavailable") == cookui::Severity::Error,
            "a safety abort must be an error");
    require(cookui::classifyStatus("Standard Home install BLOCKED safely") ==
                cookui::Severity::Warning,
            "an intentional compatibility block must be visible as a warning");
}

void testRecoveryGuidance() {
    const auto device = cookui::guideStatus(
        "Scene is up to date, but NO device is connected");
    require(device.action == cookui::RecoveryAction::RefreshQuest,
            "device failures must offer a safe refresh");
    const auto stale = cookui::guideStatus("Scene changed; cooked APK is STALE");
    require(stale.action == cookui::RecoveryAction::RetryCook,
            "stale output must offer a recook");
    const auto failed = cookui::guideStatus("Signing FAILED");
    require(failed.action == cookui::RecoveryAction::RetryCook,
            "ordinary cook failures must offer a retry");
    require(cookui::guideStatus("Cook complete").action ==
                cookui::RecoveryAction::None,
            "successful status must not show a recovery action");
    const auto vista = cookui::guideStatus(
        "Meta LayerRendering temporarily hid EnvironmentSystem");
    require(vista.action == cookui::RecoveryAction::None &&
            std::string(vista.solution).find("do not roll back") != std::string::npos,
            "LayerRendering alone must never trigger destructive recovery guidance");
}

void testProgressSteps() {
    const auto prepare = cookui::describeProgress(-1.0f);
    require(prepare.step == 1 && prepare.percent == 0,
            "progress must clamp at the first step");
    require(cookui::describeProgress(0.50f).step == 2,
            "scene generation belongs to Build Home");
    require(cookui::describeProgress(0.88f).step == 3,
            "signing belongs to Package and sign");
    require(cookui::describeProgress(0.95f).step == 4,
            "adb delivery belongs to Install or export");
    const auto done = cookui::describeProgress(2.0f);
    require(done.step == 5 && done.percent == 100,
            "progress must clamp at Complete");
}

void testDeliveryTargetIsolation() {
    require(cookui::usesVistaWrapper(1) && cookui::usesVistaWrapper(4),
            "all four Vista slots must use the wrapper route");
    require(!cookui::usesVistaWrapper(0),
            "Standard Home must never be mistaken for a Vista route");
    require(cookui::requiredCookOutputsPresent(true, false, true, 2),
            "Vista delivery needs only the reusable Rooted-System scene");
    require(!cookui::requiredCookOutputsPresent(true, false, true, 0),
            "legacy Haven delivery must not reuse a cook with no spoof APK");
    require(!cookui::requiredCookOutputsPresent(false, true, true, 2),
            "a spoof alone cannot feed a Vista wrapper");
}

}  // namespace

int main() {
    testStatusClassification();
    testRecoveryGuidance();
    testProgressSteps();
    testDeliveryTargetIsolation();
    std::cout << "cook UI policy tests passed\n";
    return 0;
}
