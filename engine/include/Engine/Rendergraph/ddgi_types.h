// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace smvk::ddgi {

// Initial fixed-grid implementation. Flat buffers avoid octahedral atlas borders.
inline constexpr uint32_t kIrrResolution = 8;
inline constexpr uint32_t kDistanceResolution = 16;
inline constexpr uint32_t kIrrBins = kIrrResolution * kIrrResolution;
inline constexpr uint32_t kDistanceBins = kDistanceResolution * kDistanceResolution;
inline constexpr uint32_t kMaxProbes = 32768;
inline constexpr uint32_t kMaxRays = 512;

// ---- Multi-volume (per-world-subcell) layout. Each occupied 64 m world
// subcell owns ONE probe volume: a dense grid over the subcell's XZ
// footprint + the local geometry's Y range. The lit shader walks the
// VolumeDesc list (a GPU buffer published through FrameUniforms) to find
// the volume containing a world point. The trace/blend passes carry each
// volume's grid + BDAs in their OWN push block; VolumeDesc is the lit
// shader's lookup table. ----
inline constexpr uint32_t kDdgiMaxVolumes = 128;

// ---- Camera-following cascades (two levels, both scroll with the camera).
// Fine (near): fixed dense spacing. Coarse (far): spacing distance-fitted
// (SDSM-style slow hysteresis) so its volume reaches the estimated far depth.
inline constexpr uint32_t kDdgiCascadeCount = 2;
inline constexpr uint32_t kDdgiFineDims[3] = {16, 8, 16};
inline constexpr uint32_t kDdgiCoarseDims[3] = {16, 8, 16};
inline constexpr float kDdgiFineSpacing = 1.5f;
// Coarse spacing clamps, expressed as MULTIPLES of the fine spacing (the
// coarse cascade must stay dense enough to contribute — never stretch to
// a useless 12 m spacing).
inline constexpr float kDdgiCoarseMinScale = 2.0f;
inline constexpr float kDdgiCoarseMaxScale = 4.0f;
// The coarse volume's half-extent in cells ~ (dims.x-1)/2; reach = this *
// spacing must cover the estimated far depth (m_ddgiFar).
inline constexpr float kDdgiCoarseReachCells = 7.5f;

// GPU descriptor of one probe volume (mirrored in ddgi_common.hlsli as
// DdgiVolumeDesc; 64 bytes, 16-aligned). Carries BOTH history-slot BDAs —
// the lit shader picks the published slot via DdgiParams::slot, so
// the descriptor list is uploaded ONCE per world (no per-frame re-upload
// hazard on the persistent buffer).
struct alignas(16) VolumeDesc {
    uint64_t irradiance[2] = {}; // per-history-slot irradiance BDAs
    uint64_t distance[2] = {};   // per-history-slot distance-moment BDAs
    float origin[3] = {};        // grid min corner
    float spacing = 0.0f;        // probe spacing (m)
    uint32_t dims[3] = {};       // probe grid dims
    uint32_t pad = 0;
};
static_assert(sizeof(VolumeDesc) == 64 && offsetof(VolumeDesc, dims) == 48);

// The lit shader's DDGI sampling params — a small per-view slab (the
// FramePSHot pattern), uploaded per frame and reached through
// FrameUniforms::ddgiParams.Get(). Kept OUT of the monolithic FrameUniforms
// (a 6-field growth there would recompile every shader and bloat the
// per-flight uniform buffer; the single BDA fits the struct's tail pad).
// Mirrored in ddgi_common.hlsli as DdgiParams (32 bytes).
struct alignas(16) DdgiParams {
    uint64_t volumes = 0;     // 0  VolumeDesc[] BDA (0 = off)
    uint32_t volumeCount = 0; // 8  live volumes
    uint32_t slot = 0;        // 12 published history slot (0/1)
    float strength = 0.0f;    // 16 blend strength (0..1)
    uint32_t enabled = 0;     // 20 1 = sample + blend
    uint32_t debug = 0;       // 24 1 = flat irradiance view
    uint32_t pad = 0;         // 28
};
static_assert(sizeof(DdgiParams) == 32);

struct Grid {
    float origin[3] = {};
    float spacing = 2.0f;
    uint32_t dims[3] = {16, 8, 16};
    uint32_t rays = 128;
    float maxDistance = 20.0f;
    float hysteresis = 0.95f;
};

struct alignas(16) RaySample {
    float direction[3];
    float distance;
    float radiance[3];
    uint32_t flags; // 0 miss, 1 front hit, 2 back hit, 3 incomplete
};
struct alignas(16) Irradiance {
    float rgb[3];
    float valid;
};
struct DistanceMoments {
    float mean, second;
};
static_assert(sizeof(RaySample) == 32 && offsetof(RaySample, flags) == 28);
static_assert(sizeof(Irradiance) == 16 && sizeof(DistanceMoments) == 8);

struct Sizes {
    uint32_t probes = 0;
    size_t scratch = 0, irradiance = 0, distance = 0;
};

