// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// The C++ side of the shared CPU/HLSL limits contract (see
// engine/shaders/engine_limits.hlsli for the full rationale). SINGLE C++
// INCLUDER of the dual-language file: it converts the macros to
// namespaced constexpr immediately below and #undefs everything, so no
// macro ever leaks past this header. Edit CPU/HLSL layout VALUES in the
// .hlsli — never their aliases. CPU-only world limits are defined here;
// never include the .hlsli from C++ anywhere else.
//
// The relative path below is deliberate (zero buildsystem machinery):
// it resolves against THIS file's location (engine/include/Engine/Core
// -> engine/shaders), so every consumer — engine, editor, tools, tests
// — sees the single source without needing engine/shaders on any
// include path.
#include "../../../shaders/engine_limits.hlsli"

namespace engine::limits {

inline constexpr uint32_t MaxMaterials = MAX_MATERIALS;
inline constexpr uint32_t MaxSimultaneousLights = MAX_SIMULTANEOUS_LIGHTS;
inline constexpr uint32_t MaxTextures = MAX_TEXTURES;
inline constexpr uint32_t MaxVolumeTextures = MAX_VOLUME_TEXTURES;
inline constexpr uint32_t MaxSamplers = MAX_SAMPLERS;

// CPU-side world-tile limits (1 engine world unit = 1 metre).
// A root streaming cell is 256 m wide; its subcells are 64 m wide.
// MaxWorldWidthMeters is the supported X/Z extent of ONE baked environment
// tile grid (16 km per axis), not a clamp on signed world coordinates:
// WorldPartition's int64 cell keys can address independent grids beyond it.
inline constexpr double WorldRootCellSizeMeters = 256.0;
inline constexpr double WorldSubcellSizeMeters = 64.0;
inline constexpr double MaxWorldWidthMeters = 16384.0;
inline constexpr uint32_t MaxWorldEnvTilesPerAxis =
    uint32_t(MaxWorldWidthMeters / WorldSubcellSizeMeters);

// uint64 path: one root sentinel + 2 bits per quadtree level.
inline constexpr uint32_t MaxWorldTileSubdivisionLevels = 31;

static_assert(WorldRootCellSizeMeters / WorldSubcellSizeMeters == 4.0);
static_assert(double(MaxWorldEnvTilesPerAxis) * WorldSubcellSizeMeters == MaxWorldWidthMeters);
static_assert(1u + 2u * MaxWorldTileSubdivisionLevels <= 64u);

} // namespace engine::limits

#undef MAX_MATERIALS
#undef MAX_SIMULTANEOUS_LIGHTS
#undef MAX_TEXTURES
#undef MAX_VOLUME_TEXTURES
#undef MAX_SAMPLERS
