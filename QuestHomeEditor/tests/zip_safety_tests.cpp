#include "cook/zip_safety.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using hslcook::zipsafety::EntryNameRegistry;
using hslcook::zipsafety::NameStatus;
using hslcook::zipsafety::StoredEntrySize;
using hslcook::zipsafety::StoredZipLayout;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testPathNormalization() {
    std::string normalized;
    require(hslcook::zipsafety::normalizeEntryName("assets/scene.zip", normalized) == NameStatus::Ok,
            "ordinary asset path is accepted");
    require(normalized == "assets/scene.zip", "ordinary path is unchanged");

    require(hslcook::zipsafety::normalizeEntryName("content//./models\\city.mesh", normalized) == NameStatus::Ok,
            "mixed separators and dot segments are accepted");
    require(normalized == "content/models/city.mesh", "path is normalized deterministically");

    require(hslcook::zipsafety::normalizeEntryName("/assets/scene.zip", normalized) == NameStatus::Absolute,
            "POSIX absolute paths are rejected");
    require(hslcook::zipsafety::normalizeEntryName("\\\\server\\share\\file", normalized) == NameStatus::Absolute,
            "UNC paths are rejected");
    require(hslcook::zipsafety::normalizeEntryName("C:\\temp\\file", normalized) == NameStatus::DriveQualified,
            "drive-qualified paths are rejected");
    require(hslcook::zipsafety::normalizeEntryName("content/../scene.zip", normalized) == NameStatus::ParentTraversal,
            "parent traversal is rejected");
    require(hslcook::zipsafety::normalizeEntryName("content/file:stream", normalized) == NameStatus::InvalidCharacter,
            "alternate data stream names are rejected");
    require(hslcook::zipsafety::normalizeEntryName("content/folder/", normalized) == NameStatus::Directory,
            "file writer rejects directory placeholders");
    require(hslcook::zipsafety::normalizeEntryName("content/folder/", normalized, true) == NameStatus::Ok,
            "reader can explicitly validate directory placeholders");
}

void testDuplicateDetection() {
    EntryNameRegistry names;
    std::string normalized;
    require(names.add("content/models/city.mesh", &normalized) == NameStatus::Ok,
            "first entry is accepted");
    require(names.add("content//models\\city.mesh") == NameStatus::Duplicate,
            "equivalent normalized path is rejected as a duplicate");
    require(names.add("content/assets.manifest") == NameStatus::Ok,
            "reserved name can be registered once");
    require(names.add("content/./assets.manifest") == NameStatus::Duplicate,
            "reserved-name conflict is detected after normalization");
}

void testStoredZipLimits() {
    StoredZipLayout layout;
    require(hslcook::zipsafety::computeStoredZipLayout({{5, 10}}, layout),
            "small stored archive fits classic ZIP");
    require(layout.centralDirectoryOffset == 45, "local layout size is exact");
    require(layout.centralDirectoryBytes == 51, "central-directory layout size is exact");
    require(layout.totalBytes == 118, "complete archive layout size is exact");

    require(!hslcook::zipsafety::computeStoredZipLayout({{0, 10}}, layout),
            "empty names are rejected");
    require(!hslcook::zipsafety::computeStoredZipLayout({{70000, 10}}, layout),
            "names that cannot fit the ZIP16 field are rejected");
    require(!hslcook::zipsafety::computeStoredZipLayout({{1, hslcook::zipsafety::kZip32Max}}, layout),
            "archives whose headers push them past ZIP32 are rejected");

    std::vector<StoredEntrySize> tooMany(65536, StoredEntrySize{1, 0});
    require(!hslcook::zipsafety::computeStoredZipLayout(tooMany, layout),
            "entry counts that require ZIP64 are rejected");
}

} // namespace

int main() {
    testPathNormalization();
    testDuplicateDetection();
    testStoredZipLimits();
    std::cout << "zip_safety_tests: all checks passed\n";
    return 0;
}
