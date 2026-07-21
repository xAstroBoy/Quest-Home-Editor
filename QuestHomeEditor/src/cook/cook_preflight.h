#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace hslcook::preflight {

// These are format limits, not tuning recommendations. The cooker writes classic
// ZIP archives and 32-bit mesh counts/indices. Every non-empty source mesh produces
// at least one archive asset, so the classic-ZIP entry ceiling is the only honest
// scene-count bound here; the later archive preflight validates the exact fan-out.
struct Limits {
    static constexpr size_t kClassicZipEntryLimit = 65535;
    static constexpr size_t kReservedArchiveEntries = 256;

    size_t maxMeshes = kClassicZipEntryLimit - kReservedArchiveEntries;
    size_t maxVerticesPerMesh = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
    size_t maxIndicesPerMesh = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
    size_t maxReportedIssues = 128;
};

struct GeometryView {
    const char* name = nullptr;
    const float* positions = nullptr;
    size_t positionComponents = 0;
    const float* uvs = nullptr;
    size_t uvComponents = 0;
    const uint32_t* indices = nullptr;
    size_t indexCount = 0;
};

enum class IssueCode {
    TooManyMeshes,
    PositionComponentCount,
    TooFewVertices,
    TooManyVertices,
    NonFinitePosition,
    NonFiniteUv,
    IndexCountNotTriangles,
    TooFewIndices,
    TooManyIndices,
    IndexOutOfRange,
};

struct Issue {
    IssueCode code = IssueCode::TooFewVertices;
    size_t meshIndex = 0;
    std::string meshName;
    size_t elementIndex = 0;
    uint64_t actual = 0;
    uint64_t expectedOrLimit = 0;
};

struct Report {
    size_t meshCount = 0;
    uint64_t vertexCount = 0;
    uint64_t triangleCount = 0;
    size_t issueCount = 0;
    std::vector<Issue> issues;

    bool ok() const { return issueCount == 0; }
    size_t omittedIssueCount() const {
        return issueCount > issues.size() ? issueCount - issues.size() : 0;
    }
};

inline void addIssue(Report& report, const Limits& limits, Issue issue) {
    ++report.issueCount;
    if (report.issues.size() < limits.maxReportedIssues)
        report.issues.push_back(std::move(issue));
}

inline Issue makeIssue(IssueCode code, size_t meshIndex, const GeometryView& mesh,
                       size_t elementIndex = 0, uint64_t actual = 0,
                       uint64_t expectedOrLimit = 0) {
    Issue issue;
    issue.code = code;
    issue.meshIndex = meshIndex;
    issue.meshName = mesh.name ? mesh.name : "";
    issue.elementIndex = elementIndex;
    issue.actual = actual;
    issue.expectedOrLimit = expectedOrLimit;
    return issue;
}

