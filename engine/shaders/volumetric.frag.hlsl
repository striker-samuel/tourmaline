// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define VOLUMETRIC_PASS
#include "common.hlsli"
#include "environment.hlsli"
#include "sggi.hlsli"
#include "sg_regular.hlsli"
#include "sg_gi_volume.hlsli"
#include "dof.hlsli"
#include "world_field.hlsli"

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/volumetric.h VolumetricPush (128 bytes).
struct VolumetricPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    uint steps;
    float2 texelSize;       // output texel (1/outputExtent)
    float2 depthTexelSize;  // full-res depth texel (1/depthExtent)
    float maxDistance;
    float density;
    float intensity;
    float anisotropy;
    float sunScale;
    float giScale;
    uint useGi;
    uint useSun;
    uint useGiVolumes;     // 1 = GI volumes stack into the fog GI
    float jitterStrength;
    float heightFalloff;
    float heightBase;
    float time;
    uint fogGiSource;      // 0=worldfield (default), 1=octree (A/B), 2=sky-only; was pad1
    // Forward+ light feed for fog in-scatter (the forward tiled lights
    // contribute to the march, not just the surfaces): the DEDICATED fog
    // tile lists ride the cull freshness contract — frame.giVolumeTileCountX
    // != 0 implies fresh (one dispatch writes both); the light buffer +
    // count publish whenever uploaded.
    vk::BufferPointer<ForwardPlusFogTileCountBuffer> fogTileCounts; // 0 = stale
    vk::BufferPointer<ForwardPlusFogTileIndexBuffer> fogTileIndices; // 0 = stale
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights;
    uint fpLightCount;      // lights-only bound (probes sort after)
    uint fogMaxPerTile;     // fog list stride (matches the cull push)
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
// The mean is LINEAR in the amps — the fog consumes it through the
// dedicated one-fetch/one-accumulator paths (SgGiVolumeRayComposite's lazy
// mean fetch + SgGiSampleMean; the CPU packs per-volume mean textures
// with the same constant, GiVolume::PackGpuMeanLayer): the constant MUST
// stay in sync in all three places.
float3 VolGiMean(float3 amps[SG_GI_LOBES])
{
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        sum += amps[i];
    return sum * 0.124959f;
}

// ---------------------------------------------------------------------------
// Forward+ tiled lights -> fog in-scatter (the same lights that shade the
// surfaces now light the medium). SCALAR ONLY (fog has no normal: no
// Lambert, no specular, no shadowing — single scattering) — the in-scatter
// core of EvaluateForwardPlusLight (default_lit_body.hlsli) with its
// Cook-Torrance half dropped, plus the HG phase toward the VIEWER (the
// same p(cosT)/4pi the sun leg applies). Cookies/light functions are
// surface effects and stay out. Per REFRESH, not per segment (ambient
// cadence): the lights sit at fixed world positions, so the field is as
// smooth as the ambient — and the per-light loop never runs inside the
// hot march.
// Tile lists are fresh when the cull ran this frame (the app publishes the
// list addresses under the same freshness contract as the GI-volume tile masks;
// frame.giVolumeTileCountX != 0 is the cross-check — one dispatch writes both).
// Stale/absent lists (or an off-grid tile) walk [0, fpLightCount)
// directly — correct, just unculled.
// ---------------------------------------------------------------------------
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
    // Frostbite windowed falloff (EvaluateForwardPlusLight's core).
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
    // Phase toward the viewer (the sun leg's p(cosT)/4pi — the medium
    // scatters light from every arrival direction, so each light pays
    // its own phase instead of the sun's fixed cosSun).
    const float phase = VolPhaseHg(dot(rayDir, L), vp.anisotropy) * 0.0795775f;
    return plColor * (atten * phase);
}

