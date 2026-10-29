// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define VOLUMETRIC_PASS
#include "common.hlsli"
#include "shadows.hlsli" // SelectCascadeGpu + the cascade meta reads (pc-free guards)
#include "environment.hlsli"
#include "sggi.hlsli"
#include "sg_regular.hlsli"
#include "sg_gi_volume.hlsli"
#include "dof.hlsli"
#include "world_field.hlsli"

// Volumetric fog INTEGRATION (the half-resolution half of the perf
// split - see Engine/Rendergraph/volumetric.h): the raymarch ONLY,
// producing (scatter.rgb, throughput.a) + the pixel's representative
// VIEW DISTANCE (R32F, MRT target1) for the full-res composite's
// depth-aware upsample. NO scene HDR read, NO composite - the HDR
// stays full-resolution end to end (the old half-res fog mode broke
// exposure/DLSS/tonemap because it shrank the HDR OUTPUT; this split
// shrinks only the MARCH - 75% fewer integration pixels at 1080p, the
// composite re-expands with a bilateral that cannot bleed across
// silhouettes).
//
// HALF-RES COORDINATE RULE (the plan's flagged hazard): every
// FULL-resolution-grid lookup that keys on pixel coordinates (the
// Forward+ fog tile lists, the GI-volume tile mask) must use the
// FULL-res pixel reconstructed from IN.uv - IN.pos.xy is half-res
// here. UV-keyed lookups (the depth tap) are resolution-agnostic.
//
// The march body is the former volumetric.frag.hlsl verbatim except:
//   - the 2x2 nearest-valid receiver block scan (the rep the composite
//     weights against - same discipline as the SSR/AO block reps);
//   - the world-field density CADENCE (the perf plan item 3): the
//     virtual field lookup runs every fieldCadence meters along the
//     ray, two cached samples linearly interpolated per segment - the
//     field is spatially low-frequency at the march's sampling rate,
//     and a virtual page-table + trilinear 3D fetch PER SEGMENT was
//     the dominant per-segment cost after the march itself;
//   - kVolGiRefreshDist from the push (the perf plan item 4: 0.75 m ->
//     a configurable 1.5 m default; the baked GI base is low-frequency
//     ambient radiance, not extinction - unlike the density cadence it
//     may run coarser without optical-depth error).
// Debug views (fogGiDebug 1/2/3) output (dbg, 0): the composite's
// hdr*a + rgb then shows the PURE upsampled debug view (transmittance
// 0 kills the base) - the FOG_GI_DEBUG discipline preserved through
// the split.

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outFog : SV_Target0;   // RGBA16F: rgb scatter, a throughput
    float4 outDepth : SV_Target1; // R32F: the representative view distance
};

// Must match Engine/Rendergraph/volumetric.h VolumetricPush (140 bytes
// of members; the C++ side pads the tail to 144). Repurposed/added
// slots: hdrTextureIndex is DEAD in the integrate shader (the HDR read
// moved to the composite; the field stays for layout stability), and
// the two tail floats are NEW (giRefreshDist + fieldCadence).
struct VolumetricPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;    // DEAD (the composite owns the HDR read)
    uint depthTextureIndex;
    uint samplerIndex;
    uint steps;
    // The CSM cascade buffer (the god-ray leg): SelectCascadeGpu's
    // splitEnd walk + the selected tile's lightVP/uv*/bias reads.
    vk::BufferPointer<DirectionalCascadeBuffer> cascadeBuffer; // 0 = off
    float2 depthTexelSize;  // full-res depth texel (1/depthExtent)
    float maxDistance;
    float density;
    float intensity;
    float anisotropy;
    float sunScale;
    float giScale;
    uint shadowDepthIndex;  // the fog sun-shadow depth atlas - the raw
                            // MAIN CSM atlas (cascade tap) or the
                            // DEDICATED volumetric atlas (coarse tile;
                            // never the EVSM moments);
                            // 0xffffffff = unshadowed sun leg
    uint useSun;
    uint useGiVolumes;     // 1 = the convex GI volumes stack into the fog
                           //     GI (0 = the quality bypass: NO volume
                           //     walks — the world-field/sky fog-GI base
                           //     still samples, independent sources)
    float jitterStrength;
    float heightFalloff;
    float heightBase;
    uint numCascades;      // 0 = no CSM fog shadowing (the unshadowed leg)
    uint fogGiSource;      // 0=worldfield (default), [1 retired], 2=sky-only
    vk::BufferPointer<ForwardPlusFogTileCountBuffer> fogTileCounts; // 0 = stale
    vk::BufferPointer<ForwardPlusFogTileIndexBuffer> fogTileIndices; // 0 = stale
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights;
    uint fpLightCount;      // lights-only bound (probes sort after)
    uint fogMaxPerTile;     // fog list stride (matches the cull push)
    uint fogGiDebug;        // 0 = off, 1 = the coverage debug view
                            //     (grayscale), 2 = the TRANSMITTANCE view
                            //     (throughput.xxx), 3 = the GOD-RAY
                            //     sun-visibility view
    float giRefreshDist;    // the baked-GI base refresh cadence (m;
                            //     was the hardcoded 0.75 - low-frequency
                            //     ambient radiance, coarser is safe)
    float fieldCadence;     // the world-field DENSITY sampling interval
                            //     (m; two cached samples, lerped per
                            //     segment - extinction-grade accuracy)
};

