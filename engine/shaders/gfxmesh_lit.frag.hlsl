// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Mesh-path twin of default_lit.frag.hlsl (classic alpha-test cutout).
//
// Same body (default_lit_body.hlsli), but the material comes from a flat
// mesh-shader varying (location 7) instead of the per-range
// pc.materialId push — one DrawMeshTasks dispatch covers many materials.
// Varyings 0..6 match the indexed VSOut exactly; only location 7 is new,
// so this links ONLY against the mesh shader (gfxfilter.mesh.hlsl), never
// the classic VS (which would leave location 7 unwritten).
//
// C++ side: GfxRenderer mesh twin program { mesh, this FS }.

#define GFXMESH_FS_PASS
#include "lit_dependencies.hlsli"
#include "gfxmesh_lit_decl.hlsli"
// The STANDARD no-op hook family (a project lit family includes its own
// hook library here instead — same body, same seam).
#include "lit_hooks_default.hlsli"
#include "default_lit_body.hlsli"
