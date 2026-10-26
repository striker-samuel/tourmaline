// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// The C++ side of the shared CPU/HLSL limits contract (see
// engine/shaders/engine_limits.hlsli for the full rationale). SINGLE C++
// INCLUDER of the dual-language file: it converts the macros to
// namespaced constexpr immediately below and #undefs everything, so no
// macro ever leaks past this header. Edit VALUES in the .hlsli — never
// the aliases — and never include the .hlsli from C++ anywhere else.
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

} // namespace engine::limits

#undef MAX_MATERIALS
#undef MAX_SIMULTANEOUS_LIGHTS
#undef MAX_TEXTURES
#undef MAX_VOLUME_TEXTURES
#undef MAX_SAMPLERS
