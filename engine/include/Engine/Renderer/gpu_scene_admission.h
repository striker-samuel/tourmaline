// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// GPUScene raster admission classification (Epoch 0 observability).
//
// The raster admission gate (WorldRenderer's gpu-scene candidate loop) decides
// whether a World draw item can be GPU-scene-admitted or must fall back to the
// legacy direct path. The FAIL-CLOSED guard aborts when ANY item is rejected,
// but the raw "compatible vs legacy" count does not say WHY. This enum names the
// first-failing gate in short-circuit evaluation order so a rejection histogram
// can explain an oversubscribed scene instead of just counting it.
//
// The categories are derived from the actual admission chain (WorldRenderer.cpp,
// the `commonEligible` && flagsOk decomposition), NOT an assumed taxonomy.

#include <array>
#include <cstdint>

namespace tour {
namespace gpu_scene {

enum class GpuSceneAdmissionFailure : uint32_t {
    Admitted = 0,          // passed every gate
    MissingInstanceRecord, // no DB registration for the source key (stale/retired)
    InstanceNotReady,      // RasterIndexedInstanceReady false (geometry/bounds not resident)
    MaterialNotEligible,   // no material, or OpaqueLayeredRasterEligible false
    ProjectLitNotReady,    // ProjectLit material family without its raster program
    NodeInvalid,           // node id >= globalTransform.size() (stale node)
    DitherCloseup,         // closeup-dither node (no matching masked/depth variant)
    MissingMesh,           // no static mesh resolved for the draw item
    LightmapUnsupported,   // packed vertex lightmap (no GPU-scene lit variant yet)
    LodGeometryMismatch,   // LOD bounds union / resident submesh descriptor mismatch
    FlagsMissingRequired,  // missing a required iterator flag bit
    FlagsForbidden,        // carries a forbidden iterator flag bit (e.g. Transparent on opaque)
    Count,
};

constexpr uint32_t kAdmissionFailureCount = static_cast<uint32_t>(GpuSceneAdmissionFailure::Count);

inline const char* AdmissionFailureName(GpuSceneAdmissionFailure f) {
    switch (f) {
    case GpuSceneAdmissionFailure::Admitted:
        return "admitted";
    case GpuSceneAdmissionFailure::MissingInstanceRecord:
        return "missing-instance-record";
    case GpuSceneAdmissionFailure::InstanceNotReady:
        return "instance-not-ready";
    case GpuSceneAdmissionFailure::MaterialNotEligible:
        return "material-not-eligible";
    case GpuSceneAdmissionFailure::ProjectLitNotReady:
        return "projectlit-not-ready";
    case GpuSceneAdmissionFailure::NodeInvalid:
        return "node-invalid";
    case GpuSceneAdmissionFailure::DitherCloseup:
        return "dither-closeup";
    case GpuSceneAdmissionFailure::MissingMesh:
        return "missing-mesh";
    case GpuSceneAdmissionFailure::LightmapUnsupported:
        return "lightmap-unsupported";
    case GpuSceneAdmissionFailure::LodGeometryMismatch:
        return "lod-geometry-mismatch";
    case GpuSceneAdmissionFailure::FlagsMissingRequired:
        return "flags-missing-required";
    case GpuSceneAdmissionFailure::FlagsForbidden:
        return "flags-forbidden";
    default:
        return "unknown";
    }
}

// Per-frame rejection histogram: one count per category, plus the admitted
// total. Cheap integer work (the gates are already evaluated by the admission
// chain); a caller can format it for the OVERSUBSCRIPTION abort or a
// ResidencyDiagnostics snapshot.
struct GpuSceneAdmissionHistogram {
    std::array<uint32_t, kAdmissionFailureCount> counts{};

    void Record(GpuSceneAdmissionFailure f) {
        ++counts[static_cast<uint32_t>(f)];
    }
    uint32_t Admitted() const {
        return counts[static_cast<uint32_t>(GpuSceneAdmissionFailure::Admitted)];
    }
    uint32_t TotalRejected() const {
        uint32_t n = 0;
        for (uint32_t i = 1; i < kAdmissionFailureCount; ++i)
            n += counts[i];
        return n;
    }
};

// Four-way publication/admission classification (Epoch S0). The renderer's
// decision, not a diagnostic: a streaming-late instance must NEVER fall back
// to CPU draw, and must NEVER trip the unsupported-content assertion — it is
// HELD (no GPU draw, no CPU draw, retried next frame) until its outstanding
// publication dependency resolves.
//
//   Ready       instance + required geometry published and valid  -> GPUScene draws
//   Held        valid, outstanding streaming/publication work     -> no draw, retry
//   Unsupported cannot be represented by the GPUScene raster path -> explicit failure / authorized
//   fallback Invalid     broken invariants (stale gen, dangling mesh)      -> fail-closed
enum class GpuSceneAdmission : uint8_t {
    Ready = 0,
    Held,
    Unsupported,
    Invalid,
    Count,
};

// Maps a first-failing gate to its four-way class. The transient-vs-broken
// line: MissingInstanceRecord / InstanceNotReady / ProjectLitNotReady /
// LodGeometryMismatch are outstanding-publication states (a valid instance
// whose geometry/program is still reconciling), so they are HELD, never
// unsupported and never invalid. Material/lightmap/dither/flags are semantic
// "cannot represent" cases; node-invalid and missing-mesh are broken
// invariants (a draw item referencing retired content).
inline GpuSceneAdmission ClassifyAdmission(GpuSceneAdmissionFailure f) {
    switch (f) {
    case GpuSceneAdmissionFailure::Admitted:
        return GpuSceneAdmission::Ready;
    case GpuSceneAdmissionFailure::MissingInstanceRecord:
    case GpuSceneAdmissionFailure::InstanceNotReady:
    case GpuSceneAdmissionFailure::ProjectLitNotReady:
    case GpuSceneAdmissionFailure::LodGeometryMismatch:
        return GpuSceneAdmission::Held;
    case GpuSceneAdmissionFailure::MaterialNotEligible:
    case GpuSceneAdmissionFailure::DitherCloseup:
    case GpuSceneAdmissionFailure::LightmapUnsupported:
    case GpuSceneAdmissionFailure::FlagsMissingRequired:
    case GpuSceneAdmissionFailure::FlagsForbidden:
        return GpuSceneAdmission::Unsupported;
    case GpuSceneAdmissionFailure::NodeInvalid:
    case GpuSceneAdmissionFailure::MissingMesh:
        return GpuSceneAdmission::Invalid;
    default:
        return GpuSceneAdmission::Invalid;
    }
}

inline const char* AdmissionClassName(GpuSceneAdmission a) {
    switch (a) {
    case GpuSceneAdmission::Ready:
        return "ready";
    case GpuSceneAdmission::Held:
        return "held";
    case GpuSceneAdmission::Unsupported:
        return "unsupported";
    case GpuSceneAdmission::Invalid:
        return "invalid";
    default:
        return "unknown";
    }
}

} // namespace gpu_scene
} // namespace tour
