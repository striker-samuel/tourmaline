// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/RHI/raw_table.h>
#include <Engine/RHI/FrameConfig.h>
#include <Engine/Rendergraph/forwardplus.h>
namespace tour::gpu_records {
// Probes are presently encoded as ForwardPlusLight with type=Probe and
// appended AFTER direct lights. This table retains the exact packed ABI,
// but must be COMPOSED into the existing light+probe buffer in light-first
// order; it must not be bound as a separate probe buffer by itself.
using Probes = smvk::table::RawTable<smvk::ForwardPlusLight,
                                    smvk::limits::MaxFramesInFlight>;
inline bool IsProbe(const smvk::ForwardPlusLight& record) {
    return record.type == smvk::kForwardPlusProbe;
}
}