// Reject malformed grids and allocations above the caller's device/budget limit.
inline bool CalculateSizes(const Grid& g, size_t byteBudget, Sizes& out) {
    out = {};
    if (!std::isfinite(g.spacing) || g.spacing <= 0 || !std::isfinite(g.maxDistance) ||
        g.maxDistance <= 0.001f || g.maxDistance > 1.0e8f || !std::isfinite(g.hysteresis) ||
        g.hysteresis < 0 || g.hysteresis >= 1 || g.rays == 0 || g.rays > kMaxRays)
        return false;
    uint64_t count = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(g.origin[i]) || g.dims[i] < 2 || g.dims[i] > kMaxProbes ||
            count > kMaxProbes / g.dims[i] ||
            !std::isfinite(g.origin[i] + (g.dims[i] - 1) * g.spacing))
            return false;
        count *= g.dims[i];
    }
    const uint64_t scratch = count * g.rays * sizeof(RaySample);
    const uint64_t irr = count * kIrrBins * sizeof(Irradiance);
    const uint64_t dist = count * kDistanceBins * sizeof(DistanceMoments);
    const uint64_t total = scratch + 2 * (irr + dist);
    if (total > byteBudget || total > std::numeric_limits<size_t>::max())
        return false;
    out = {static_cast<uint32_t>(count),
           static_cast<size_t>(scratch),
           static_cast<size_t>(irr),
           static_cast<size_t>(dist)};
    return true;
}

// Push mirrors use scalar members to avoid vector alignment surprises.
struct TracePush {
    uint64_t frameUniforms = 0, scratch = 0;
    float originX = 0, originY = 0, originZ = 0, spacing = 0;
    uint32_t dimX = 0, dimY = 0, dimZ = 0, rays = 0;
    float maxDistance = 0;
    uint32_t updateIndex = 0;
    // E_indirect feedback: the PREVIOUS-frame irradiance field + distance
    // moments of THIS volume, sampled at surface hit points so the field
    // gathers multi-bounce color (the lit shader's walk, read-only).
    uint64_t oldIrr = 0, oldDistance = 0;
    // Per-TLAS-instance compact material (albedo/emissive/metallic) for hit
    // lighting: float4 albedoEmissive + float4 pbr per instance.
    uint64_t rayMaterials = 0;
    // Per-probe relocation offset (float4, written by the SDF relocation).
    uint64_t relocation = 0;
    // Previous-frame grid origin (camera-following scroll): the OLD history
    // was written at the old origin, so E_indirect sampling must map hit
    // positions through the old grid, not the current one.
    float oldOriginX = 0, oldOriginY = 0, oldOriginZ = 0;
    // Scroll delta (probe units) + reset flag: the trace SKIPS probes whose
    // history carried across the scroll (coord + delta still in [0, dims))
    // except the rotating subset, so only scrolled-in + a rotating 1/N of
    // carried probes are re-traced each frame (incremental tracing).
    int32_t deltaX = 0, deltaY = 0, deltaZ = 0;
    uint32_t reset = 0;
    uint32_t pad_ = 0;
};
// Fixed choice per volume/dispatch, not a per-ray switch.
enum class TraceBackend : uint8_t { HardwareRT, StaticSdf };

struct SdfTracePush {
    TracePush trace{};
    uint64_t sceneAddress = 0;
};
static_assert(sizeof(TracePush) == 120 && offsetof(TracePush, oldOriginX) == 88);
static_assert(sizeof(SdfTracePush) == 128 && offsetof(SdfTracePush, sceneAddress) == 120);

struct BlendPush {
    uint64_t scratch = 0, oldIrr = 0, oldDistance = 0, newIrr = 0, newDistance = 0;
    uint32_t probes = 0, rays = 0, reset = 1;
    float hysteresis = 0, maxDistance = 0;
    // Toroidal history carry (camera-following scroll): a probe at logical
    // coord c this frame lived at c + delta in the previous grid. carry =
    // c+delta inside [0, dims); those probes blend hysteresis against the
    // shifted old history, the rest (scrolled-in territory) reset to 0.
    int32_t deltaX = 0, deltaY = 0, deltaZ = 0;
    uint32_t dimX = 0, dimY = 0, dimZ = 0;
    uint32_t updateIndex = 0; // rotating-subset phase (incremental trace match)
};
// SDF-assisted probe relocation (ddgi_relocate.cs.hlsl).
struct RelocatePush {
    uint64_t sceneAddress = 0, relocation = 0;
    float originX = 0, originY = 0, originZ = 0, spacing = 0;
    uint32_t dimX = 0, dimY = 0, dimZ = 0, probes = 0;
    float searchStep = 0;
    uint32_t iterations = 0;
};
static_assert(sizeof(RelocatePush) == 56 && offsetof(RelocatePush, iterations) == 52);
static_assert(sizeof(TracePush) == 120 && offsetof(TracePush, maxDistance) == 48);
static_assert(sizeof(BlendPush) == 88 && offsetof(BlendPush, deltaX) == 60);

} // namespace smvk::ddgi