[[vk::push_constant]]
ConstantBuffer<VolumetricPush> vp;

// Interleaved gradient noise: march-origin dither against banding.
float VolIgn(float2 px)
{
    return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
}

// Henyey-Greenstein phase (normalized-ish, 1/(4pi) convention folded into
// artist intensity; g in [-0.9, 0.9], +forward).
float VolPhaseHg(float cosT, float g)
{
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5);
    return (1.0 - g2) / max(denom, 1e-4);
}

// Isotropic mean radiance of the SG probe field: integral over the sphere
// divided by 4pi. Integral of one unit SG lobe = 2pi/l*(1-e^-2l); with the
// fixed sharpness 4 the mean weight is (1-e^-8)/8 ~= 0.125 per lobe.
float3 VolGiMean(float3 amps[SG_GI_LOBES])
{
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        sum += amps[i];
    return sum * 0.124959f;
}

static const float kVolPi = 3.14159265359;

// One light's in-scatter radiance contribution at pos (candela convention:
// intensity * window / (4pi r^2), spots add the cone angular fade).
float3 VolLightInScatter(ForwardPlusLight pl, float3 pos, float3 rayDir)
{
    float3 toLight = pl.position - pos;
    float dist2 = dot(toLight, toLight);
    if (pl.range > 0.0f && dist2 > pl.range * pl.range)
        return float3(0.0, 0.0, 0.0);
    const float dist = sqrt(dist2);
    const float3 L = toLight / max(dist, 1e-6f);
    float window_ = 1.0f;
    if (pl.range > 0.0f)
    {
        const float x = dist / max(pl.range, 1e-6f);
        window_ = pow(saturate(1.0f - x * x * x * x), 2.0f);
    }
    const float atten = window_ / max(4.0f * kVolPi * dist * dist, 1e-2f);
    float3 plColor = pl.color.rgb * pl.intensity;
    if (pl.type == 1) // spot: cone angular fade
    {
        const float cosA = dot(-L, pl.direction);
        const float cosOuter = cos(pl.outerAngle);
        if (cosA < cosOuter)
            return float3(0.0, 0.0, 0.0);
        const float cosInner = cos(pl.innerAngle);
        plColor *= smoothstep(cosOuter, cosInner, cosA);
    }
    const float phase = VolPhaseHg(dot(rayDir, L), vp.anisotropy) * 0.0795775f;
    return plColor * (atten * phase);
}

