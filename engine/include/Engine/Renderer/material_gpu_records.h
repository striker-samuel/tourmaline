// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/RHI/indexed_raw_table.h>
#include <Engine/RHI/FrameConfig.h>
#include <Engine/Renderer/mesh.h>

namespace tour::gpu_records {
// Authored material identity remains engine-owned; raw GPU ABI is unchanged.
// Stable external indices match MaterialsBuffer.mats[worldMaterialId].
// CPU material pools remain the host source; this publishes resolved GPU rows.
using Materials = smvk::table::IndexedRawTable<tour::MaterialValues,
                                       smvk::limits::MaxFramesInFlight>;
static_assert(sizeof(tour::MaterialValues)==128);
}
