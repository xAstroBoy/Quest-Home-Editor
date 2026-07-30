#include "library/home_library_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testArtifactClassification() {
    using homelibrary::Kind;
    require(homelibrary::classifyFileName("Cyber_Rooted-System.apk").kind ==
                Kind::StandardRooted,
            "rooted output must appear under normal Homes");
    require(homelibrary::classifyFileName("Cyber_NoRoot-Spoof.apk").kind ==
                Kind::HavenSpoof,
            "Haven spoof must be identified");
    const auto calming =
        homelibrary::classifyFileName("Polar-Calming-Vista.apk");
    require(calming.kind == Kind::Vista && calming.vistaSlot == 1,
            "Calming Vista must map to slot 1");
    const auto horror =
        homelibrary::classifyFileName("Cyber-HORROR-VISTA.APK");
    require(horror.kind == Kind::Vista && horror.vistaSlot == 4,
            "Vista classification must be case-insensitive");
    require(homelibrary::classifyFileName("raw-home.apk").kind == Kind::Unknown,
            "raw APKs are sources, not finished library artifacts");
}

void testArtifactProvenance() {
    require(!homelibrary::isCurrentArtifactSignature("0123456789abcdef"),
            "legacy unversioned cooks must require an update");
    const std::string current =
        homelibrary::artifactSignature("0123456789abcdef");
    require(homelibrary::isCurrentArtifactSignature(current),
            "current versioned cooks must be installable");
    require(!homelibrary::isCurrentArtifactSignature("qhe-p1:0123456789abcdef"),
            "older pipeline revisions must require an update");
}

void testUserFolderPersistencePolicy() {
    const auto folders = homelibrary::uniqueFolderLines(
        {"  D:\\Homes  \r\n", "", "d:\\homes", "E:\\Quest Exports\n"});
    require(folders.size() == 2,
            "user folders must be trimmed and deduplicated case-insensitively");
    require(folders[0] == "D:\\Homes",
            "the first user-selected spelling must be preserved");
    require(folders[1] == "E:\\Quest Exports",
            "independent user folders must remain available");
}

}  // namespace

int main() {
    testArtifactClassification();
    testArtifactProvenance();
    testUserFolderPersistencePolicy();
    std::cout << "Home library policy tests passed\n";
    return 0;
}