// All forward+ lights' in-scatter at pos: the DEDICATED fog tile lists
// when fresh (cull ran this frame), else the unculled light head.
// pix is the FULL-resolution pixel (the half-res coordinate rule: the
// tile grid is full-res).
float3 VolForwardPlusLights(FrameUniforms frame, float3 pos, float3 rayDir,
    uint2 pix)
{
    const uint nLights = min(vp.fpLightCount, (uint)MAX_FP_LIGHTS);
    if (nLights == 0u)
        return float3(0.0, 0.0, 0.0);
    if (frame.giVolumeTileCountX != 0u)
    {
        const uint ts = max(frame.giVolumeTileSize, 1u);
        const uint tx = (uint)(pix.x / (float)ts);
        if (tx < frame.giVolumeTileCountX)
        {
            const uint ty = (uint)(pix.y / (float)ts);
            const uint tile = ty * frame.giVolumeTileCountX + tx;
            if (tile < (uint)MAX_FP_TILES)
            {
                const uint fSlots = min(vp.fogMaxPerTile,
                    (uint)MAX_FP_TILE_SORT_SLOTS);
                const uint listCount = min(
                    vp.fogTileCounts.Get().counts[tile], fSlots);
                const uint baseIdx = tile * fSlots;
                float3 acc = float3(0.0, 0.0, 0.0);
                [loop]
                for (uint s = 0u; s < listCount; ++s)
                {
                    const uint li = vp.fogTileIndices.Get().indices[baseIdx + s];
                    if (li >= nLights)
                        continue;
                    acc += VolLightInScatter(vp.fpLights.Get().lights[li],
                        pos, rayDir);
                }
                return acc;
            }
        }
    }
    float3 acc = float3(0.0, 0.0, 0.0);
    [loop]
    for (uint li = 0u; li < nLights; ++li)
        acc += VolLightInScatter(vp.fpLights.Get().lights[li], pos, rayDir);
    return acc;
}

// ---------------------------------------------------------------------------
// CSM fog sun shadowing (the god-ray leg): ONE unfiltered depth-delta
// tap per active fog segment against the RAW CSM atlas (see the former
// volumetric.frag.hlsl header - the surface path's conventions, the
// depth-delta soft penumbra, the fallthrough).
// FORM NOTE: the tap samples with NORMALIZED atlas UV (SampleLevel,
// PCFShadowAtlas's form), never a texel Load scaled by physical dims -
// the descriptors carry uvScale/uvBias/clamps and the normalized form
// respects them exactly (a texel form would re-derive them and drift).
// ---------------------------------------------------------------------------
float SampleFogSunCSM(float3 wpos, float4x4 view)
{
    float viewDepth = max(-mul(view, float4(wpos, 1.0)).z, 0.0);
    int first = SelectCascadeGpu(viewDepth, vp.cascadeBuffer, vp.numCascades);

    [loop]
    for (int k = first; k < int(vp.numCascades); ++k)
    {
        float4 clip = mul(
            vp.cascadeBuffer.Get().dir.cascades[k].lightVP,
            float4(wpos, 1.0));

        if (clip.w <= 0.0)
            continue;

        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * 0.5 + 0.5;

        if (ndc.z <= 0.0 || ndc.z >= 1.0 ||
            any(uv < 0.0) || any(uv > 1.0))
            continue;

        float2 atlasUV =
            uv * vp.cascadeBuffer.Get().dir.cascades[k].uvScale +
            vp.cascadeBuffer.Get().dir.cascades[k].uvBias;
        atlasUV = clamp(
            atlasUV,
            vp.cascadeBuffer.Get().dir.cascades[k].uvClampMin,
            vp.cascadeBuffer.Get().dir.cascades[k].uvClampMax);

        SamplerState smp = samplers[NonUniformResourceIndex(vp.samplerIndex)];
        float storedDepth = textures[vp.shadowDepthIndex]
            .SampleLevel(smp, atlasUV, 0.0).r;

        float bias =
            vp.cascadeBuffer.Get().dir.cascades[k].bias * 0.3;
        float receiver = ndc.z - bias;

        static const float kFogSunSoften = 16.0;
        float soft = max(
            vp.cascadeBuffer.Get().dir.cascades[k].bias * kFogSunSoften,
            1e-6);
        float delta = storedDepth - receiver;
        return saturate(delta / soft + 0.5);
    }

    return 1.0;
}

