#pragma once

namespace standalonehaven {

struct PackagingPlan {
    bool nuxdManifestIdentity;
    bool footprintManifestIdentity;
};

// The standalone replacement uses AstroBoy's device-proven NUXD combined
// Environment identity under the replaceable Haven package name. Explicit
// footprint mode remains a separate legacy workflow.
constexpr PackagingPlan plan(bool preserveFootprintMode) {
    return {!preserveFootprintMode, preserveFootprintMode};
}

}  // namespace standalonehaven