// All forward+ lights' in-scatter at pos: the DEDICATED fog tile lists
// when fresh (cull ran this frame), else the unculled light head.
// The fog lists are separate from the surface lists on purpose: their
// candidacy ignores the tile depth slab (march samples live in front of
// the nearest opaque surface — slab cuts split the fog on the tile grid
// at depth discontinuities) and only cuts beyond the march range, so a
// list is valid for EVERY depth along the pixel's ray through the tile.
// Bounded + relevance-sorted by the cull: the per-refresh walk below
// costs at most fogMaxPerTile lights even in dense crowds.
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
                // Same stride clamp as the cull (both derive from the
                // desc's maxFogPerTile, so the rows always agree).
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
                        continue; // probe entries never ride the fog list; belt+braces
                    acc += VolLightInScatter(vp.fpLights.Get().lights[li],
                        pos, rayDir);
                }
                return acc;
            }
        }
    }
    // Stale/absent lists: unculled walk over the light head.
    float3 acc = float3(0.0, 0.0, 0.0);
    [loop]
    for (uint li = 0u; li < nLights; ++li)
        acc += VolLightInScatter(vp.fpLights.Get().lights[li], pos, rayDir);
    return acc;
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = vp.frameUniforms.Get();
    // Push-constant indices are wave-UNIFORM (same for every pixel of the
    // draw): plain bindless indexing keeps the descriptor fetches scalar.
    // NonUniformResourceIndex here would force divergent descriptor paths
    // for indices that can never diverge.
    Texture2D hdrTex = textures[vp.hdrTextureIndex];
    Texture2D depthTex = textures[vp.depthTextureIndex];
    SamplerState smp = samplers[vp.samplerIndex];

    float3 base = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);

    float density = max(vp.density, 0.0f);
    float intensity = max(vp.intensity, 0.0f);
    // World-field voxel fog (Engine/WorldFields): with a live field
    // (frame.worldFieldCount > 0 — id 0 is the fog density field, the
    // registration-order contract), zero ANALYTIC density must still
    // march: the resident bricks carry their own extinction (missing
    // bricks decode to the field's 0 fallback — no contribution, no
    // seams). Only both-zero (no field AND no analytic density) or zero
    // intensity early out.
    if ((density <= 1e-6 && frame.worldFieldCount == 0u) || intensity <= 1e-6)
    {
        OUT.outColor = float4(base, 1.0f);
        return OUT;
    }

    // Full-res depth tap for this pixel (output == render resolution, so
    // IN.uv maps 1:1 onto depth texels; downstream samples us by UV).
    float2 depthSize = float2(1.0 / max(vp.depthTexelSize.x, 1e-9),
                              1.0 / max(vp.depthTexelSize.y, 1e-9));
    int2 depthPx = int2(clamp(IN.uv * depthSize - 0.5,
                              float2(0.0, 0.0), depthSize - 1.0));
    float ndcDepth = depthTex.Load(int3(depthPx, 0)).r;

    // Camera ray in world space (exact, flip/finite-far agnostic: unproject
    // through invViewProj like the depth helpers do via invProj).
    float2 ndcXY = IN.uv * 2.0 - 1.0;
    float4 farH = mul(frame.invViewProj, float4(ndcXY, 0.0, 1.0)); // reversed-Z far
    float3 worldFar = farH.xyz / max(farH.w, 1e-9);
    float3 camPos = frame.cameraPos;
    float3 rayDir = worldFar - camPos;
    float farLen = length(rayDir);
    rayDir = (farLen > 1e-6) ? rayDir / farLen : float3(0.0, 0.0, 1.0);

    float rayLen;
    if (DofIsBackgroundOrGarbage(ndcDepth))
    {
        rayLen = max(vp.maxDistance, 1.0);
    }
    else
    {
        float4 hitH = mul(frame.invViewProj, float4(ndcXY, ndcDepth, 1.0));
        float3 worldHit = hitH.xyz / max(hitH.w, 1e-9);
        rayLen = clamp(length(worldHit - camPos), 0.0, max(vp.maxDistance, 1.0));
    }
    if (rayLen <= 1e-3)
    {
        OUT.outColor = float4(base, 1.0f);
        return OUT;
    }

    // ---- Analytic env zones (see environment.hlsli EnvZone*): the march
    // constants (scattering ratio + extinction density) resolve at the
    // ray's MIDPOINT in 3D and lerp with the pass base by blendWeight *
    // falloff. The near-weighted segment distribution concentrates samples
    // in the first meters, so the midpoint reads as the pixel's fog; a
    // zone with volumetrics 0 drives the blend to 0 and the march
    // degenerates to pass-through (the composite returns base). ----
    {
        float zoneWeight = 0.0;
        EnvZoneLook zoneLook;
        if (EnvZoneAt(camPos + rayDir * (rayLen * 0.5), frame,
            zoneWeight, zoneLook))
        {
            float zIntensity = intensity;
            float zDensity = density;
            EnvZoneVolumetrics(zoneLook, zoneWeight, intensity, density,
                zIntensity, zDensity);
            intensity = zIntensity;
            density = zDensity;
        }
    }

    uint nSteps = clamp(vp.steps, 4, 128);
    // Static IGN dither for the march origin (banding guard), hashed at the
    // native pixel grid so the grain is per-pixel fine. Deliberately NOT
    // time-animated: with no temporal accumulation the drift term only makes
    // the field swim/flicker. fullPxF matches the depth tap above.
    float2 fullPxF = IN.uv * depthSize - 0.5;
    float jitter = VolIgn(fullPxF) * clamp(vp.jitterStrength, 0.0, 1.0);

    float3 sunDir = normalize(frame.env.sunDirection.xyz + 1e-9);
    float3 sunHdr = frame.env.sunColor.rgb * max(frame.env.sunColor.w, 0.0f);
    float iblOn = saturate(frame.env.fogEndAndFlags.w);
    float giOn = (vp.useGi != 0 && frame.giEnabled != 0) ? 1.0 : 0.0;
    float sunOn = (vp.useSun != 0) ? 1.0 : 0.0;
    float cosSun = dot(rayDir, sunDir);
    float phase = VolPhaseHg(cosSun, vp.anisotropy) * 0.0795775; // /4pi
    float giStrength = max(frame.giIntensity, 0.0f) * max(vp.giScale, 0.0f);

    // ---- Loop-invariant GI bases (the old body re-evaluated the sky
    // ambient every segment; these are pixel-constants) ----
    // Sky's up-facing mean — the fog ambient fallback (no octree
    // coverage, no volumes).
    const float3 skyMean =
        EvaluateIrradiance(float3(0.0, 1.0, 0.0), frame.env) * 0.31831;
    // Fog GI sources: giOn = the octree world field, giVolumesOn = GI
    // volumes stacking over it. giVolumesOn is the fog-GI quality bypass
    // (useGiVolumes = 0): octree/sky only ??? no tile-mask load, no volume
    // walks.
    const bool giVolumesOn = (vp.useGiVolumes != 0u) && (frame.giVolumesEnabled != 0u);
    const bool fogGiOn = (giOn > 0.5) || giVolumesOn;
    // Per-tile volume bitmask is pixel-constant: one lookup per PIXEL
    // (0 under the bypass — the mean composite then walks nothing).
    uint volMask = 0u;
    if (giVolumesOn)
        volMask = SgGiVolumeTileMask(frame, IN.pos.xy);
    // Scattering intensity hoisted (push constant, but keeps the hot
    // segment clean of the max()).
    const float intensity_ = intensity;

    // GI refresh cadence: means and the octree/sky base are spatially
    // low-frequency, so the march refreshes them every
    // kVolGiRefreshDist world units along the ray and HOLDS them
    // between refreshes — 16-64 fog steps no longer mean 16-64
    // independent octree walks + mean fetches per pixel. The volume
    // WEIGHTS are exact per segment (ray-space FMA ramps — no cadence
    // on the blend itself), so fade bands stay crisp while the held
    // quantities are the smooth ones. Distance-based, not step-based:
    // the quadratic step distribution makes near steps millimeters and
    // far steps meters, so a step cadence would thrash the near end
    // (no savings) and starve the far end. 0.75 m sits in the 0.5-1.0 m
    // band ~ typical probe spacing at bake densities.
    static const float kVolGiRefreshDist = 0.75;
    float giT = -1e30; // distance of the last refresh (expired -> seg 0)

    // Light refresh cadence: DECOUPLED from the GI cadence above. Local
    // lights are point/spot emitters with 1-10 m influence radii — the
    // in-scatter field they produce is smooth enough to hold for 3 m
    // (vs 0.75 m for the probe ambient which sits in baked density
    // gradients), but a 0.75 m light cadence meant ~15 full tile-list
    // walks per pixel on a 16-step 80 m march — the dominant per-pixel
    // fog-lighting cost (the review's finding #1). 3 m sits in the
    // 2-4 m band the review suggested; near small lights the 1 m
    // segment distribution still resolves the scatter ramp (the march
    // SAMPLES exact per segment; only the LIGHT LIST walk is held).
    static const float kVolLightRefreshDist = 3.0;
    float lightT = -1e30; // distance of the last light refresh

    // Ray-space volume prep (once per pixel): tile mask -> fade-expanded
    // AABB intervals vs the march ray -> base/slope plane coefficients
    // for the overlapping volumes. The march then blends exact weights
    // with FMA-only ramps (no plane loads, no dot3s per segment).
    GiVolumeRayPrep rayPrep;
    float3 baseHeld = skyMean;
    if (fogGiOn)
        SgGiVolumeRayPrep(frame, volMask, camPos, rayDir, rayLen, rayPrep);

    // Held forward+ light in-scatter (per-LIGHT-refresh — decoupled from
    // the GI cadence above, 3 m vs 0.75 m; the lights sit at fixed world
    // positions, so the field is as smooth as the ambient; never
    // evaluated inside the hot march).
    float3 lightsHeld = float3(0.0, 0.0, 0.0);
    const bool fogLightsOn = vp.fpLightCount > 0u;

    // Ambient base: the GI-off value is pixel-constant (one sky eval for
    // the whole march); the GI-on value refreshes on the cadence below.
    float3 ambient = skyMean * giStrength;
    if (!fogGiOn)
        ambient *= iblOn;

    // Extinction is achromatic — scalar throughput (the old float3 kept
    // three identical components alive through the whole march).
    float throughput = 1.0;
    float3 scatter = float3(0.0, 0.0, 0.0);

    [loop]
    for (uint i = 0; i < 128; ++i)
    {
        if (i >= nSteps)
            break;
        // Near-weighted (quadratic) step distribution: segment i spans
        // [rayLen*(i/N)^2, rayLen*((i+1)/N)^2], so samples concentrate near
        // the camera where the transmittance weight ~1. Uniform steps do the
        // opposite: the far samples that decide total extinction jump a full
        // coarse step per dithered pixel, so neighbors alternate between
        // extinctions (black-pixel checker on long/dense sightlines) while
        // wasting samples where the weight has already decayed to ~0.
        float fj0 = float(i) / float(nSteps);
        float fj1 = float(i + 1u) / float(nSteps);
        float s0 = rayLen * fj0 * fj0;
        float s1 = rayLen * fj1 * fj1;
        float segW = max(s1 - s0, 1e-6);
        float t = s0 + jitter * segW;
        float3 wpos = camPos + rayDir * t;

        float sigma = density;
        if (vp.heightFalloff > 0.0)
            sigma *= exp(-max(wpos.y - vp.heightBase, 0.0) * vp.heightFalloff);
        // ---- World-field voxel fog (Engine/WorldFields): additional
        // extinction from the resident 3D brick field — BC4 sqrt-density
        // decoded to m^-1 via the field's decodeScale (the .r channel;
        // id 0 = fog per WORLD_FIELD_ID_*). LOD 0 (1 m voxels) at the
        // segment sample; the sampler's coarser-walk picks up resident
        // coarse bricks when fine pages are still streaming, and every
        // miss decodes to the field's 0 fallback — missing/unstreamed
        // bricks contribute NOTHING (no seams, no stale data). One
        // page-table lookup + one trilinear 3D fetch per segment.
        if (frame.worldFieldCount > 0u)
            sigma += SampleWorldField(frame, WORLD_FIELD_ID_FOG, wpos, 0u).r;

        // ---- Ambient: EXACT per-segment volume weights from the
        // ray-space table (FMA-only ramps — no plane loads, no dot3s
        // in the march), composited with means held on the cadence
        // below. The volume stack is linear in the means, so holding
        // the (low-frequency) means while evaluating the (exact)
        // weights per segment is both cheaper and more faithful than
        // holding the whole composite.
        float3 volAcc = float3(0.0, 0.0, 0.0);
        float volTrans = 1.0;
        if (fogGiOn)
        {
            SgGiVolumeRayComposite(frame, rayPrep, t, wpos, volAcc, volTrans);
            if (t - giT >= kVolGiRefreshDist)
            {
                giT = t;
                // ---- Fog GI base: the A/B source toggle. The world-field
                // path (default) samples the baked BC6H mean via the
                // paired fog-GI sampler — ONE page-table lookup + ONE
                // trilinear 3D fetch per refresh, no octree walk, no
                // 9-lobe fetch. The octree path (the legacy SgGiSampleMean
                // walk over the dense BC6H level layers) stays as the A/B
                // reference. TOURMALINE_FOG_GI_SOURCE selects:
                //   worldfield (default) — the paired fog-GI field
                //   octree — the legacy SgGiSampleMean walk
                //   sky — the sky mean everywhere (the floor baseline)
                // Both paths preserve the covered/uncovered contract: an
                // uncovered position keeps the sky mean (the world
                // field's coverage channel distinguishes genuine darkness
                // from missing probes — see SampleFogGi).
                if (volTrans > 0.0) {
                    if (vp.fogGiSource == 1u) {
                        // Legacy: the octree walk (the A/B reference).
                        float3 octMean = float3(0.0, 0.0, 0.0);
                        const bool octreeCovered = (giOn > 0.5)
                            && SgGiSampleMean(frame, wpos, octMean);
                        baseHeld = octreeCovered ? octMean : skyMean;
                    }
                    else if (vp.fogGiSource == 2u) {
                        // Sky-only (the floor baseline for perf A/B).
                        baseHeld = skyMean;
                    }
                    else {
                        // Default: the world-field fog-GI path.
                        const FogGiSample gi =
                            SampleFogGi(frame, wpos, 0u);
                        if (gi.resident && gi.coverage > 0.02)
                            baseHeld = gi.radiance;
                        else
                            baseHeld = skyMean;
                    }
                }
                // volTrans <= 0: a weight-1 GI volume owns the base —
                // the octree/field walk is dead work; keep baseHeld.
            }
            ambient = (volAcc + baseHeld * volTrans) * giStrength;
        }

        // ---- Forward+ light in-scatter: INDEPENDENT of the GI block
        // above (the review's finding #3 — the old nest under
        // if (fogGiOn) { ... } meant GI-off fog never evaluated local
        // lights even with fpLightCount > 0). Own cadence at
        // kVolLightRefreshDist (finding #1 — the 0.75 m GI cadence meant
        // ~15 tile-list walks per pixel; 3 m is the review-suggested
        // 2-4 m band). The lights sit at fixed world positions; the
        // in-scatter field is smooth at the segment scale.
        if (fogLightsOn && t - lightT >= kVolLightRefreshDist) {
            lightT = t;
            lightsHeld = VolForwardPlusLights(frame, wpos, rayDir,
                (uint2)IN.pos.xy);
        }

        float3 source = ambient + lightsHeld;
        if (sunOn > 0.5)
            source += sunHdr * (phase * max(vp.sunScale, 0.0f));
        // Quarantine: a non-finite baked probe amplitude (NaN/Inf) must cost
        // one segment's scatter, never a poisoned pixel (NaN out stores NaN
        // -> downstream sanitize -> black; Inf -> white block).
        source = SanitizeHdr(source);

        // ---- Exact segment integration: sigma and source are held
        // constant over the segment by construction (sampled once), so
        // the in-scatter integral is analytic —
        //   ∫ throughput * sigmaS * source * exp(-sigma*(s-s0)) ds
        //     = throughput * intensity * source * (1 - exp(-tau))
        // instead of the Euler throughput * sigmaS * source * segW the
        // old body paid. Equal for small tau, but the exact form stays
        // well-behaved on coarse/dense segments (tau vs 1-e^-tau),
        // which is what makes longer steps survivable.
        const float trans = exp(-sigma * segW);
        const float scatterWeight = throughput * intensity_ * (1.0 - trans);
        scatter += SanitizeHdr(source * scatterWeight);
        throughput *= trans;

        if (throughput < 3.333333e-4f)
        {
            throughput = 0.0;
            break;
        }
    }

    float3 outColor = base * throughput + scatter;
    OUT.outColor = float4(SanitizeHdr(outColor), 1.0f);
    return OUT;
}
