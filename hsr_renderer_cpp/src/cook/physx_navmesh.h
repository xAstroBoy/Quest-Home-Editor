// ── physx_navmesh.h — INTEGRATED navmesh/collider cook (declaration only; no PhysX headers leak here) ───────────
// The cooker calls cookNavmeshSEBD() DIRECTLY (no external exe): it PhysX-cooks arbitrary world triangles into the
// exact haven2025 `SEBD` + GUID 77E92B17... collision mesh (platform tag patched to ANDR). Returns empty on failure.
// Implemented in physx_navmesh.cpp, which is the only TU that includes PxPhysicsAPI.h + links the vendored PhysX libs.
#pragma once
#include <vector>
#include <cstdint>

std::vector<uint8_t> cookNavmeshSEBD(const float* verts, uint32_t nVerts, const uint32_t* idx, uint32_t nTris);
