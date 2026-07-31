#include "device/runtime_health_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testPositiveEvidenceIsRequired() {
    using runtimehealth::Verdict;
    require(runtimehealth::classify(
                "Home loaded OK on device (Scene loaded).") == Verdict::Healthy,
            "Scene-loaded evidence is healthy");
    require(runtimehealth::classify(
                "REJECTED -> nuxd fallback.") == Verdict::Rejected,
            "explicit fallback is rejected");
    require(runtimehealth::classify(
                "Load inconclusive - see report.txt") == Verdict::Inconclusive,
            "resolution without a render marker is inconclusive");
    require(!runtimehealth::mustRollbackStandaloneHaven(
                "Load inconclusive - see report.txt", false),
            "normal layer transitions must not roll back a working Haven");
    require(!runtimehealth::mustRollbackStandaloneHaven(
                "Home loaded OK on device (Scene loaded).", false),
            "positive runtime evidence commits the transaction");
    require(!runtimehealth::mustRollbackStandaloneHaven(
                "Load inconclusive - see report.txt", true),
            "isolated development may inspect an inconclusive runtime");
    require(runtimehealth::mustRollbackStandaloneHaven(
                "REJECTED -> nuxd fallback.", true),
            "even development mode rolls back an explicit rejection");
}

}  // namespace

int main() {
    testPositiveEvidenceIsRequired();
    std::cout << "runtime health policy tests passed\n";
    return 0;
}
