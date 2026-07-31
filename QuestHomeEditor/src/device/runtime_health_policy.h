#pragma once

#include <string>

namespace runtimehealth {

enum class Verdict {
    Healthy,
    Rejected,
    Inconclusive,
};

inline Verdict classify(const std::string& diagnosis) {
    if (diagnosis.rfind("Home loaded OK", 0) == 0)
        return Verdict::Healthy;
    if (diagnosis.rfind("REJECTED", 0) == 0)
        return Verdict::Rejected;
    return Verdict::Inconclusive;
}

inline bool mustRollbackStandaloneHaven(const std::string& diagnosis,
                                        bool explicitDevelopmentOverride) {
    (void)explicitDevelopmentOverride;
    // VrShell legitimately reports an inconclusive/temporarily hidden
    // EnvironmentSystem while panels and other composition layers are active.
    // Rolling Haven back during that transition creates the black state we
    // were trying to prevent. Only an explicit environment fallback/rejection
    // is destructive evidence.
    return classify(diagnosis) == Verdict::Rejected;
}

}  // namespace runtimehealth
