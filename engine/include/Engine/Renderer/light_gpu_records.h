// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/RHI/raw_table.h>
#include <Engine/RHI/FrameConfig.h>
#include <Engine/Rendergraph/forwardplus.h>
namespace tour::gpu_records {
// Full raw GPU light ABI, including frame-derived shadowTile/mapIndex.
// The long-lived source parameters must be kept separate from these
// frame-specific fields when the direct Forward+ writer is migrated.
using Lights = smvk::table::RawTable<smvk::ForwardPlusLight,
                                    smvk::limits::MaxFramesInFlight>;
static_assert(sizeof(smvk::ForwardPlusLight)==96);
}
