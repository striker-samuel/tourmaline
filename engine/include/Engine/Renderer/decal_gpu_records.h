// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/RHI/raw_table.h>
#include <Engine/RHI/FrameConfig.h>
#include <Engine/Rendergraph/forwardplus.h>
namespace tour::gpu_records {
// ForwardPlusDecal is a WORLD-space, frame-packaged GPU descriptor.
// The WorldPage SceneDecal remains the authored host abstraction.
using Decals = smvk::table::RawTable<smvk::ForwardPlusDecal,
                                    smvk::limits::MaxFramesInFlight>;
static_assert(sizeof(smvk::ForwardPlusDecal)==128);
}
