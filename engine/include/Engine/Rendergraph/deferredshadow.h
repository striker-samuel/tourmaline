#pragma once
// Deferred Shadows — engine builder + CPU "Prepare" phase.
//
// Implements the tiled/zBin deferred-shadow pipeline (Square Enix,
// "Deferred Shadows"): shadow visibility is computed ahead of the
// lighting pass by a 5-phase pipeline and consumed per pixel from packed
// light lists + a visibility buffer, instead of sampling shadow maps
// inline per shaded fragment.
//
//   Phase 1 — Prepare (CPU, this file): the light buffer is sorted by
//   closest distance to the near plane; zBins (1024 depth bins, min/max
//   sorted light index per bin + "ranged" prefix/suffix variants for
//   depth-RANGE queries, figures 2a/2b) and per-TileGroup light masks
//   (64x64px screen tiles, the "Tile Light Mask Buffer") are built and
//   uploaded. Everything else runs on the GPU.
//
//   Phase 2 — Find Lights Approximate (deferredshadow_phase2.cs.hlsl):
//   per Shadow Tile (8x8 px) froxel culling (zBins x tile masks x
//   sphere/cone/clip-space bounds) -> TileGroup light masks + flexible
//   per-tile light-list slot reservation (one atomic per TileGroup,
//   listing 1).
//
//   Phase 3 — Find Lights Accurate (deferredshadow_phase3.cs.hlsl):
//   per-pixel bounds tests -> Early light list (3x10-bit indices per
//   uint, figure 5) + per-tile counts.
//
//   Phase 4 — Generate Deferred Shadows (deferredshadow_phase4.cs.hlsl):
//   per-pixel visibility via the spot-shadow atlas; all-lit flag /
//   all-shadow drop / 64B VisibilityBuffer slots (8 bits/px) + Final
//   light list (10-bit index | 22-bit slot, figure 7b).
//
//   Phase 5 — HiQuality (NOT implemented): the paper composites closeup
//   character shadowmaps over the visibility results for character/light
//   pairs. Extension point: an extra compute pass between phase 4 and
//   the lighting consumer would upgrade VisibilityBuffer slots (or the
//   all-lit flag) for tiles covering characters. This scene has no
//   character-registered meshes, so the phase is left as a hook.
//
// Buffer sizing: all GPU buffers are FIXED size (see kDs* caps, sized
// for 4K) and ringed per frame in flight like ExposureMeter — CPU
// uploads (zbins/masks/counters zero-fill) submit immediately at record
// time, so a single shared buffer would let frame N+1 clobber frame N's
// still-executing phases. Caps degrade gracefully: slot reservation and
// visibility allocation clamp on overflow (lights drop to fully lit, no
// OOB), and the app reads the counters back for the debug overlay.
//
// Usage (app side, see SceneRenderer):
//   smvk::DeferredShadows ds;
//   ds.Ensure(ctx, rg, renderExtent);            // every frame (cheap)
//   // phase 1 (CPU) in the light upload step:
//   smvk::DsSortLightsByNearDistance(lights, frame.view);
//   smvk::DsBuildZBins(lights, frame.view, near, far, staging);
//   smvk::DsBuildTileMasks(lights, frame.view, frame.proj, w, h, staging);
//   // phases 2-4: AddPhase2/3/4Pass + recording funcs (bind, push, dispatch)
//   // lighting pass: AddMainReadHints + FrameUniforms ds* fields.

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace smvk {

// ---------------------------------------------------------------------------
// Constants (must match engine/shaders/deferredshadow_types.hlsli)
// ---------------------------------------------------------------------------

static constexpr uint32_t kDsShadowTilePx = 8;     // Shadow Tile = 8x8 px
static constexpr uint32_t kDsTileGroupTiles = 8;   // TileGroup = 8x8 tiles
static constexpr uint32_t kDsTileGroupPx = 64;     // 8 * 8
static constexpr uint32_t kDsZBinCount = 1024;
static constexpr uint32_t kDsMaxLightGroups = 32;  // MAX_FP_LIGHTS / 32
static constexpr uint32_t kDsMaxTileGroups = 4096; // 64x64px groups (4K)
static constexpr uint32_t kDsMaxTiles = 131072;    // 8px tiles (4K)
static constexpr uint32_t kDsMaxListUInts = 4u << 20;  // 16 MB
static constexpr uint32_t kDsMaxVisSlots = 256u << 10; // 64B each, 16 MB
static constexpr uint32_t kDsMaxLightsPerTile = 96;
static constexpr uint32_t kDsSlotAllLit = 0x3FFFFFu;
static constexpr float kDsInfiniteRange = 1e6f;
// Tile state = 4 uints per Shadow Tile {slot, count, approx, early}. The
// 4-uint layout is UNCONDITIONAL (shared shader ABI); the approx/early
// telemetry words are written only in DS_DEBUG_VIZ builds (see the phase
// shaders) and read by the light-count visualizers (paper 5.4).
static constexpr size_t kDsZBinBytes = sizeof(uint32_t) * 4 * kDsZBinCount;
static constexpr size_t kDsTileMaskBytes =
    sizeof(uint32_t) * (1 + kDsMaxLightGroups) * kDsMaxTileGroups;
static constexpr size_t kDsTileStateBytes = sizeof(uint32_t) * 4 * kDsMaxTiles;static constexpr size_t kDsLightListBytes = sizeof(uint32_t) * kDsMaxListUInts;
static constexpr size_t kDsVisibilityBytes = uint64_t(kDsMaxVisSlots) * 64;
// Counters + the phase 3/4 classification histogram (see DsCounters).
static constexpr uint32_t kDsCounterCount = 10;

inline constexpr const char* kDsPhase2CsPath =
    "content/shaders/deferredshadow_phase2.cs.hlsl.spv";
inline constexpr const char* kDsPhase3CsPath =
    "content/shaders/deferredshadow_phase3.cs.hlsl.spv";
inline constexpr const char* kDsPhase4CsPath =
    "content/shaders/deferredshadow_phase4.cs.hlsl.spv";

// Must match DsPhasePush in engine/shaders/deferredshadow.hlsli (120 B).
struct alignas(8) DsPhasePush {
    uint64_t frameUniforms = 0;
    uint64_t lights = 0;      // sorted light buffer (phase 1)
    uint64_t zbins = 0;
    uint64_t tileMasks = 0;   // CPU phase-1 masks (phase 2 input)
    uint64_t tileGroupMasks = 0; // GPU masks (phase 2 out, 3 in)
    uint64_t tileState = 0;
    uint64_t lightList = 0;
    uint64_t visibility = 0;
    uint64_t counters = 0;
    uint64_t spotShadowBuffer = 0;
    uint32_t spotShadowMapIndex = UINT32_MAX;
    uint32_t spotShadowCount = 0;
    uint32_t lightCount = 0;
    uint32_t tileCountX = 0;
    uint32_t tileCountY = 0;
    uint32_t groupCountX = 0;
    uint32_t groupCountY = 0;
    uint32_t depthTextureIndex = UINT32_MAX;
    float spotShadowNear = 0.5f; // SPOT_SHADOW_NEAR (PCSS eye-depth inversion)
    float pcssLightSize = 0.35f;  // world-space emitting-sphere diameter (PCSS)
};
static_assert(sizeof(DsPhasePush) == 120);

// ---------------------------------------------------------------------------
// Phase 1 (CPU): light sort key — closest distance from the near plane to
// the light's nearest extent. Sorting by this (ascending) is what lets
// zBins store only a min/max light index per bin (paper 2.1). Lights
// fully behind the camera sort last (FLT_MAX) and carry the kDsZBinCount
// near-sentinel so the monotonic max-bin walk skips them.
// ---------------------------------------------------------------------------

inline float DsNearSortKey(const ForwardPlusLight& L, const glm::mat4& view)
{
    const float range = (L.range <= 0.0f) ? kDsInfiniteRange : L.range;
    const glm::vec3 cView = view * glm::vec4(
        glm::vec3(L.position[0], L.position[1], L.position[2]), 1.0f);
    const float z = -cView.z; // forward distance (view looks down -z)
    if (z + range <= 0.0f)
        return std::numeric_limits<float>::max(); // fully behind camera
    return std::max(z - range, 0.0f);
}

inline void DsSortLightsByNearDistance(std::vector<ForwardPlusLight>& lights,
    const glm::mat4& view)
{
    const uint32_t n = (uint32_t)lights.size();
    if (n < 2)
        return;
    // Decorate-sort-undecorate: the key costs a mat4 multiply, and the
    // old stable_sort comparator recomputed BOTH keys on every one of
    // the ~n*log2(n) comparisons (~20k mat4 muls/frame at 1024 lights).
    // Keys are computed once here; the index tiebreak reproduces
    // stable_sort's order for equal keys. thread_local scratch keeps
    // the steady state allocation-free.
    static thread_local std::vector<std::pair<float, uint32_t>> keys;
    keys.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        keys[i] = { DsNearSortKey(lights[i], view), i };
    std::sort(keys.begin(), keys.end());
    static thread_local std::vector<ForwardPlusLight> sorted;
    sorted.assign(lights.begin(), lights.end());
    for (uint32_t i = 0; i < n; ++i)
        lights[i] = sorted[keys[i].second];
}

// ---------------------------------------------------------------------------
// Phase 1 (CPU): zBins. Output layout: 4 uints per bin
// {minIdx, maxIdx, rangedMin, rangedMax} (uint4 in the shader).
//   minIdx[b]  = min{ i : farBin[i] >= b }  (sentinel = lightCount)
//   maxIdx[b]  = last i with nearBin[i] <= b (nearBin is non-decreasing
//                after the sort; 0 = empty)
//   rangedMin  = prefix running min of minIdx (query: far bin)
//   rangedMax  = suffix running max of maxIdx (query: near bin)
// Conservative-safe: every light overlapping a bin has index in
// [min..max]; empty bins encode min > max. Dead (fully-behind) lights
// contribute nothing (near sentinel, far 0 — far 0 only ever shrinks
// minIdx[0], which stays safe).
// ---------------------------------------------------------------------------

inline void DsBuildZBins(const std::vector<ForwardPlusLight>& sortedLights,
    const glm::mat4& view, float nearPlane, float farPlane,
    std::vector<uint32_t>& outBins)
{
    outBins.assign(4 * kDsZBinCount, 0);
    const uint32_t n = (uint32_t)sortedLights.size();
    // thread_local scratch: this runs every frame on the render thread;
    // reuse the capacity instead of allocating ~20 KB per call.
    static thread_local std::vector<uint32_t> nearBin, farBin;
    nearBin.assign(n, kDsZBinCount);
    farBin.assign(n, 0);
    const float span = std::max(farPlane - nearPlane, 1e-4f);
    const auto binOf = [&](float dist) -> uint32_t {
        const float t = std::clamp((dist - nearPlane) / span, 0.0f, 1.0f);
        return (uint32_t)(t * float(kDsZBinCount - 1));
    };
    for (uint32_t i = 0; i < n; ++i) {
        const ForwardPlusLight& L = sortedLights[i];
        const float range = (L.range <= 0.0f) ? kDsInfiniteRange : L.range;
        const glm::vec3 cView = view * glm::vec4(
            glm::vec3(L.position[0], L.position[1], L.position[2]), 1.0f);
        const float z = -cView.z;
        if (z + range <= 0.0f)
            continue; // dead: keep (kDsZBinCount, 0) sentinels
        nearBin[i] = binOf(std::max(z - range, 0.0f));
        farBin[i] = binOf(z + range);
    }

    // Watermark fill (O(n + bins)): each light covers the bin PREFIX
    // [0..farBin[i]], so the filled set is always a contiguous prefix
    // — every bin above the high-water mark is still at its sentinel
    // and gets the current (minimal) index. The old per-light rescan
    // of the whole prefix was O(n * kDsZBinCount) with up to ~1M
    // branchy iterations at 1024 lights.
    static thread_local std::vector<uint32_t> minIdx;
    minIdx.assign(kDsZBinCount, n); // sentinel = n (empty)
    uint32_t hi = 0; // bins [0..hi) already carry their min index
    for (uint32_t i = 0; i < n; ++i) {
        if (nearBin[i] == kDsZBinCount)
            continue; // dead light
        if (farBin[i] >= hi) {
            for (uint32_t b = hi; b <= farBin[i]; ++b)
                minIdx[b] = i;
            hi = farBin[i] + 1;
        }
    }
    static thread_local std::vector<uint32_t> maxIdx;
    maxIdx.assign(kDsZBinCount, 0);
    {
        uint32_t j = 0;
        for (uint32_t b = 0; b < kDsZBinCount; ++b) {
            while (j < n && nearBin[j] <= b)
                ++j;
            maxIdx[b] = (j > 0) ? j - 1 : 0;
        }
    }
    for (uint32_t b = 0; b < kDsZBinCount; ++b) {
        outBins[b * 4 + 0] = minIdx[b];
        outBins[b * 4 + 1] = maxIdx[b];
    }
    uint32_t rmin = n;
    for (uint32_t b = 0; b < kDsZBinCount; ++b) {
        rmin = std::min(rmin, minIdx[b]);
        outBins[b * 4 + 2] = rmin;
    }
    uint32_t rmax = 0;
    for (uint32_t b = kDsZBinCount; b-- > 0;) {
        rmax = std::max(rmax, maxIdx[b]);
        outBins[b * 4 + 3] = rmax;
    }
}

// ---------------------------------------------------------------------------
// Phase 1 (CPU): Tile Light Mask buffer. 2D screen-space cull (no depth —
// zBins add that on the GPU) of every light vs the 4 side planes of each
// 64x64px TileGroup it could touch, in the "Light Mask Entry" format
// (figure 4): per group, 1 uint of group bits + 32 uints of 32-light
// masks. Sphere-only (cone/clip narrowing happens in phase 2), so it is
// a conservative superset of the light's influence: any pixel within
// `range` of the light lies in a group the sphere-vs-planes test keeps.
// Plane math mirrors forwardplus.cs.hlsl (origin planes from unprojected
// group corners; NDC y down in the framebuffer).
// ---------------------------------------------------------------------------

inline void DsBuildTileMasks(const std::vector<ForwardPlusLight>& lights,
    const glm::mat4& view, const glm::mat4& proj,
    uint32_t vpWidth, uint32_t vpHeight,
    std::vector<uint32_t>& outMasks,
    uint32_t& outGroupCountX, uint32_t& outGroupCountY)
{
    outGroupCountX = (vpWidth + kDsTileGroupPx - 1) / kDsTileGroupPx;
    outGroupCountY = (vpHeight + kDsTileGroupPx - 1) / kDsTileGroupPx;
    outMasks.assign(size_t(outGroupCountX) * outGroupCountY * (1 + kDsMaxLightGroups), 0);
    if (outGroupCountX == 0 || outGroupCountY == 0)
        return;

    const glm::mat4 invProj = glm::inverse(proj);
    const auto unprojectNear = [&](const glm::vec2& ndc) -> glm::vec3 {
        const glm::vec4 v = invProj * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
        return glm::vec3(v) / v.w;
    };
    const float wF = std::max(float(vpWidth), 1.0f);
    const float hF = std::max(float(vpHeight), 1.0f);

    const uint32_t n = std::min<uint32_t>(
        (uint32_t)lights.size(), kDsMaxLightGroups * 32u);
    // Group origin planes are light-independent (same cross() order as
    // forwardplus.cs.hlsl — verified there by hand): precompute 4 inward
    // planes per group. They depend ONLY on (proj, extent), so they are
    // cached across frames and rebuilt on drift (memcmp is conservative:
    // a -0.0/0.0 bit difference just costs one rebuild). NDC y down in
    // the framebuffer (y0 = top row on screen). Static-camera frames
    // skip the ~2k (1080p) to ~8k (4K) unproject+normalize per frame.
    struct GroupPlaneCache {
        glm::mat4 proj{};
        uint32_t w = 0, h = 0;
        std::vector<glm::vec3> planes;
    };
    static thread_local GroupPlaneCache cache;
    const size_t groupCount = size_t(outGroupCountX) * outGroupCountY;
    if (cache.w != vpWidth || cache.h != vpHeight ||
        std::memcmp(&cache.proj, &proj, sizeof(glm::mat4)) != 0 ||
        cache.planes.size() != groupCount * 4) {
        cache.planes.assign(groupCount * 4, glm::vec3(0.0f));
        for (uint32_t gy = 0; gy < outGroupCountY; ++gy) {
            for (uint32_t gx = 0; gx < outGroupCountX; ++gx) {
                const float x0 = (float(gx * kDsTileGroupPx) / wF) * 2.0f - 1.0f;
                const float x1 = (float((gx + 1) * kDsTileGroupPx) / wF) * 2.0f - 1.0f;
                const float y0 = (float(gy * kDsTileGroupPx) / hF) * 2.0f - 1.0f;
                const float y1 = (float((gy + 1) * kDsTileGroupPx) / hF) * 2.0f - 1.0f;
                const glm::vec3 c00 = unprojectNear(glm::vec2(x0, y0));
                const glm::vec3 c10 = unprojectNear(glm::vec2(x1, y0));
                const glm::vec3 c11 = unprojectNear(glm::vec2(x1, y1));
                const glm::vec3 c01 = unprojectNear(glm::vec2(x0, y1));
                glm::vec3* planes = &cache.planes[(size_t(gy) * outGroupCountX + gx) * 4];
                planes[0] = glm::normalize(glm::cross(c00, c10)); // top
                planes[1] = glm::normalize(glm::cross(c11, c01)); // bottom
                planes[2] = glm::normalize(glm::cross(c01, c00)); // left
                planes[3] = glm::normalize(glm::cross(c10, c11)); // right
            }
        }
        cache.proj = proj;
        cache.w = vpWidth;
        cache.h = vpHeight;
    }
    const std::vector<glm::vec3>& groupPlanes = cache.planes;
    for (uint32_t i = 0; i < n; ++i) {
        const ForwardPlusLight& L = lights[i];
        const float range = (L.range <= 0.0f) ? kDsInfiniteRange : L.range;
        const glm::vec3 pos(L.position[0], L.position[1], L.position[2]);
        const glm::vec3 cView = view * glm::vec4(pos, 1.0f);
        if (cView.z - range > 0.0f)
            continue; // fully behind the camera plane

        // Conservative screen rect: NDC bounds of the 8 view-space box
        // corners around the light center (the box contains the sphere,
        // so its projection contains the sphere's). Any corner behind
        // the near plane -> the whole grid (rare, degenerate).
        float ndcMinX = std::numeric_limits<float>::max();
        float ndcMaxX = std::numeric_limits<float>::lowest();
        float ndcMinY = std::numeric_limits<float>::max();
        float ndcMaxY = std::numeric_limits<float>::lowest();
        bool crossesNear = false;
        for (int sz = -1; sz <= 1; sz += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sx = -1; sx <= 1; sx += 2) {
                    const glm::vec3 corner = cView + range * glm::vec3(float(sx), float(sy), float(sz));
                    const glm::vec4 clip = proj * glm::vec4(corner, 1.0f);
                    if (clip.w <= 1e-6f) {
                        crossesNear = true;
                        break;
                    }
                    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                    ndcMinX = std::min(ndcMinX, ndc.x);
                    ndcMaxX = std::max(ndcMaxX, ndc.x);
                    ndcMinY = std::min(ndcMinY, ndc.y);
                    ndcMaxY = std::max(ndcMaxY, ndc.y);
                }
                if (crossesNear) break;
            }
            if (crossesNear) break;
        }

        uint32_t gx0 = 0, gy0 = 0, gx1 = outGroupCountX - 1, gy1 = outGroupCountY - 1;
        if (!crossesNear) {
            auto px = [](float ndc, float size) { return (ndc * 0.5f + 0.5f) * size; };
            const float pMinX = px(ndcMinX, wF), pMaxX = px(ndcMaxX, wF);
            const float pMinY = px(ndcMinY, hF), pMaxY = px(ndcMaxY, hF);
            const auto groupOf = [](float p, uint32_t count) -> uint32_t {
                const int32_t g = (int32_t)std::floor(p / float(kDsTileGroupPx));
                return (uint32_t)std::clamp(g, 0, int32_t(count - 1));
            };
            gx0 = groupOf(pMinX, outGroupCountX);
            gx1 = groupOf(pMaxX, outGroupCountX);
            gy0 = groupOf(pMinY, outGroupCountY);
            gy1 = groupOf(pMaxY, outGroupCountY);
        }

        const uint32_t lightGroup = i / 32u;
        const uint32_t lightBit = 1u << (i % 32u);
        for (uint32_t gy = gy0; gy <= gy1; ++gy) {
            for (uint32_t gx = gx0; gx <= gx1; ++gx) {
                const glm::vec3* planes =
                    &groupPlanes[(size_t(gy) * outGroupCountX + gx) * 4];
                bool outside = false;
                for (int p = 0; p < 4; ++p) {
                    if (dot(planes[p], cView) < -range) {
                        outside = true;
                        break;
                    }
                }
                if (outside)
                    continue;
                const size_t base = (size_t(gy) * outGroupCountX + gx) * (1 + kDsMaxLightGroups);
                outMasks[base] |= 1u << lightGroup;
                outMasks[base + 1 + lightGroup] |= lightBit;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// GPU resource builder (ForwardPlus / ExposureMeter pattern).
// ---------------------------------------------------------------------------

struct DeferredShadows {
    // Per-flight ring (see the pipeline note above).
    Holder<BufferHandle> zBins[MAX_FRAMES_IN_FLIGHT]{};
    Holder<BufferHandle> tileMasks[MAX_FRAMES_IN_FLIGHT]{};
    Holder<BufferHandle> tileGroupMasks[MAX_FRAMES_IN_FLIGHT]{};
    Holder<BufferHandle> tileState[MAX_FRAMES_IN_FLIGHT]{};
    Holder<BufferHandle> lightList[MAX_FRAMES_IN_FLIGHT]{};
    Holder<BufferHandle> visibility[MAX_FRAMES_IN_FLIGHT]{};
    Holder<BufferHandle> counters[MAX_FRAMES_IN_FLIGHT]{};
    LogicalBufferHandle zBinsLogical[MAX_FRAMES_IN_FLIGHT]{};
    LogicalBufferHandle tileMasksLogical[MAX_FRAMES_IN_FLIGHT]{};
    LogicalBufferHandle tileGroupMasksLogical[MAX_FRAMES_IN_FLIGHT]{};
    LogicalBufferHandle tileStateLogical[MAX_FRAMES_IN_FLIGHT]{};
    LogicalBufferHandle lightListLogical[MAX_FRAMES_IN_FLIGHT]{};
    LogicalBufferHandle visibilityLogical[MAX_FRAMES_IN_FLIGHT]{};
    LogicalBufferHandle countersLogical[MAX_FRAMES_IN_FLIGHT]{};

    Holder<ShaderModuleHandle> phaseModules[3]{};
    ShaderProgram phasePrograms[3]{};

    Extent2D extent{ 0, 0 };
    uint32_t tileCountX = 0;
    uint32_t tileCountY = 0;
    uint32_t groupCountX = 0;
    uint32_t groupCountY = 0;

    [[nodiscard]] bool Valid() const {
        if (tileCountX == 0 || tileCountY == 0)
            return false;
        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
            if (zBins[f].empty() || tileMasks[f].empty() ||
                tileGroupMasks[f].empty() || tileState[f].empty() ||
                lightList[f].empty() || visibility[f].empty() ||
                counters[f].empty())
                return false;
        }
        return true;
    }
    [[nodiscard]] bool ShadersValid() const {
        for (uint32_t p = 0; p < 3; ++p) {
            if (phasePrograms[p].shaderCount == 0 || phaseModules[p].empty())
                return false;
        }
        return true;
    }

    static uint32_t TilesFor(uint32_t dim) {
        return (dim + kDsShadowTilePx - 1) / kDsShadowTilePx;
    }
    static uint32_t GroupsFor(uint32_t dim) {
        return (dim + kDsTileGroupPx - 1) / kDsTileGroupPx;
    }

    bool EnsureShaders(ResourceManager& ctx) {
        if (ShadersValid())
            return true;
        static constexpr const char* kPaths[3] = {
            kDsPhase2CsPath, kDsPhase3CsPath, kDsPhase4CsPath
        };
        try {
            for (uint32_t p = 0; p < 3; ++p) {
                if (phaseModules[p].empty())
                    phaseModules[p] = ctx.CreateShaderModule(
                        readFile(kPaths[p]), "deferredShadowCS");
            }
        } catch (...) {
            return false;
        }
        for (uint32_t p = 0; p < 3; ++p) {
            if (phaseModules[p].empty())
                return false;
            Shader cs{
                .module = phaseModules[p],
                .entryPoint = "mainCS",
                .stage = ShaderStage::Compute };
            phasePrograms[p] = { .shaders = { cs }, .shaderCount = 1 };
        }
        return true;
    }

    void DestroyShaders() {
        for (uint32_t p = 0; p < 3; ++p) {
            phasePrograms[p] = ShaderProgram{};
            phaseModules[p] = {};
        }
    }

    bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent) {
        if (swapExtent.width == 0 || swapExtent.height == 0)
            return false;
        Destroy(rg);

        const uint32_t tcx = TilesFor(swapExtent.width);
        const uint32_t tcy = TilesFor(swapExtent.height);
        const uint32_t gcx = GroupsFor(swapExtent.width);
        const uint32_t gcy = GroupsFor(swapExtent.height);
        // Fixed-cap buffers: beyond 4K-class extents the pipeline is off
        // (better than silently writing out of range).
        if (tcx * tcy > kDsMaxTiles || gcx * gcy > kDsMaxTileGroups)
            return false;

        struct BufSpec { size_t size; const char* name; };
        const BufSpec specs[7] = {
            { kDsZBinBytes, "ds zbins" },
            { kDsTileMaskBytes, "ds tileMasks" },
            { kDsTileMaskBytes, "ds tileGroupMasks" },
            { kDsTileStateBytes, "ds tileState" },
            { kDsLightListBytes, "ds lightList" },
            { kDsVisibilityBytes, "ds visibility" },
            { kDsCounterCount * sizeof(uint32_t), "ds counters" },
        };
        Holder<BufferHandle>* buffers[7] = {
            zBins, tileMasks, tileGroupMasks, tileState, lightList, visibility, counters
        };
        LogicalBufferHandle* logicals[7] = {
            zBinsLogical, tileMasksLogical, tileGroupMasksLogical,
            tileStateLogical, lightListLogical, visibilityLogical, countersLogical
        };
        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
            for (uint32_t b = 0; b < 7; ++b) {
                Holder<BufferHandle> buf = ctx.CreateBuffer(
                    specs[b].size, BufferType_Storage, specs[b].name);
                if (buf.empty()) {
                    Destroy(rg);
                    return false;
                }
                LogicalBufferHandle logical =
                    rg.AddExternalBuffer(buf, specs[b].name);
                if (logical.empty()) {
                    Destroy(rg);
                    return false;
                }
                buffers[b][f] = std::move(buf);
                logicals[b][f] = logical;
            }
            // No zero-fill here: every slice is zeroed at record time
            // before its frame's passes run (UpdateForwardPlusLights runs
            // after Ensure in RenderFrame; the upload batch flushes ahead
            // of G1 inside Execute and ahead of the main submit, both with
            // explicit timeline waits), so no GPU read can observe a
            // stale slot. Zeroing here as well records a second copy to
            // the same slice in the same transfer batch when Create runs
            // lazily inside a frame; the streamer emits only a release
            // barrier between batched copies, which sync validation
            // flags as WRITE_AFTER_WRITE (observed on the 40-byte
            // counters buffer at startup).
        }

        extent = swapExtent;
        tileCountX = tcx;
        tileCountY = tcy;
        groupCountX = gcx;
        groupCountY = gcy;
        return EnsureShaders(ctx);
    }

    void Destroy(RenderGraph& rg) {
        LogicalBufferHandle* logicals[7] = {
            zBinsLogical, tileMasksLogical, tileGroupMasksLogical,
            tileStateLogical, lightListLogical, visibilityLogical, countersLogical
        };
        Holder<BufferHandle>* buffers[7] = {
            zBins, tileMasks, tileGroupMasks, tileState, lightList, visibility, counters
        };
        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
            for (uint32_t b = 0; b < 7; ++b) {
                if (!logicals[b][f].empty())
                    rg.FreeBuffer(logicals[b][f]);
                logicals[b][f] = {};
                buffers[b][f] = {};
            }
        }
        extent = { 0, 0 };
        tileCountX = tileCountY = groupCountX = groupCountY = 0;
    }

    // Buffers are fixed-size: Ensure only refreshes dispatch dims.
    bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent) {
        const bool shapeOk = Valid() &&
            extent.width == swapExtent.width &&
            extent.height == swapExtent.height;
        if (shapeOk)
            return EnsureShaders(ctx);
        return Create(ctx, rg, swapExtent);
    }

    // ---- Addresses / logicals (per flight) ----
    [[nodiscard]] uint64_t ZBinsAddress(ResourceManager& ctx, uint32_t f) const {
        return ctx.GetBufferDeviceAddress(zBins[f]);
    }
    [[nodiscard]] uint64_t TileMasksAddress(ResourceManager& ctx, uint32_t f) const {
        return ctx.GetBufferDeviceAddress(tileMasks[f]);
    }
    [[nodiscard]] uint64_t TileGroupMasksAddress(ResourceManager& ctx, uint32_t f) const {
        return ctx.GetBufferDeviceAddress(tileGroupMasks[f]);
    }
    [[nodiscard]] uint64_t TileStateAddress(ResourceManager& ctx, uint32_t f) const {
        return ctx.GetBufferDeviceAddress(tileState[f]);
    }
    [[nodiscard]] uint64_t LightListAddress(ResourceManager& ctx, uint32_t f) const {
        return ctx.GetBufferDeviceAddress(lightList[f]);
    }
    [[nodiscard]] uint64_t VisibilityAddress(ResourceManager& ctx, uint32_t f) const {
        return ctx.GetBufferDeviceAddress(visibility[f]);
    }
    [[nodiscard]] uint64_t CountersAddress(ResourceManager& ctx, uint32_t f) const {
        return ctx.GetBufferDeviceAddress(counters[f]);
    }
    [[nodiscard]] BufferHandle CounterBuffer(uint32_t f) const {
        return counters[f];
    }
    [[nodiscard]] BufferHandle ZBinsBuffer(uint32_t f) const {
        return zBins[f];
    }
    [[nodiscard]] BufferHandle TileMasksBuffer(uint32_t f) const {
        return tileMasks[f];
    }
    [[nodiscard]] BufferHandle TileGroupMasksBuffer(uint32_t f) const {
        return tileGroupMasks[f];
    }
    [[nodiscard]] BufferHandle TileStateBuffer(uint32_t f) const {
        return tileState[f];
    }
    [[nodiscard]] BufferHandle LightListBuffer(uint32_t f) const {
        return lightList[f];
    }
    [[nodiscard]] LogicalBufferHandle TileStateLogical(uint32_t f) const {
        return tileStateLogical[f];
    }
    [[nodiscard]] LogicalBufferHandle LightListLogical(uint32_t f) const {
        return lightListLogical[f];
    }
    [[nodiscard]] LogicalBufferHandle VisibilityLogical(uint32_t f) const {
        return visibilityLogical[f];
    }
    [[nodiscard]] LogicalBufferHandle TileGroupMasksLogical(uint32_t f) const {
        return tileGroupMasksLogical[f];
    }

    // ---- Pass shells (caller sets the recording func: bind phase
    // program, push DsPhasePush, dispatch) ----

    static void AddComputeRWHint(RenderPass& pass, LogicalBufferHandle h) {
        BufferSyncRequest req{};
        req.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
        req.barrierMasks.stageMask = PipelineStageBit_Compute;
        pass.AddBufferHint(h, req);
    }
    static void AddComputeReadHint(RenderPass& pass, LogicalBufferHandle h) {
        BufferSyncRequest req{};
        req.barrierMasks.accessMask = AccessFlags::ShaderRead;
        req.barrierMasks.stageMask = PipelineStageBit_Compute;
        pass.AddBufferHint(h, req);
    }
    static void AddDepthReadHint(RenderPass& pass, LogicalImageViewHandle depthLogical) {
        // Same request as ForwardPlus::AddCullPass: the graph promotes
        // depth formats to DepthStencilReadOnlyOptimal.
        ImageSyncRequest depthReq{};
        depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
        depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
        depthReq.layout = ImageLayout::ShaderReadOnlyOptimal;
        pass.AddImageHint(depthLogical, depthReq);
    }

    // Phase 2: reads depth + lights + zbins + CPU masks; writes group
    // masks, tile state, counters. Dispatch (groupCountX, groupCountY, 1).
    RenderPass& AddPhase2Pass(RenderGraph& rg,
        LogicalImageViewHandle depthLogical, LogicalBufferHandle lightsLogical,
        uint32_t f, const char* name = "DsFindLightsApprox") const {
        assert(Valid());
        RenderPass& pass = rg.AddComputePass(name);
        AddDepthReadHint(pass, depthLogical);
        AddComputeReadHint(pass, lightsLogical);
        AddComputeReadHint(pass, zBinsLogical[f]);
        AddComputeReadHint(pass, tileMasksLogical[f]);
        AddComputeRWHint(pass, tileGroupMasksLogical[f]);
        AddComputeRWHint(pass, tileStateLogical[f]);
        AddComputeRWHint(pass, countersLogical[f]);
        return pass;
    }

    // Phase 3: reads depth + lights + group masks; writes tile state +
    // light list. Dispatch (tileCountX, tileCountY, 1).
    RenderPass& AddPhase3Pass(RenderGraph& rg,
        LogicalImageViewHandle depthLogical, LogicalBufferHandle lightsLogical,
        uint32_t f, const char* name = "DsFindLightsAccurate") const {
        assert(Valid());
        RenderPass& pass = rg.AddComputePass(name);
        AddDepthReadHint(pass, depthLogical);
        AddComputeReadHint(pass, lightsLogical);
        AddComputeReadHint(pass, tileGroupMasksLogical[f]);
        AddComputeRWHint(pass, tileStateLogical[f]);
        AddComputeRWHint(pass, lightListLogical[f]);
        return pass;
    }

    // Phase 4: reads depth + lights + early list + spot shadows; writes
    // final list (in place), tile state, visibility, counters. The spot
    // atlas is SAMPLED in compute (bindless, depth read-only layout).
    // Dispatch (tileCountX, tileCountY, 1).
    RenderPass& AddPhase4Pass(RenderGraph& rg,
        LogicalImageViewHandle depthLogical, LogicalBufferHandle lightsLogical,
        LogicalBufferHandle spotShadowBufferLogical,
        LogicalImageViewHandle spotAtlasLogical,
        uint32_t f, const char* name = "DsGenerateShadows") const {
        assert(Valid());
        RenderPass& pass = rg.AddComputePass(name);
        AddDepthReadHint(pass, depthLogical);
        AddComputeReadHint(pass, lightsLogical);
        AddComputeReadHint(pass, spotShadowBufferLogical);
        if (!spotAtlasLogical.empty()) {
            ImageSyncRequest atlasReq{};
            atlasReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
            atlasReq.barrierMasks.stageMask = PipelineStageBit_Compute;
            atlasReq.layout = ImageLayout::DepthStencilReadOnlyOptimal;
            pass.AddImageHint(spotAtlasLogical, atlasReq);
        }
        AddComputeRWHint(pass, tileStateLogical[f]);
        AddComputeRWHint(pass, lightListLogical[f]);
        AddComputeRWHint(pass, visibilityLogical[f]);
        AddComputeRWHint(pass, countersLogical[f]);
        return pass;
    }

    // Read-side hints for the lighting pass (final list + visibility +
    // tile state, fragment stage).
    static void AddMainReadHints(RenderPass& mainPass,
        LogicalBufferHandle tileState, LogicalBufferHandle lightList,
        LogicalBufferHandle visibility) {
        BufferSyncRequest read{};
        read.barrierMasks.accessMask = AccessFlags::ShaderRead;
        read.barrierMasks.stageMask = PipelineStageBit_Fragment;
        mainPass.AddBufferHint(tileState, read);
        mainPass.AddBufferHint(lightList, read);
        mainPass.AddBufferHint(visibility, read);
    }
};

} // namespace smvk
