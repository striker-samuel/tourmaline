// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/RHI/raw_table.h>
#include <Engine/RHI/FrameConfig.h>
#include <Engine/Renderer/gfx_types.h>
namespace tour::gpu_records {
// TextureMeta is addressed by BINDLESS slot in existing shaders. Stable
// record handles and bindless indices differ: a future producer must keep an
// explicit mapping rather than assume generation slots == descriptor slots.
using TextureMetadata = smvk::table::RawTable<tour::gfx::TextureMeta,
                                              smvk::limits::MaxFramesInFlight>;
static_assert(sizeof(tour::gfx::TextureMeta) == 8);
}
