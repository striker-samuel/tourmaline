// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/RHI/raw_table.h>
#include <Engine/RHI/FrameConfig.h>
#include <Engine/Renderer/mesh.h>

namespace tour::gpu_records {
// Authored material identity remains engine-owned; raw GPU ABI is unchanged.
// This is the table type used for a future replacement of per-frame material
// slab uploads; no direct-renderer consumer is switched in this PR.
using Materials = smvk::table::RawTable<tour::MaterialValues,
                                       smvk::limits::MaxFramesInFlight>;
static_assert(sizeof(tour::MaterialValues)==128);
}
