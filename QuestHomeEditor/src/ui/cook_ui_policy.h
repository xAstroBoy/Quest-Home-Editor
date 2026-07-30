#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace cookui {

enum class Severity {
    Success,
    Warning,
    Error,
};

enum class RecoveryAction {
    None,
    RetryCook,
    RefreshQuest,
};

struct Guidance {
    Severity severity = Severity::Success;
    RecoveryAction action = RecoveryAction::None;
    const char* solution = "";
};

struct Progress {
    int step = 1;
    int stepCount = 5;
    int percent = 0;
    const char* label = "Prepare";
};

inline std::string lowerAscii(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline Severity classifyStatus(const std::string& status) {
    const std::string low = lowerAscii(status);
    if (low.find("failed") != std::string::npos ||
        low.find("error") != std::string::npos ||
        low.find("rejected") != std::string::npos ||
        low.find("missing") != std::string::npos ||
        low.find("aborted") != std::string::npos)
        return Severity::Error;
    if (low.find("warning") != std::string::npos ||
        low.find("blocked") != std::string::npos ||
        low.find("stale") != std::string::npos ||
        low.find("unknown") != std::string::npos ||
        low.find("not ") != std::string::npos ||
        low.find("no device") != std::string::npos)
        return Severity::Warning;
    return Severity::Success;
}

inline Guidance guideStatus(const std::string& status) {
    const std::string low = lowerAscii(status);
    Guidance result;
    result.severity = classifyStatus(status);
    if (low.find("no device") != std::string::npos ||
        low.find("adb/device") != std::string::npos ||
        low.find("connect the quest") != std::string::npos) {
        result.action = RecoveryAction::RefreshQuest;
        result.solution = "Connect and unlock the Quest, allow USB debugging, then refresh.";
    } else if (low.find("layerrendering") != std::string::npos) {
        result.solution = "Quest panels can temporarily hide the Home layer. Close panels and verify visually; do not roll back from this message alone.";
    } else if (low.find("vista slot") != std::string::npos) {
        result.solution = "Choose the requested Vista slot and retry without changing the other installed Homes.";
    } else if (low.find("stale") != std::string::npos ||
               low.find("no cooked apk") != std::string::npos ||
               low.find("recook") != std::string::npos ||
               low.find("re-cook") != std::string::npos ||
               result.severity == Severity::Error) {
        result.action = RecoveryAction::RetryCook;
        result.solution = "The source is safe. Retry the cook; existing working backups are kept.";
    } else if (result.severity == Severity::Warning) {
        result.solution = "Review the message, correct the highlighted requirement, then try again.";
    }
    return result;
}

inline Progress describeProgress(float fraction) {
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    Progress result;
    result.percent = static_cast<int>(f * 100.0f + 0.5f);
    if (f < 0.10f)
        return {1, 5, result.percent, "Prepare"};
    if (f < 0.85f)
        return {2, 5, result.percent, "Build Home"};
    if (f < 0.91f)
        return {3, 5, result.percent, "Package and sign"};
    if (f < 0.99f)
        return {4, 5, result.percent, "Install or export"};
    return {5, 5, result.percent, f >= 1.0f ? "Complete" : "Verify"};
}

inline bool usesVistaWrapper(int target) {
    return target >= 1 && target <= 4;
}

inline bool requiredCookOutputsPresent(bool haveSystem,
                                       bool haveLegacyHaven,
                                       bool emitLegacyHaven,
                                       int target) {
    return haveSystem &&
           (usesVistaWrapper(target) || !emitLegacyHaven || haveLegacyHaven);
}

}  // namespace cookui