// ---------------------------------------------------------------------------
// The COARSE volumetric shadow atlas (the perf plan item 5): the
// dedicated sun-oriented ortho over the camera fog frustum with ONLY
// the marked large occluders rendered (RecordVolumetricShadowCasters)
// into its OWN half-res D32 image (fogShadowRect = the identity
// (0,0,1,1) over that atlas - kept as a rect so the mapping stays
// source-agnostic). ONE depth fetch, NO cascade selection loop, NO
// PCF - the depth-delta soft ramp (the CSM tap's twin, on the atlas's
// own bias). enabled = 0 OR an unbound feed index (no atlas / no
// marked casters / mode off) returns fully lit - never sample an
// out-of-range descriptor. Works under EVSM: the atlas renders through
// its own RAW depth pass, never the EVSM moment path.
// ---------------------------------------------------------------------------
float SampleVolumetricShadow(float3 wpos, FrameUniforms frame)
{
    if (frame.fogShadowEnabled == 0u
        || vp.shadowDepthIndex == 0xFFFFFFFFu)
        return 1.0;

    float4 clip = mul(frame.fogShadowVP, float4(wpos, 1.0));
    if (clip.w <= 0.0)
        return 1.0;

    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    // Reject positions outside the receiver volume (the ortho fit over
    // the fog frustum): never read as shadowed.
    if (ndc.z <= 0.0 || ndc.z >= 1.0 ||
        any(uv < 0.0) || any(uv > 1.0))
        return 1.0;

    float2 atlasUV = frame.fogShadowRect.xy
        + uv * frame.fogShadowRect.zw;
    atlasUV = clamp(atlasUV, frame.fogShadowRect.xy,
        frame.fogShadowRect.xy + frame.fogShadowRect.zw);

    SamplerState smp = samplers[NonUniformResourceIndex(vp.samplerIndex)];
    float storedDepth = textures[vp.shadowDepthIndex]
        .SampleLevel(smp, atlasUV, 0.0).r;

    float receiver = ndc.z - frame.fogShadowBias * 0.3;
    static const float kFogSunSoften = 16.0;
    float soft = max(frame.fogShadowBias * kFogSunSoften, 1e-6);
    float delta = storedDepth - receiver;
    return saturate(delta / soft + 0.5);
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = vp.frameUniforms.Get();
    Texture2D depthTex = textures[vp.depthTextureIndex];

    float density = max(vp.density, 0.0f);
    float intensity = max(vp.intensity, 0.0f);
    // The passthrough fog (no scatter, full transmittance) every early
    // exit writes - the composite's upsample consumes it like any fog
    // texel.
    const float4 passthrough = float4(0.0, 0.0, 0.0, 1.0);

    const bool zoneFogPossible = (frame.envZoneFlags & 2u) != 0u;
    if ((density <= 1e-6 && frame.worldFieldCount == 0u && !zoneFogPossible)
        || intensity <= 1e-6)
    {
        // Still need the representative depth for the upsample weights.
        float2 depthSize = float2(1.0 / max(vp.depthTexelSize.x, 1e-9),
                                  1.0 / max(vp.depthTexelSize.y, 1e-9));
        int2 depthPx = int2(clamp(IN.uv * depthSize - 0.5,
            float2(0.0, 0.0), depthSize - 1.0));
        float ndcDepth = depthTex.Load(int3(depthPx, 0)).r;
        OUT.outFog = passthrough;
        OUT.outDepth = float4(DofIsBackgroundOrGarbage(ndcDepth)
            ? max(vp.maxDistance, 1.0)
            : min(DofViewDist(ndcDepth, IN.uv * 2.0 - 1.0, frame.invProj),
                max(vp.maxDistance, 1.0)), 0.0, 0.0, 1.0);
        return OUT;
    }

    // ---- The receiver block: the 2x2 FULL-res texels this half-res
    // pixel covers; the NEAREST VALID (reverse-Z max) is the
    // representative - the same block-rep discipline as the SSR/AO
    // chains, never a fabricated center across a silhouette. The
    // composite's bilateral weights key on THIS depth (outDepth).
    float2 depthSize = float2(1.0 / max(vp.depthTexelSize.x, 1e-9),
                              1.0 / max(vp.depthTexelSize.y, 1e-9));
    int2 halfPx = int2(IN.pos.xy);
    int2 fullBase = halfPx * 2;
    float dBest = 0.0;
    int2 repPx = fullBase;
    {
        static const int2 kOff[4] = {
            int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
        [unroll]
        for (int j = 0; j < 4; ++j)
        {
            const float d = depthTex
                .Load(int3(fullBase + kOff[j], 0)).r;
            if (!DofIsBackgroundOrGarbage(d) && d > dBest)
            {
                dBest = d;
                repPx = fullBase + kOff[j];
            }
        }
    }
    // The representative UV: the SELECTED texel's center (fallback: the
    // block's uv - an all-background block).
    float2 repUv = IN.uv;
    if (!DofIsBackgroundOrGarbage(dBest))
        repUv = (float2(repPx) + 0.5) / depthSize;
    float ndcDepth = dBest;

    // Camera ray in world space (exact through invViewProj, off the
    // representative UV - the ray the march actually integrates).
    float2 ndcXY = repUv * 2.0 - 1.0;
    float4 farH = mul(frame.invViewProj, float4(ndcXY, 0.0, 1.0)); // reversed-Z far
    float3 worldFar = farH.xyz / max(farH.w, 1e-9);
    float3 camPos = frame.cameraPos;
    float3 rayDir = worldFar - camPos;
    float farLen = length(rayDir);
    rayDir = (farLen > 1e-6) ? rayDir / farLen : float3(0.0, 0.0, 1.0);

    float rayLen;
    float repDist; // the representative view distance (the upsample key)
    if (DofIsBackgroundOrGarbage(ndcDepth))
    {
        rayLen = max(vp.maxDistance, 1.0);
        repDist = rayLen;
    }
    else
    {
        float4 hitH = mul(frame.invViewProj, float4(ndcXY, ndcDepth, 1.0));
        float3 worldHit = hitH.xyz / max(hitH.w, 1e-9);
        rayLen = clamp(length(worldHit - camPos), 0.0, max(vp.maxDistance, 1.0));
        repDist = rayLen;
    }
    if (rayLen <= 1e-3)
    {
        OUT.outFog = passthrough;
        OUT.outDepth = float4(max(repDist, 1e-3), 0.0, 0.0, 1.0);
        return OUT;
    }

    uint nSteps = clamp(vp.steps, 4, 128);
    // Static IGN dither for the march origin (banding guard), hashed at
    // the NATIVE (half-res) pixel grid - the grain rides the half-res
    // lattice and the composite's upsample smooths it.
    float2 halfPxF = IN.pos.xy;
    float jitter = VolIgn(halfPxF) * clamp(vp.jitterStrength, 0.0, 1.0);

    float3 sunDir = normalize(frame.env.sunDirection.xyz + 1e-9);
    float3 sunHdr = frame.env.sunColor.rgb * max(frame.env.sunColor.w, 0.0f);
    float sunOn = (vp.useSun != 0) ? 1.0 : 0.0;
    float cosSun = dot(rayDir, sunDir);
    float phase = VolPhaseHg(cosSun, vp.anisotropy) * 0.0795775; // /4pi
    float giStrength = max(frame.giIntensity, 0.0f) * max(vp.giScale, 0.0f);

    // ---- Fog sun-shadow prep (loop-invariant): the shadow atlas index
    // bound. Two lookup forms, SELECTABLE (never combined - the fog
    // sun-shadow mode): the COARSE tile (frame.fogShadowEnabled, the
    // dedicated marked-caster atlas - works under EVSM) takes priority;
    // the CSM cascade tap (vp.numCascades, PCF mode only) is the
    // fallback; neither bound = the unshadowed sun leg. BOTH legs
    // require the atlas index: an unbound feed (0xffffffff) must NEVER
    // sample - an out-of-range descriptor fetch is undefined garbage
    // that can change per frame (the pre-feed-wiring coarse leg did
    // exactly that: visibly "jumping" fog shadows).
    // ----
    const bool coarseShadowSun = frame.fogShadowEnabled != 0u
        && vp.shadowDepthIndex != 0xFFFFFFFFu;
    const bool csmShadowSun =
        !coarseShadowSun
        && vp.numCascades > 0u
        && vp.shadowDepthIndex != 0xFFFFFFFFu;
    const bool shadowSun = (coarseShadowSun || csmShadowSun)
        && vp.useSun != 0u;

    // The FULL-resolution pixel for the pixel-keyed lookups (the tile
    // lists + the GI-volume tile mask - the half-res coordinate rule).
    float2 fullPxF = repUv * depthSize - 0.5;
    const uint2 fullPxU = uint2(clamp(fullPxF, float2(0.0, 0.0),
        depthSize - 1.0));

    const float3 skyMean =
        EvaluateIrradiance(float3(0.0, 1.0, 0.0), frame.env) * 0.31831;
    const bool volumeGiOn = (vp.useGiVolumes != 0u) && (frame.giVolumesEnabled != 0u);
    uint volMask = 0u;
    if (volumeGiOn)
        volMask = SgGiVolumeTileMask(frame, fullPxU);

    // ---- Analytic env zones, PER SEGMENT (the tile cache - unchanged).
    const bool zonesLive = frame.envZoneCount > 0u
        && (frame.envZoneFlags & 1u) != 0u;
    EnvZoneMarchCache zoneCache = (EnvZoneMarchCache)0;
    zoneCache.rec = 0xFFFFFFFFu; // force the first record fetch

    // GI refresh cadence: CONFIGURABLE (the push's giRefreshDist - was
    // the hardcoded 0.75; the baked world-field/sky GI base is
    // low-frequency AMBIENT RADIANCE, so a coarser cadence costs no
    // optical accuracy - unlike the density cadence below). The volume
    // BLEND WEIGHTS stay exact per segment (SgGiVolumeRayComposite
    // runs every segment regardless).
    const float giRefreshDist = max(vp.giRefreshDist, 0.1);
    float giT = -1e30; // distance of the last refresh (expired -> seg 0)

    // Light refresh cadence: DECOUPLED from the GI cadence (unchanged).
    static const float kVolLightRefreshDist = 3.0;
    float lightT = -1e30;

    // ---- World-field DENSITY cadence (the perf plan item 3): the
    // virtual field lookup (page table + trilinear 3D fetch) runs
    // every fieldCadence meters; the two bracketing samples lerp per
    // segment. Density controls OPTICAL DEPTH, so the cadence stays
    // tight (1 m default) and the lerp is linear in the field value -
    // the segment integration itself still uses the interpolated
    // density EXACTLY (the exact (1-e^-tau) form absorbs it). Zone
    // blending, height falloff and extinction all run per segment as
    // before - only the FIELD FETCH is cadenced.
    const float fieldCadence = max(vp.fieldCadence, 0.1);
    float fieldDistA = -1e30; // distance of sample A
    float fieldDistB = -1e30; // distance of sample B (A < t <= B)
    float fieldValA = 0.0;
    float fieldValB = 0.0;
    const bool fieldFogOn = frame.worldFieldCount > 0u;

    // ---- The fog-GI COVERAGE debug view (fogGiDebug == 1):
    // (dbg, 0) - the composite shows the pure upsampled view.
    if (vp.fogGiDebug == 1u) {
        const FogGiSample gi =
            SampleFogGi(frame, camPos + rayDir * rayLen, 0u);
        float3 dbg;
        if (!gi.resident)
            dbg = float3(0.9, 0.1, 0.1);
        else if (!gi.coverageResident)
            dbg = float3(0.1, 0.9, 0.2);
        else
            dbg = float3(gi.coverage, gi.coverage, gi.coverage);
        OUT.outFog = float4(dbg, 0.0);
        OUT.outDepth = float4(repDist, 0.0, 0.0, 1.0);
        return OUT;
    }

    // Ray-space volume prep (once per pixel).
    GiVolumeRayPrep rayPrep;
    float3 baseHeld = skyMean;
    if (volumeGiOn)
        SgGiVolumeRayPrep(frame, volMask, camPos, rayDir, rayLen, rayPrep);

    float3 lightsHeld = float3(0.0, 0.0, 0.0);
    const bool fogLightsOn = vp.fpLightCount > 0u;

    float3 ambient = skyMean * giStrength;

    float throughput = 1.0;
    float3 scatter = float3(0.0, 0.0, 0.0);
    // The god-ray leg's visibility accumulators (fogGiDebug==3 the CSM
    // map, ==5 the coarse-tile map - the ground-truth instruments).
    float visSum = 0.0;
    uint visCount = 0u;
    float coarseVisSum = 0.0;
    uint coarseVisCount = 0u;

    [loop]
    for (uint i = 0; i < 128; ++i)
    {
        if (i >= nSteps)
            break;
        float fj0 = float(i) / float(nSteps);
        float fj1 = float(i + 1u) / float(nSteps);
        float s0 = rayLen * fj0 * fj0;
        float s1 = rayLen * fj1 * fj1;
        float segW = max(s1 - s0, 1e-6);
        float t = s0 + jitter * segW;
        float3 wpos = camPos + rayDir * t;

        // ---- Zone volumetrics, PER SEGMENT through the tile cache.
        float segIntensity = intensity;
        float segDensity = density;
        float fogRatio = 1.0;
        if (zonesLive)
        {
            float3 segMid = camPos + rayDir * ((s0 + s1) * 0.5);
            float zoneW = 0.0;
            EnvZoneLook zLook;
            if (EnvZoneSegment(segMid, frame, zoneCache, zoneW, zLook))
            {
                EnvZoneVolumetrics(zLook, zoneW, intensity, density,
                    segIntensity, segDensity);
                fogRatio = lerp(1.0, max(zLook.volumetrics, 0.0),
                    saturate(zoneW));
            }
        }

        float sigma = segDensity;
        if (vp.heightFalloff > 0.0)
            sigma *= exp(-max(wpos.y - vp.heightBase, 0.0) * vp.heightFalloff);
        // ---- World-field voxel fog ON THE CADENCE: advance the sample
        // bracket while the segment is past sample B, then lerp. The
        // first segment primes both samples at t and t+cadence.
        if (fieldFogOn)
        {
            if (fieldDistB < t - 1e-4)
            {
                // Shift B into A; the new B samples one cadence past
                // the old B (the first-ever call primes A at t).
                if (fieldDistA > -1e29)
                {
                    fieldDistA = fieldDistB;
                    fieldValA = fieldValB;
                    fieldDistB = fieldDistA + fieldCadence;
                }
                else
                {
                    fieldDistA = t;
                    fieldValA = SampleWorldField(frame,
                        WORLD_FIELD_ID_FOG, camPos + rayDir * t, 0u).r;
                    fieldDistB = fieldDistA + fieldCadence;
                }
                fieldValB = SampleWorldField(frame, WORLD_FIELD_ID_FOG,
                    camPos + rayDir * fieldDistB, 0u).r;
            }
            const float fFrac = saturate(
                (t - fieldDistA) / max(fieldDistB - fieldDistA, 1e-4));
            sigma += lerp(fieldValA, fieldValB, fFrac) * fogRatio;
        }

        // ---- Ambient: EXACT per-segment volume weights (unchanged) +
        // the cadenced base refresh.
        float3 volAcc = float3(0.0, 0.0, 0.0);
        float volTrans = 1.0;
        if (volumeGiOn)
            SgGiVolumeRayComposite(frame, rayPrep, t, wpos, volAcc, volTrans);
        if (t - giT >= giRefreshDist)
        {
            giT = t;
            if (volTrans > 0.0) {
                if (vp.fogGiSource == 2u) {
                    baseHeld = skyMean;
                }
                else {
                    const FogGiSample gi =
                        SampleFogGi(frame, wpos, 0u);
                    if (gi.resident && gi.coverage > 0.02)
                        baseHeld = gi.radiance;
                    else
                        baseHeld = skyMean;
                }
            }
        }
        ambient = (volAcc + baseHeld * volTrans) * giStrength;

        // ---- Forward+ light in-scatter (own cadence, unchanged; the
        // tile walk keys on the FULL-res pixel).
        if (fogLightsOn && t - lightT >= kVolLightRefreshDist) {
            lightT = t;
            lightsHeld = VolForwardPlusLights(frame, wpos, rayDir,
                fullPxU);
        }

        float3 source = ambient + lightsHeld;
        // ---- The sun leg, SHADOWED by the SELECTED fog sun-shadow
        // mode (never both - multiplying two occlusion forms would
        // darken the fog twice): the COARSE tile first (the dedicated
        // marked-caster pass), else the CSM cascade tap. A BLOCKED
        // segment is NOT skipped: visibility gates ONLY the
        // directional sun term (under a roof the sun shaft dies, the
        // ambient fog stays).
        if (sunOn > 0.5)
        {
            float visibility = 1.0;
            if (shadowSun && sigma > 1e-6 && segIntensity > 1e-6)
            {
                if (coarseShadowSun)
                {
                    visibility = SampleVolumetricShadow(wpos, frame);
                    coarseVisSum += visibility;
                    coarseVisCount += 1u;
                }
                else
                {
                    visibility = SampleFogSunCSM(wpos, frame.view);
                    visSum += visibility;
                    visCount += 1u;
                }
            }
            source += sunHdr * (phase * max(vp.sunScale, 0.0f)) * visibility;
        }
        source = SanitizeHdr(source);

        // ---- Exact segment integration (unchanged) + the zone-off
        // pass-through skip.
        if (segIntensity > 1e-6)
        {
            const float trans = exp(-sigma * segW);
            const float scatterWeight =
                throughput * segIntensity * (1.0 - trans);
            scatter += SanitizeHdr(source * scatterWeight);
            throughput *= trans;
        }

        if (throughput < 3.333333e-4f)
        {
            throughput = 0.0;
            break;
        }
    }

    // ---- The TRANSMITTANCE view (fogGiDebug==2): (dbg, 0).
    if (vp.fogGiDebug == 2u) {
        OUT.outFog = float4(throughput.xxx, 0.0);
        OUT.outDepth = float4(repDist, 0.0, 0.0, 1.0);
        return OUT;
    }
    // ---- The GOD-RAY visibility view (fogGiDebug==3): the CSM taps.
    if (vp.fogGiDebug == 3u) {
        const float meanVis = visCount > 0u
            ? visSum / float(visCount) : 1.0;
        const float dbgGain = 1.0 / max(frame.exposure, 1e-6);
        OUT.outFog = float4((meanVis * dbgGain).xxx, 0.0);
        OUT.outDepth = float4(repDist, 0.0, 0.0, 1.0);
        return OUT;
    }
    // ---- The COARSE-ATLAS views (fogGiDebug==4/5 - the perf plan
    // item 5's diagnostics): 4 = the raw atlas DEPTH at each pixel's
    // fog-shadow projection (the atlas's own content; the standard-Z
    // stored value, bright = far/lit), 5 = the coarse fog-shadow
    // VISIBILITY (the march's mean SampleVolumetricShadow - the
    // mode-3 twin for the atlas).
    if (vp.fogGiDebug == 4u) {
        const float dbgGain = 1.0 / max(frame.exposure, 1e-6);
        float dbg = 1.0;
        if (frame.fogShadowEnabled != 0u
            && vp.shadowDepthIndex != 0xFFFFFFFFu) {
            float4 clip = mul(frame.fogShadowVP,
                float4(camPos + rayDir * rayLen, 1.0));
            if (clip.w > 0.0) {
                float3 ndc = clip.xyz / clip.w;
                float2 uv = ndc.xy * 0.5 + 0.5;
                if (ndc.z > 0.0 && ndc.z < 1.0
                    && all(uv >= float2(0.0, 0.0))
                    && all(uv <= float2(1.0, 1.0))) {
                    float2 atlasUV = frame.fogShadowRect.xy
                        + uv * frame.fogShadowRect.zw;
                    atlasUV = clamp(atlasUV, frame.fogShadowRect.xy,
                        frame.fogShadowRect.xy + frame.fogShadowRect.zw);
                    SamplerState smp =
                        samplers[NonUniformResourceIndex(vp.samplerIndex)];
                    dbg = textures[vp.shadowDepthIndex]
                        .SampleLevel(smp, atlasUV, 0.0).r;
                }
            }
        }
        OUT.outFog = float4((dbg * dbgGain).xxx, 0.0);
        OUT.outDepth = float4(repDist, 0.0, 0.0, 1.0);
        return OUT;
    }
    if (vp.fogGiDebug == 5u) {
        const float meanVis = coarseVisCount > 0u
            ? coarseVisSum / float(coarseVisCount) : 1.0;
        const float dbgGain = 1.0 / max(frame.exposure, 1e-6);
        OUT.outFog = float4((meanVis * dbgGain).xxx, 0.0);
        OUT.outDepth = float4(repDist, 0.0, 0.0, 1.0);
        return OUT;
    }

    // The integration result: (scatter, throughput). The composite
    // applies hdr * a + rgb at full resolution.
    OUT.outFog = float4(SanitizeHdr(scatter), saturate(throughput));
    OUT.outDepth = float4(repDist, 0.0, 0.0, 1.0);
    return OUT;
}
