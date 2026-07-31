#pragma once

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace installsafety {

enum class StandaloneHavenDecision {
    Allow,
    BlockInvalidV206Carrier,
};

inline StandaloneHavenDecision standaloneHavenDecision(
    bool replacingHaven,
    bool vrShell206OrNewer,
    bool provenCarrierValidated) {
    if (replacingHaven && vrShell206OrNewer) {
        if (!provenCarrierValidated)
            return StandaloneHavenDecision::BlockInvalidV206Carrier;
        return StandaloneHavenDecision::Allow;
    }
    return StandaloneHavenDecision::Allow;
}

inline uint64_t requiredFreeBytes(uint64_t apkBytes) {
    constexpr uint64_t kMiB = 1024ull * 1024ull;
    return std::max<uint64_t>(256ull * kMiB, apkBytes * 2ull + 128ull * kMiB);
}

inline uint64_t parseDfAvailableBytes(const std::string& output) {
    std::istringstream lines(output);
    std::string line;
    uint64_t available = 0;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::vector<std::string> tokens;
        std::string token;
        while (fields >> token) tokens.push_back(token);
        if (tokens.size() < 6 || tokens[0] == "Filesystem") continue;
        try {
            const uint64_t value = std::stoull(tokens[3]);
            available = value * 1024ull;
        } catch (...) {
        }
    }
    return available;
}

inline bool hasEnoughSpace(uint64_t availableBytes, uint64_t apkBytes) {
    return availableBytes == 0 || availableBytes >= requiredFreeBytes(apkBytes);
}

inline bool mayRetryAfterUninstall(const std::string& adbOutput) {
    return adbOutput.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos ||
           adbOutput.find("INSTALL_FAILED_VERSION_DOWNGRADE") != std::string::npos ||
           adbOutput.find("INSTALL_FAILED_ALREADY_EXISTS") != std::string::npos;
}

}  // namespace installsafety
