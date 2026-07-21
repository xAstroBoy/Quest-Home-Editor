#include "cook/cook_preflight.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using hslcook::preflight::GeometryView;
using hslcook::preflight::IssueCode;
using hslcook::preflight::Limits;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

GeometryView view(const char* name, const std::vector<float>& positions,
                  const std::vector<float>& uvs,
                  const std::vector<uint32_t>& indices) {
    return {name, positions.data(), positions.size(), uvs.data(), uvs.size(),
            indices.data(), indices.size()};
}

bool hasIssue(const hslcook::preflight::Report& report, IssueCode code,
              size_t meshIndex = 0) {
    for (const auto& issue : report.issues)
        if (issue.code == code && issue.meshIndex == meshIndex) return true;
    return false;
}

const hslcook::preflight::Issue* findIssue(
        const hslcook::preflight::Report& report, IssueCode code,
        size_t meshIndex = 0) {
    for (const auto& issue : report.issues)
        if (issue.code == code && issue.meshIndex == meshIndex) return &issue;
    return nullptr;
}

void testValidGeometryIsReadOnly() {
    std::vector<float> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<float> uvs{0, 0, 1, 0, 0, 1};
    std::vector<uint32_t> indices{0, 1, 2};
    const auto positionsBefore = positions;
    const auto uvsBefore = uvs;
    const auto indicesBefore = indices;
    const GeometryView geometry = view("valid", positions, uvs, indices);

    const auto report = hslcook::preflight::validateScene(&geometry, 1);
    require(report.ok(), "valid triangle passes preflight");
    require(report.vertexCount == 3 && report.triangleCount == 1,
            "valid scene summary is exact");
    require(positions == positionsBefore && uvs == uvsBefore && indices == indicesBefore,
            "preflight never changes valid source geometry");
}

void testMalformedGeometryReportsPreciseFailures() {
    std::vector<float> positions{0, 0, 0, 1, 0, 0,
                                 0, std::numeric_limits<float>::infinity(), 0, 7};
    std::vector<float> uvs{0, 0, 1, 0, 0};
    std::vector<uint32_t> indices{0, 1, 9, 0};
    const GeometryView geometry = view("broken", positions, uvs, indices);

    const auto report = hslcook::preflight::validateScene(&geometry, 1);
    require(!report.ok(), "malformed mesh fails preflight");
    require(hasIssue(report, IssueCode::PositionComponentCount),
            "misaligned position stream is reported");
    require(hasIssue(report, IssueCode::NonFinitePosition),
            "non-finite position is reported");
    require(hasIssue(report, IssueCode::IndexCountNotTriangles),
            "non-triangle index list is reported");
    require(hasIssue(report, IssueCode::IndexOutOfRange),
            "out-of-range index is reported");
    const auto* badPosition = findIssue(report, IssueCode::NonFinitePosition);
    const auto* badIndex = findIssue(report, IssueCode::IndexOutOfRange);
    require(badPosition && badPosition->elementIndex == 7,
            "position error identifies the exact component");
    require(badIndex && badIndex->elementIndex == 2 && badIndex->actual == 9 &&
                badIndex->expectedOrLimit == 3,
            "index error identifies the exact slot, value, and vertex count");

    std::vector<float> validPositions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<float> badUvs{0, 0, std::numeric_limits<float>::quiet_NaN(), 0, 0, 1};
    std::vector<uint32_t> validIndices{0, 1, 2};
    const GeometryView badUvGeometry = view("bad_uv", validPositions, badUvs, validIndices);
    const auto uvReport = hslcook::preflight::validateScene(&badUvGeometry, 1);
    require(hasIssue(uvReport, IssueCode::NonFiniteUv),
            "non-finite UV data is reported before encoding");
}

void testOptionalUvsRemainSupported() {
    std::vector<float> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<float> noUvs;
    std::vector<uint32_t> indices{0, 1, 2};
    const GeometryView geometry = view("no_uv", positions, noUvs, indices);
    const auto report = hslcook::preflight::validateScene(&geometry, 1);
    require(report.ok(), "missing optional UVs use the existing zero-UV fallback");

    std::vector<float> shortUvs{0, 0};
    const GeometryView shortGeometry = view("short_uv", positions, shortUvs, indices);
    require(hslcook::preflight::validateScene(&shortGeometry, 1).ok(),
            "short legacy UV streams keep the existing zero-UV fallback");

    GeometryView empty;
    empty.name = "empty_placeholder";
    require(hslcook::preflight::validateScene(&empty, 1).ok(),
            "empty draw placeholders keep the existing skip behavior");
}

void testHardLimitsAreEnforced() {
    std::vector<float> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<float> uvs{0, 0, 1, 0, 0, 1};
    std::vector<uint32_t> indices{0, 1, 2};
    const GeometryView geometries[] = {
        view("first", positions, uvs, indices),
        view("second", positions, uvs, indices),
    };
    Limits limits;
    require(limits.maxMeshes > 1000,
            "default format limit does not recreate the legacy 1000-mesh cap");
    limits.maxMeshes = 1;
    limits.maxVerticesPerMesh = 2;
    limits.maxIndicesPerMesh = 2;

    const auto report = hslcook::preflight::validateScene(geometries, 2, limits);
    require(hasIssue(report, IssueCode::TooManyMeshes),
            "scene mesh hard limit is reported");
    require(hasIssue(report, IssueCode::TooManyVertices, 0),
            "per-mesh vertex hard limit is reported");
    require(hasIssue(report, IssueCode::TooManyIndices, 0),
            "per-mesh index hard limit is reported");
}

void testIssueReportingIsBounded() {
    std::vector<float> positions{0, 0, 0};
    std::vector<float> noUvs;
    std::vector<uint32_t> noIndices;
    GeometryView geometries[4] = {
        view("a", positions, noUvs, noIndices),
        view("b", positions, noUvs, noIndices),
        view("c", positions, noUvs, noIndices),
        view("d", positions, noUvs, noIndices),
    };
    Limits limits;
    limits.maxReportedIssues = 2;
    const auto report = hslcook::preflight::validateScene(geometries, 4, limits);
    require(report.issueCount > report.issues.size(),
            "all issue counts are retained when details are bounded");
    require(report.issues.size() == 2 && report.omittedIssueCount() > 0,
            "detail list honors its deterministic cap");
}

} // namespace

int main() {
    testValidGeometryIsReadOnly();
    testMalformedGeometryReportsPreciseFailures();
    testOptionalUvsRemainSupported();
    testHardLimitsAreEnforced();
    testIssueReportingIsBounded();
    std::cout << "cook_preflight_tests: all checks passed\n";
    return 0;
}