inline Report validateScene(const GeometryView* meshes, size_t meshCount,
                            const Limits& limits = {}) {
    Report report;
    report.meshCount = meshCount;
    if (meshCount > limits.maxMeshes) {
        GeometryView scene;
        scene.name = "<scene>";
        addIssue(report, limits,
                 makeIssue(IssueCode::TooManyMeshes, 0, scene, 0,
                           static_cast<uint64_t>(meshCount),
                           static_cast<uint64_t>(limits.maxMeshes)));
    }

    if (!meshes && meshCount != 0) {
        GeometryView scene;
        scene.name = "<scene>";
        addIssue(report, limits,
                 makeIssue(IssueCode::TooFewVertices, 0, scene));
        return report;
    }

    for (size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        const GeometryView& mesh = meshes[meshIndex];

        // Empty draw records have historically been harmless and are skipped by
        // the cook loop. Preserve that compatibility; partially empty geometry
        // still fails with an actionable vertex/index error below.
        if (mesh.positionComponents == 0 && mesh.indexCount == 0) continue;

        const bool positionLayoutOk = mesh.positionComponents % 3 == 0;
        if (!positionLayoutOk) {
            addIssue(report, limits,
                     makeIssue(IssueCode::PositionComponentCount, meshIndex, mesh, 0,
                               static_cast<uint64_t>(mesh.positionComponents), 3));
        }
        const size_t vertices = mesh.positionComponents / 3;
        report.vertexCount += static_cast<uint64_t>(vertices);
        if (vertices < 3) {
            addIssue(report, limits,
                     makeIssue(IssueCode::TooFewVertices, meshIndex, mesh, 0,
                               static_cast<uint64_t>(vertices), 3));
        }
        if (vertices > limits.maxVerticesPerMesh) {
            addIssue(report, limits,
                     makeIssue(IssueCode::TooManyVertices, meshIndex, mesh, 0,
                               static_cast<uint64_t>(vertices),
                               static_cast<uint64_t>(limits.maxVerticesPerMesh)));
        }
        if (mesh.positionComponents != 0 && !mesh.positions) {
            addIssue(report, limits,
                     makeIssue(IssueCode::NonFinitePosition, meshIndex, mesh));
        } else {
            for (size_t i = 0; i < mesh.positionComponents; ++i) {
                if (!std::isfinite(mesh.positions[i])) {
                    addIssue(report, limits,
                             makeIssue(IssueCode::NonFinitePosition, meshIndex, mesh, i));
                    break;
                }
            }
        }

        // UVs are optional and short streams intentionally use the cooker's 0,0
        // fallback. Reject only non-finite supplied data; do not tighten the
        // long-standing layout compatibility as part of a geometry-safety check.
        if (mesh.uvComponents != 0) {
            if (!mesh.uvs) {
                addIssue(report, limits,
                         makeIssue(IssueCode::NonFiniteUv, meshIndex, mesh));
            } else {
                for (size_t i = 0; i < mesh.uvComponents; ++i) {
                    if (!std::isfinite(mesh.uvs[i])) {
                        addIssue(report, limits,
                                 makeIssue(IssueCode::NonFiniteUv, meshIndex, mesh, i));
                        break;
                    }
                }
            }
        }

        if (mesh.indexCount % 3 != 0) {
            addIssue(report, limits,
                     makeIssue(IssueCode::IndexCountNotTriangles, meshIndex, mesh, 0,
                               static_cast<uint64_t>(mesh.indexCount), 3));
        }
        report.triangleCount += static_cast<uint64_t>(mesh.indexCount / 3);
        if (mesh.indexCount < 3) {
            addIssue(report, limits,
                     makeIssue(IssueCode::TooFewIndices, meshIndex, mesh, 0,
                               static_cast<uint64_t>(mesh.indexCount), 3));
        }
        if (mesh.indexCount > limits.maxIndicesPerMesh) {
            addIssue(report, limits,
                     makeIssue(IssueCode::TooManyIndices, meshIndex, mesh, 0,
                               static_cast<uint64_t>(mesh.indexCount),
                               static_cast<uint64_t>(limits.maxIndicesPerMesh)));
        }
        if (mesh.indexCount != 0 && !mesh.indices) {
            addIssue(report, limits,
                     makeIssue(IssueCode::IndexOutOfRange, meshIndex, mesh));
        } else {
            for (size_t i = 0; i < mesh.indexCount; ++i) {
                if (mesh.indices[i] >= vertices) {
                    addIssue(report, limits,
                             makeIssue(IssueCode::IndexOutOfRange, meshIndex, mesh, i,
                                       static_cast<uint64_t>(mesh.indices[i]),
                                       static_cast<uint64_t>(vertices)));
                    break;
                }
            }
        }
    }
    return report;
}

inline const char* issueCodeName(IssueCode code) {
    switch (code) {
        case IssueCode::TooManyMeshes: return "too many source meshes";
        case IssueCode::PositionComponentCount: return "position component count is not divisible by 3";
        case IssueCode::TooFewVertices: return "mesh has fewer than 3 vertices";
        case IssueCode::TooManyVertices: return "mesh exceeds the 32-bit vertex limit";
        case IssueCode::NonFinitePosition: return "position contains NaN/Infinity";
        case IssueCode::NonFiniteUv: return "UV stream contains NaN/Infinity";
        case IssueCode::IndexCountNotTriangles: return "index count is not divisible by 3";
        case IssueCode::TooFewIndices: return "mesh has fewer than 3 indices";
        case IssueCode::TooManyIndices: return "mesh exceeds the 32-bit index-count limit";
        case IssueCode::IndexOutOfRange: return "index references a vertex outside the mesh";
    }
    return "unknown cook preflight issue";
}

} // namespace hslcook::preflight
