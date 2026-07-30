#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace homelibrary {

// Increment whenever cooker/wrapper behavior changes in a way that requires
// finished library artifacts to be rebuilt. Legacy 16-hex cook signatures did
// not carry provenance and must never be presented as current.
inline constexpr int kArtifactPipelineRevision = 2;
inline constexpr std::string_view kArtifactSignaturePrefix = "qhe-p2:";

inline std::string artifactSignature(std::string_view contentHash) {
    return std::string(kArtifactSignaturePrefix) + std::string(contentHash);
}

inline bool isCurrentArtifactSignature(std::string_view signature) {
    return signature.size() == kArtifactSignaturePrefix.size() + 16 &&
           signature.substr(0, kArtifactSignaturePrefix.size()) ==
               kArtifactSignaturePrefix;
}

inline std::string trimPathLine(std::string value) {
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' ||
            value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    size_t first = 0;
    while (first < value.size() &&
           (value[first] == ' ' || value[first] == '\t'))
        ++first;
    value.erase(0, first);
    return value;
}

inline std::vector<std::string> uniqueFolderLines(
    const std::vector<std::string>& lines) {
    const auto fold = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return value;
    };
    std::vector<std::string> result;
    for (std::string value : lines) {
        value = trimPathLine(std::move(value));
        if (value.empty())
            continue;
        const std::string folded = fold(value);
        const auto duplicate = std::find_if(
            result.begin(), result.end(), [&](const std::string& existing) {
                return fold(existing) == folded;
            });
        if (duplicate == result.end())
            result.push_back(std::move(value));
    }
    return result;
}

enum class Kind {
    Unknown,
    StandardRooted,
    HavenSpoof,
    Vista,
};

struct Classification {
    Kind kind = Kind::Unknown;
    int vistaSlot = 0;
    const char* destination = "Unknown";
};

inline std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline Classification classifyFileName(const std::string& fileName) {
    const std::string low = lowerAscii(fileName);
    if (low.size() < 4 || low.substr(low.size() - 4) != ".apk")
        return {};
    if (low.find("-calming-vista.apk") != std::string::npos)
        return {Kind::Vista, 1, "Calming"};
    if (low.find("-focused-vista.apk") != std::string::npos)
        return {Kind::Vista, 2, "Focused"};
    if (low.find("-oceanarium-vista.apk") != std::string::npos)
        return {Kind::Vista, 3, "Oceanarium"};
    if (low.find("-horror-vista.apk") != std::string::npos)
        return {Kind::Vista, 4, "Horror"};
    if (low.find("_noroot-spoof.apk") != std::string::npos)
        return {Kind::HavenSpoof, 0, "Haven 2025"};
    if (low.find("_rooted-system.apk") != std::string::npos)
        return {Kind::StandardRooted, 0, "Standard Home"};
    return {};
}

inline const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::StandardRooted: return "Rooted Home";
        case Kind::HavenSpoof: return "Spoofed Home";
        case Kind::Vista: return "Vista Home";
        default: return "Other APK";
    }
}

}  // namespace homelibrary
