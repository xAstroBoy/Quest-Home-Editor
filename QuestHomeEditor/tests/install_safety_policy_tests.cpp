#include "device/install_safety_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testStorageParsingAndBudget() {
    const std::string df =
        "Filesystem 1K-blocks Used Available Use% Mounted on\n"
        "/dev/block/dm-8 10000000 2000000 8000000 20% /data\n";
    require(installsafety::parseDfAvailableBytes(df) == 8000000ull * 1024ull,
            "Android df available space must be parsed in KiB");
    require(installsafety::requiredFreeBytes(10ull * 1024ull * 1024ull) ==
                256ull * 1024ull * 1024ull,
            "small APKs still need a safe installation reserve");
    require(!installsafety::hasEnoughSpace(300ull * 1024ull * 1024ull,
                                           200ull * 1024ull * 1024ull),
            "large APK staging must be rejected before destructive work");
    require(installsafety::hasEnoughSpace(0, 200ull * 1024ull * 1024ull),
            "unknown capacity must not create a false hard failure");
}

void testStandaloneHavenGate() {
    using installsafety::StandaloneHavenDecision;
    require(installsafety::standaloneHavenDecision(true, true, false) ==
                StandaloneHavenDecision::BlockInvalidV206Carrier,
            "standalone Haven replacement must fail closed for an invalid v206 carrier");
    require(installsafety::standaloneHavenDecision(false, true, false) ==
                StandaloneHavenDecision::Allow,
            "Vista installs must remain available on VrShell 206");
    require(installsafety::standaloneHavenDecision(true, false, false) ==
                StandaloneHavenDecision::Allow,
            "the v206 gate must not change older proven runtimes");
    require(installsafety::standaloneHavenDecision(true, true, true) ==
                StandaloneHavenDecision::Allow,
            "a validated v206 carrier is supported when Calming is the active transition slot");
}

void testUninstallRetryIsNarrow() {
    require(installsafety::mayRetryAfterUninstall(
                "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures differ]"),
            "signature conflicts require a clean replacement");
    require(installsafety::mayRetryAfterUninstall(
                "Failure [INSTALL_FAILED_VERSION_DOWNGRADE]"),
            "a rejected downgrade may require a clean replacement");
    require(!installsafety::mayRetryAfterUninstall(
                "Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE]"),
            "storage failure must never uninstall the working package");
    require(!installsafety::mayRetryAfterUninstall(
                "Failure [INSTALL_PARSE_FAILED_BAD_MANIFEST]"),
            "a malformed candidate must never uninstall the working package");
    require(!installsafety::mayRetryAfterUninstall("error: device offline"),
            "transport failure must leave the working package untouched");
}

}  // namespace

int main() {
    testStorageParsingAndBudget();
    testStandaloneHavenGate();
    testUninstallRetryIsNarrow();
    std::cout << "install safety policy tests passed\n";
    return 0;
}
