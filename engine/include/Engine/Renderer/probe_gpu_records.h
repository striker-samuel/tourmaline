// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/RHI/raw_table.h>
#include <Engine/RHI/FrameConfig.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <type_traits>
namespace tour::gpu_records {
// Probes presently occupy ForwardPlusLight GPU records, appended AFTER
// direct lights. Use a distinct wrapper to give probe handles a separate
// type while maintaining the exact 96-byte packed shader representation.
struct alignas(16) ProbePayload {
    smvk::ForwardPlusLight packed{};
};
static_assert(sizeof(ProbePayload)==sizeof(smvk::ForwardPlusLight));
static_assert(std::is_standard_layout_v<ProbePayload>);
static_assert(std::is_trivially_copyable_v<ProbePayload>);
using Probes = smvk::table::RawTable<ProbePayload,
                                    smvk::limits::MaxFramesInFlight>;
inline bool IsProbe(const ProbePayload& record) {
    return record.packed.type == smvk::kForwardPlusProbe;
}
// The producer MUST compose this data into the light+probe buffer in
// light-first order. Separate GPU probe addressing requires a shader ABI PR.
}
