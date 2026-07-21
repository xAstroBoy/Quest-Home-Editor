#include "cook/zip_safety.h"

#include <cstdlib>
#include <iostream>

namespace {

using hslcook::zipsafety::ArchiveReadBudget;
using hslcook::zipsafety::ArchiveReadLimits;
using hslcook::zipsafety::ArchiveReadStatus;
using hslcook::zipsafety::NameStatus;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testAcceptsNormalLargeHome() {
    ArchiveReadBudget budget({8, 1024, 4096});
    require(budget.add("content/assets.manifest", 128).status == ArchiveReadStatus::Ok,
            "manifest is accepted");
    require(budget.add("content/world/model.mesh", 900).status == ArchiveReadStatus::Ok,
            "large ordinary asset is accepted");
    require(budget.add("content/world/textures/", 0, true).status == ArchiveReadStatus::Ok,
            "directory placeholder is accepted explicitly");
    require(budget.result().entryCount == 3, "accepted entries are counted");
    require(budget.result().totalUncompressedBytes == 1028,
            "declared uncompressed bytes are accumulated exactly");
}

void testEntryAndTotalLimits() {
    ArchiveReadBudget perEntry({4, 100, 1000});
    require(perEntry.add("content/huge.bin", 101).status == ArchiveReadStatus::EntryTooLarge,
            "one oversized entry is rejected before extraction");

    ArchiveReadBudget total({4, 100, 150});
    require(total.add("content/a.bin", 100).status == ArchiveReadStatus::Ok,
            "first entry fits total budget");
    require(total.add("content/b.bin", 51).status == ArchiveReadStatus::TotalTooLarge,
            "cumulative declared bytes cannot exceed the budget");

    ArchiveReadBudget count({1, 100, 100});
    require(count.add("content/a", 1).status == ArchiveReadStatus::Ok,
            "first entry fits count budget");
    require(count.add("content/b", 1).status == ArchiveReadStatus::TooManyEntries,
            "entry count limit is enforced");
}

void testNamesAndStickyFailure() {
    ArchiveReadBudget unsafe({8, 100, 1000});
    auto result = unsafe.add("../outside.exe", 1);
    require(result.status == ArchiveReadStatus::UnsafeName,
            "parent traversal is rejected");
    require(result.nameStatus == NameStatus::ParentTraversal,
            "the precise unsafe-name reason is retained");
    require(unsafe.add("content/good.bin", 1).status == ArchiveReadStatus::UnsafeName,
            "a failed budget remains failed");

    ArchiveReadBudget duplicate({8, 100, 1000});
    require(duplicate.add("content/models/city.mesh", 1).status == ArchiveReadStatus::Ok,
            "first normalized name is accepted");
    require(duplicate.add("content//models/./city.mesh", 1).status == ArchiveReadStatus::DuplicateName,
            "ambiguous normalized duplicate is rejected");
}

} // namespace

int main() {
    testAcceptsNormalLargeHome();
    testEntryAndTotalLimits();
    testNamesAndStickyFailure();
    std::cout << "archive_read_limits_tests: all checks passed\n";
    return 0;
}
