// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Shared skydome IBL + fog evaluation. Included after common.hlsli so
// FrameUniforms / EnvironmentFrameData are visible. All colors are linear;
// intensities convert the 0..1 UI colors to pre-exposure HDR radiance.

// Sky gradient: horizon at y=0, zenith at y=1, ground below horizon.
// exponent shapes the horizon->zenith falloff.
float3 EvaluateSkyGradient(float3 dir, EnvironmentFrameData env)
{
    float exponent = max(env.skyHorizon.w, 0.01f);
    float skyIntensity = max(env.skyZenith.w, 0.0f);
    float3 zenith = env.skyZenith.rgb * skyIntensity;
    float3 horizon = env.skyHorizon.rgb * skyIntensity;
    float3 groundCol = env.ground.rgb * skyIntensity;
    float t = saturate(dir.y);
    float3 sky = lerp(horizon, zenith, pow(t, exponent));
    if (dir.y < 0.0f)
        sky = lerp(horizon, groundCol, saturate(-dir.y * 3.0f));
    return sky;
}

// Full sky radiance including the sun disc. Returns HDR (pre-exposure).
float3 EvaluateSkyColor(float3 dir, EnvironmentFrameData env)
{
    float skyMode = env.fogEndAndFlags.y;
    if (skyMode < 0.5f)
        return float3(0.0f, 0.0f, 0.0f);
    float3 sky = EvaluateSkyGradient(dir, env);
    float discCos = env.sunDirection.w;
    if (discCos > -0.5f)
    {
        float cosA = dot(normalize(dir), normalize(env.sunDirection.xyz));
        if (cosA > discCos)
        {
            // Solid disc at sun HDR intensity so bloom picks it up.
            float3 sunHdr = env.sunColor.rgb * max(env.sunColor.w, 0.0f);
            // Small smooth rim to avoid a hard 1-texel stair.
            float rim = smoothstep(discCos, min(discCos + 0.0008f, 1.0f), cosA);
            sky = lerp(sky, sunHdr, rim);
        }
    }
    return sky;
}

// Narrow-BDA variants for the fullscreen sky pass (see sky.frag.hlsl).
// The by-value overloads above copy the full 128-byte EnvironmentFrameData
// (and callers typically materialize the 1360+ byte FrameUniforms to get
// it); the sky pixel shader only needs the sky/sun fields, so these pull
// each float4 out of the frame pointer with scalar BDA reads instead —
// the same discipline as the shadow path's
// pc.frameUniforms.Get().view + cascade-buffer-pointer contract
// (shadows.hlsli GetShadowFactorCascadedFromPC). dir must be normalized;
// sunDirection.xyz is documented normalized (common.hlsli), so only it
// keeps a defensive normalize — the per-pixel normalize(dir) is dropped.
float3 EvaluateSkyGradientFromFrame(float3 dir, vk::BufferPointer<FrameUniforms> framePtr)
{
    float4 skyZenith = framePtr.Get().env.skyZenith;
    float4 skyHorizon = framePtr.Get().env.skyHorizon;
    float4 ground = framePtr.Get().env.ground;
    float exponent = max(skyHorizon.w, 0.01f);
    float skyIntensity = max(skyZenith.w, 0.0f);
    float3 zenith = skyZenith.rgb * skyIntensity;
    float3 horizon = skyHorizon.rgb * skyIntensity;
    float3 groundCol = ground.rgb * skyIntensity;
    float t = saturate(dir.y);
    float3 sky = lerp(horizon, zenith, pow(t, exponent));
    if (dir.y < 0.0f)
        sky = lerp(horizon, groundCol, saturate(-dir.y * 3.0f));
    return sky;
}

float3 EvaluateSkyColorFromFrame(float3 dir, vk::BufferPointer<FrameUniforms> framePtr)
{
    float skyMode = framePtr.Get().env.fogEndAndFlags.y;
    if (skyMode < 0.5f)
        return float3(0.0f, 0.0f, 0.0f);
    float3 sky = EvaluateSkyGradientFromFrame(dir, framePtr);
    float4 sunDirection = framePtr.Get().env.sunDirection;
    float discCos = sunDirection.w;
    if (discCos > -0.5f)
    {
        float cosA = dot(dir, normalize(sunDirection.xyz));
        if (cosA > discCos)
        {
            // Solid disc at sun HDR intensity so bloom picks it up.
            float4 sunColor = framePtr.Get().env.sunColor;
            float3 sunHdr = sunColor.rgb * max(sunColor.w, 0.0f);
            // Small smooth rim to avoid a hard 1-texel stair.
            float rim = smoothstep(discCos, min(discCos + 0.0008f, 1.0f), cosA);
            sky = lerp(sky, sunHdr, rim);
        }
    }
    return sky;
}

// Diffuse irradiance for N (hemispheric blend of sky + ground).
// Scaled by iblIntensity (env.ground.w); returns HDR.
float3 EvaluateIrradiance(float3 N, EnvironmentFrameData env)
{
    float ibl = max(env.ground.w, 0.0f) * env.fogEndAndFlags.w;
    if (ibl <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    float exponent = max(env.skyHorizon.w, 0.01f);
    float up = saturate(N.y);
    float dn = saturate(-N.y);
    float3 zenith = env.skyZenith.rgb;
    float3 horizon = env.skyHorizon.rgb;
    float3 groundCol = env.ground.rgb;
    float3 skyPart = lerp(horizon, zenith, pow(up, exponent));
    float3 gndPart = lerp(horizon, groundCol, saturate(dn * 1.5f));
    float3 hemi = lerp(gndPart, skyPart, saturate(N.y * 0.5f + 0.5f));
    return hemi * ibl;
}

// Box parallax correction for a probe capture (classic box projection):
// the capture is treated as if it lived on the faces of the probe's
// DEPTH-FITTED AABB — the baker analyzes each cube face's primary-ray
// depths and stores the six plane distances (one wall scalar per plane,
// manifest "box" group; forwardplus.h packs them into the probe light
// entry) — so a reflection off a surface AWAY from the capture point
// samples the capture in the direction the surface would actually see.
// Without it every pixel samples along the raw reflect vector R, as if
// the capture surrounded the PIXEL — interior surfaces then read dark
// capture regions (floor/ceiling texels) instead of the bright geometry
// across the room (the Sponza-atrium gold-cloth streaks).
//
// The box is asymmetric by design (walls sit at different distances per
// plane); probeCenter is the CAPTURE point and may sit off the box
// midpoint. Callers without a fitted box pass the legacy symmetric
// center +- influence cube. Every full-weight pixel (dist <= influence)
// lies inside the influence volume, and fade-ring pixels use the same
// far-plane exit projection — continuous across the box faces by
// construction, additionally faded by the probe weight at the call
// site. Returns R unchanged when the box is degenerate or the ray
// misses (keeps the helper total).
float3 BoxParallaxCorrect(float3 worldPos, float3 R, float3 boxMin, float3 boxMax, float3 probeCenter)
{
    if (!all(boxMax - boxMin > float3(1e-3f, 1e-3f, 1e-3f)))
        return R;
    // Safe per-axis reciprocal (R is normalized but can sit ~0 on an axis;
    // IEEE inf would still work in the slab test, but the clamp keeps it
    // total on drivers with imprecise inf handling).
    float3 safeR = R;
    safeR.x = abs(safeR.x) < 1e-6f ? (safeR.x < 0.0f ? -1e-6f : 1e-6f) : safeR.x;
    safeR.y = abs(safeR.y) < 1e-6f ? (safeR.y < 0.0f ? -1e-6f : 1e-6f) : safeR.y;
    safeR.z = abs(safeR.z) < 1e-6f ? (safeR.z < 0.0f ? -1e-6f : 1e-6f) : safeR.z;
    float3 invR = 1.0f / safeR;
    float3 tMin = (boxMin - worldPos) * invR;
    float3 tMax = (boxMax - worldPos) * invR;
    float3 tNear3 = min(tMin, tMax);
    float3 tFar3 = max(tMin, tMax);
    float tNear = max(max(tNear3.x, tNear3.y), tNear3.z);
    float tFar = min(min(tFar3.x, tFar3.y), tFar3.z);
    if (tFar < max(tNear, 0.0f))
        return R;
    float3 hit = worldPos + R * tFar;
    float3 corr = hit - probeCenter;
    // Degenerate (pixel at the probe center looking outward: hit == center
    // only when tFar == 0 with worldPos == center — length ~0): keep R.
    if (dot(corr, corr) < 1e-12f)
        return R;
    return normalize(corr);
}

// Corrected sample direction for one probe, faded by its weight (full
// inside the influence, raw R at the fade edge) so probe handoffs stay
// pop-free.
float3 ProbeSampleDir(float3 worldPos, float3 R, float3 center,
    float3 boxMin, float3 boxMax, float weight)
{
    if (weight <= 1e-4f)
        return R;
    float3 Rcorr = BoxParallaxCorrect(worldPos, R, boxMin, boxMax, center);
    return normalize(lerp(R, Rcorr, clamp(weight, 0.0f, 1.0f)));
}

// Cheap specular env: horizon<->zenith by reflection Y, dielectrics get
// F0-weighted reflections, rougher surfaces fade out.
// Probe selection walks the caller's Forward+ tile list and filters
// type-2 (kForwardPlusProbe) light entries packed as position = center,
// range = influence + fade, direction = (fade, cubemapIndex, mipLevels) —
// the same tile walk the direct-light loop does, so specular rides the
// light bucketing with no second list set. The FP inputs arrive as plain
// parameters (NOT via pc: this header is also included by pass TUs like
// volumetric/sky that have no push constants). tileIdx is the caller's
// 16px tile (0xffffffffu = no tile); probeTileFresh (FrameUniforms,
// app-gated on the cull dispatch) selects fresh tile lists vs the global
// cubemap fallback — there is no probe buffer left to scan, so a stale
// tile path would just re-walk garbage.
// The baked chains are GGX-preconvolved (roughness = mip/(mips-1)), so
// sampling selects the LOD from the material roughness (+ the UI LOD
// bias); the clamp sampler avoids cube-seam wrap bleeding.
// Blend selection written by the probe pre-mix weights pass (must mirror
// smvk::probepremix::BlendSelectionGpu, probepremix.h + the weights/mix
// shaders). GPU-owned: the material reads the DOMINANT probe's packed
// parallax box straight from it via the FrameUniforms BDA - no CPU
// readback, no staging, no classification anywhere.
struct ProbePremixSelection
{
    uint count;          // 0
    uint indices[4];     // 4  cubemap bindless indices
    float weights[4];     // 20 normalized (sum = 1 over the set)
    float pos[3];          // 36 dominant probe's capture position
    float boxMinus[3];    // 48 dominant minus-plane distances
    float boxPlus[3];     // 60 dominant plus-plane distances
    uint pad0;            // 72
    uint pad1;            // 76
};

// Rough-specular fade, metal-aware. The dielectric fade (gloss^2 with a
// 0.1 floor) exists because a rough F0=0.04 lobe reads as surface noise,
// not light - cheap and right for dielectrics. Metals must NOT ride it:
// the GGX-preconvolved mip chain already blurred their lobe (energy kept
// per mip, roughness = mip/(mips-1)), and the tinted reflection IS the
// surface's entire ambient light (kD = 1 - metallic -> 0: no diffuse, no
// GI ambient term). The old uniform fade kept a rough-0.5 gold brocade
// (the sponza banners: metallic ~1, roughness ~0.5) at 32% of its ONLY
// ambient light - "dark gold even when getting proper contribution from
// cubemaps" (the probe content was verified proper; the fade crushed it
// on the way out). Pure metals keep full energy, blended scale between.
float EnvSpecFade(float gloss, float metallic)
{
    const float dielectricFade = gloss * gloss * 0.9f + 0.1f;
    return lerp(dielectricFade, 1.0f, saturate(metallic));
}

float3 EvaluateSpecularEnv(float3 R, float3 F, float roughness, EnvironmentFrameData env,
    float3 worldPos, FrameUniforms frame, uint tileIdx,
    uint fpLightCount, uint fpMaxPerTile,
    vk::BufferPointer<ForwardPlusTileCountBuffer> fpTileCounts,
    vk::BufferPointer<ForwardPlusTileIndexBuffer> fpTileIndices,
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights, float metallic)
{
    // Intensity split: iblIntensity (ground.w) is the ANALYTIC fallback's
    // intensity only. Baked samples are TRUE radiance (physical units,
    // matching the direct sun view of the same surface) scaled by the
    // cubemap panel's own intensity — applying ibl to baked content
    // crushed sun-lit reflections by skyIntensity/ibl (~30x here: a
    // mirror showed the sun-lit wall far darker than looking at it).
    // Also deliberately NOT the fogEndAndFlags.w diffuse-isolation flag
    // (ambient modes): smooth surfaces cannot do mirrors from the broad
    // baked lobes, so they keep env reflections in every mode. The
    // cubemap toggle rides useBaked below (off = analytic sky, never
    // black).
    float ibl = max(env.ground.w, 0.0f);
    // Baked-cubemap kill switch (UI): off falls back to the analytic sky,
    // never to black — probe-covered pixels must not go dark just because
    // the bake is disabled.
    bool useBaked = frame.cubemapIntensity > 0.0f;
    uint cubeDbg = frame.cubemapDebug;

    // ---- GPU probe pre-mix (docs/probe_premix_plan.md; the Remember Me
    // architecture, GPU-only selection): the weights pass already
    // selected the top-K captures around the point of interest (the
    // camera) off the Forward+ light-buffer probe tail - the GPU system
    // we already have, NO CPU classification - and the mix pass blended
    // them into ONE cubemap (mip-to-mip). This branch is a SINGLE
    // cubemap fetch of the blend with the DOMINANT probe's box parallax
    // (the selection's slot-0 winner, read straight from the GPU-owned
    // selection buffer via the FrameUniforms BDA): no per-pixel probe
    // set, no tile-slot walk, no candidate blending - nothing can
    // switch per pixel, which is the point (the blockiness class came
    // from per-pixel set switching). Zero-candidate selection (or the
    // pin off: index = 0xffffffff) falls through to the per-pixel walk
    // unchanged - the zero-regression contract.
    if (useBaked && frame.probePremixIndex != 0xffffffffu) {
        const ProbePremixSelection sel =
            vk::BufferPointer<ProbePremixSelection>(frame.probePremixSel).Get();
        if (sel.count > 0u) {
            TextureCube blend =
                cubemaps[NonUniformResourceIndex(frame.probePremixIndex)];
            const SamplerState cubeSampler =
                samplers[NonUniformResourceIndex(frame.cubemapSampler)];
            // Roughness LOD against the blended chain (mip-to-mip mix
            // preserves the baked prefilter), the same clamp the walk
            // uses (the blended chain depth rides probePremixMips).
            const float lod = clamp(
                roughness * (float(frame.probePremixMips) - 1.0f)
                    + frame.cubemapLodBias,
                0.0f, float(frame.probePremixMips) - 1.0f);
            // Dominant box parallax: unpack the packed plane distances
            // (pos - minus, pos + plus - the same packing the walk reads
            // off the light tail) and re-aim R at the fitted box, faded
            // to full weight inside (the blend represents the whole
            // neighborhood - the parallax is the dominant probe's).
            const float3 c = float3(sel.pos[0], sel.pos[1], sel.pos[2]);
            const float3 bMin = c - float3(sel.boxMinus[0], sel.boxMinus[1], sel.boxMinus[2]);
            const float3 bMax = c + float3(sel.boxPlus[0], sel.boxPlus[1], sel.boxPlus[2]);
            const float3 Rs = ProbeSampleDir(worldPos, R, c, bMin, bMax, 1.0f);
            float3 envCol2 = blend.SampleLevel(cubeSampler, Rs, lod).rgb
                * max(frame.cubemapIntensity, 0.0f);
            if (cubeDbg == 1u) return envCol2; // raw blend (no Fresnel)
            if (cubeDbg == 2u) return float3(1.0f, 0.25f, 0.0f); // pre-mix heat
            const float gloss = 1.0f - clamp(roughness, 0.0f, 1.0f);
            return envCol2 * F * EnvSpecFade(gloss, metallic);
        }
        // Selection empty this frame (the weights pass found no
        // candidates): fall through to the per-pixel walk below - it
        // owns the same fallbacks (global cubemap, then analytic sky).
    }

    // Walk-scope sampler + LOD bias (the pre-mix block above declares
    // its own local copies inside its branch scope).
    const SamplerState cubeSampler =
        samplers[NonUniformResourceIndex(frame.cubemapSampler)];
    const float lodBias = frame.cubemapLodBias;

    float3 envCol = float3(0.0f, 0.0f, 0.0f);
    bool foundProbe = false;
    float bestWeight = 0.0f; // debug views (probe-weight heat) only

    // Tile-bucketed probe scan. The cull's outer-sphere test is
    // conservative (influence + fade vs tile frustum + depth range), so a
    // tile miss implies a global miss: when the lists are fresh and the
    // tile holds no contributing probe, the pixel takes the analytic sky
    // (then the global cubemap below) with no fallback scan.
    // CUBIC FADE x INFLUENCE-WIDTH BLEND: every candidate probe in the
    // tile list (at most MAX_FP_PROBE_TILE_SLOTS = 8) contributes
    // (w^3 * influence^2) * sample, normalized by the summed blend
    // weight: envCol = sum(wEff * S_i) / sum(wEff),
    // wEff = fadeWeight^3 * influenceRadius^2.
    //  - CONTINUITY: each probe's contribution rises/falls with its own
    //    fade weight (0 at the influence+fade boundary) - there is no
    //    discrete candidate set, so no swap steps anywhere
    //    (winner-takes-all flipped whole captures at boundaries; the
    //    top-2 blend still stepped hard swapping the second candidate
    //    between two weight-1.0 captures in dense pockets - visible as
    //    tiles).
    //  - CONTENT: the influence^2 breaks the weight-1.0 ties of the
    //    dense overlapping pockets toward the WIDEST capture - the
    //    room-average reflection (the user-validated top-2 look, whose
    //    tie-break was widest-first upload order). Tight pockets near
    //    occluders (a banner filling a capture face at close range)
    //    lose ~3.7:1 to the wide probe's cleaner room reflection. The
    //    w^3 keeps far fade-ring captures from tinting the result.
    //    (A nearest/distance tie-break was tried and rejected: the
    //    nearest captures in dense pockets are exactly the
    //    occluder-adjacent tight ones - the red banner reflection
    //    got STRONGER and more blocky.)
    //  - TILE-SLOT overflow (a 9th probe dropped in one tile but kept
    //    in the neighbor) perturbs only its own small blend share - the
    //    seam drops below visibility instead of changing the pair set.
    // Cost ceiling: one cubemap fetch per tile-slot candidate (8).
    // Index space: lights [0, fpLightCount), probes [fpLightCount,
    // fpLightCount + frame.probeEntries). The bound MUST include the
    // probe tail — a lights-only bound skips every probe entry and the
    // walk silently degrades to the global fallback cubemap. Scenes with
    // zero lights (sponza) still walk: entryTotal > 0 via the tail.
    float3 probeSum = float3(0.0f, 0.0f, 0.0f);
    float probeWeightSum = 0.0f;
    const uint entryTotal = fpLightCount + frame.probeEntries;
    bool tileFresh = useBaked && tileIdx != 0xffffffffu && tileIdx < (uint)MAX_FP_TILES
        && frame.probeTileFresh != 0u && entryTotal > 0u;
    if (tileFresh) {
        // Shared effective capacity (common.hlsli FpTileLightSlots): the
        // cull producer derives its row stride from the SAME clamp chain,
        // so both sides address the same rows whatever the configured
        // maxLightsPerTile. The old consumer-only clamp against
        // MAX_FP_PER_TILE (128) read wrong rows the moment the desc
        // exceeded the producer's sort capacity (32).
        uint perTile = FpTileLightSlots(fpMaxPerTile);
        uint stride = perTile + (uint)MAX_FP_PROBE_TILE_SLOTS;
        // Probes pack the dedicated tail slots (immune to light saturation
        // — a packed light budget must never flip specular to the global
        // fallback). counts packs the TRUE counts: lights low 16, probes
        // high 16.
        uint listCount = min(fpTileCounts.Get().counts[tileIdx] >> 16,
            (uint)MAX_FP_PROBE_TILE_SLOTS);
        [loop]
        for (uint s = 0; s < listCount; ++s) {
            uint li = fpTileIndices.Get().indices[tileIdx * stride + perTile + s];
            if (li >= entryTotal)
                continue;
            ForwardPlusLight L = fpLights.Get().lights[li];
            if (L.type != kForwardPlusProbe)
                continue; // direct light: the light loop owns it
            uint cubemapIdx = (uint)L.direction.y;
            if (cubemapIdx == 0xffffffffu)
                continue;
            float influenceRadius = max(L.range - L.direction.x, 0.0f);
            float fadeRadius = max(L.direction.x, 0.0f);
            float dist = length(worldPos - L.position);
            float weight = 0.0f;
            if (dist <= influenceRadius) {
                weight = 1.0f;
            } else if (fadeRadius > 0.0f && dist <= influenceRadius + fadeRadius) {
                weight = 1.0f - (dist - influenceRadius) / fadeRadius;
            }
            if (weight <= 0.0f)
                continue;
            // Depth-fitted parallax box: the baker's per-plane wall
            // scalars, packed as color.rgb = minus-plane distances and
            // (intensity, innerAngle, outerAngle) = plus-plane distances
            // (see forwardplus.h). All-zero = legacy row without a fit:
            // fall back to the symmetric influence cube.
            float3 dNeg = L.color.rgb;
            float3 dPos = float3(L.intensity, L.innerAngle, L.outerAngle);
            float3 pMin = L.position - dNeg;
            float3 pMax = L.position + dPos;
            if (dot(dNeg, dNeg) + dot(dPos, dPos) <= 1e-10f) {
                const float h = max(influenceRadius, 0.05f);
                pMin = L.position - h;
                pMax = L.position + h;
            }
            const float mips = max(L.direction.z, 1.0f);
            const float lod = clamp(roughness * (mips - 1.0f) + lodBias,
                0.0f, mips - 1.0f);
            TextureCube cube = cubemaps[NonUniformResourceIndex(cubemapIdx)];
            // Box parallax per candidate: re-aim R at the probe's fitted
            // box, faded by its own weight (see ProbeSampleDir).
            float3 Rs = ProbeSampleDir(worldPos, R, L.position,
                pMin, pMax, weight);
            // Blend weight: fade weight^3 (the influence/fade ring) over
            // the INFLUENCE RADIUS SQUARED. The radius^2 breaks the
            // weight-1.0 TIES among the dense overlapping pockets toward
            // the WIDEST capture - the room-average look (the old top-2
            // tie-break, widest-first upload order, the user-validated
            // look). The tight pockets near occluders (a capture with a
            // banner filling a face at close range, e.g.) are exactly the
            // ones the tight fit produces - the wide probe's cleaner
            // room reflection outweighs them ~ (5/2.6)^2 = 3.7:1, without
            // losing continuity: every term varies smoothly (fade^3 hits
            // 0 at the coverage boundary; the radius is per-probe
            // constant), so no discrete candidate set exists anywhere.
            const float w3 = weight * weight * weight;
            const float wideW = w3 * influenceRadius * influenceRadius;
            probeSum += wideW * cube.SampleLevel(cubeSampler, Rs, lod).rgb;
            probeWeightSum += wideW;
            if (weight > bestWeight)
                bestWeight = weight;
        }
        if (probeWeightSum > 0.0f) {
            envCol = probeSum / probeWeightSum;
            foundProbe = true;
        }
    }

    if (!foundProbe && useBaked && frame.cubemapIndex != 0xffffffffu) {
        // Fallback to global cubemap. Roughness-aware LOD like the tile
        // path (the old sharp-only fetch sampled a single dark texel on
        // mid-rough metals instead of the blurred mean — streaky). The
        // mip count is queried from the texture itself (bakes always ship
        // full chains); clamped defensively for procedural fallbacks.
        TextureCube cubemap = cubemaps[NonUniformResourceIndex(frame.cubemapIndex)];
        uint fbW = 0u, fbH = 0u, fbLevels = 1u;
        cubemap.GetDimensions(0, fbW, fbH, fbLevels);
        float fbMips = max((float)fbLevels, 1.0f);
        float fbLod = clamp(roughness * (fbMips - 1.0f) + lodBias, 0.0f, fbMips - 1.0f);
        envCol = cubemap.SampleLevel(cubeSampler, R, fbLod).rgb;
        foundProbe = true;
        bestWeight = 1.0f;
    }

    if (!foundProbe) {
        // Fallback to analytic sky. ibl gates THIS path only (the baked
        // paths above carry their own intensity).
        if (ibl <= 0.0f)
            return float3(0.0f, 0.0f, 0.0f);
        float3 zenith = env.skyZenith.rgb;
        float3 horizon = env.skyHorizon.rgb;
        float3 groundCol = env.ground.rgb;
        float3 skyPart = lerp(horizon, zenith, pow(saturate(R.y), 1.5f));
        float3 gndPart = lerp(horizon, groundCol, saturate(-R.y * 2.0f));
        envCol = R.y >= 0.0f ? skyPart : gndPart;
    } else {
        // Baked cubemap: TRUE radiance x the cubemap panel intensity. The
        // capture stores physical units (no /skyIntensity at bake), so a
        // mirror reflecting a sun-lit wall matches looking at that wall
        // directly — the IBL slider never touches baked content.
        envCol *= max(frame.cubemapIntensity, 0.0f);
    }

    // Debug views (UI): raw cubemap without Fresnel/roughness weighting,
    // or the probe blend weight as heat.
    if (cubeDbg == 1u && foundProbe) {
        return envCol;
    }
    if (cubeDbg == 2u) {
        float w = foundProbe ? bestWeight : 0.0f;
        return float3(w, 1.0f - abs(w - 0.5f) * 2.0f * 0.5f, 1.0f - w);
    }

    float gloss = 1.0f - clamp(roughness, 0.0f, 1.0f);
    if (foundProbe)
        return envCol * F * EnvSpecFade(gloss, metallic);
    return envCol * ibl * F * EnvSpecFade(gloss, metallic);
}

// Specular-visibility proxy from the baked GI (Lagarde-style occlusion):
// the pixel's baked irradiance vs the UNOCCCLUDED sun+sky reference in
// the same transport units approximates how much environment the surface
// can actually see. Without it, shaded metals under roofs mirror the full
// sunlit courtyard from the probe capture ("crazy luminous" gold trim) —
// geometry they cannot see through. giScale normalizes the UI GI-intensity
// multiplier back out of the baked term. Surfaces with no baked source
// keep 1 (callers decide).
//
// The ratio's ZERO is a fit artifact, not enclosure: the 9-lobe
// white-world SG fit at a brick under-represents the horizontal band a
// vertical open-air surface sees (the sponza banner gold — metallic ~1,
// rough ~0.5, hanging in the open nave, probes covering it with proper
// bright captures — evaluates e(N) ~ 0 against a sky-dominated refL),
// and envVis 0 zeroes the cubemap env spec, a metal's ONLY ambient
// light (kD = 1 - metallic -> 0): "dark gold even when getting proper
// contribution from cubemaps". Floor the factor, TAPERED BY ROUGHNESS:
// a broad lobe smears across occlusion gaps (the hemispherical ratio
// overestimates enclosure for it — Lagarde's exponent makes the same
// argument from the other side), while a sharp lobe needs real
// visibility (the under-roof "crazy luminous" gold trim stays
// data-driven at smooth — the floor dies below rough 0, exactly where
// the mirror-flash artifact lived). Linear taper to 0.35 at rough 0.5.
float SpecularOcclusionFromGI(float3 bakedE, float giScale, float3 N,
    EnvironmentFrameData env, float roughness)
{
    const float3 kLum = float3(0.2126, 0.7152, 0.0722);
    const float3 e = bakedE * (1.0 / max(giScale, 1e-4));
    // Unoccluded reference: direct sun (when N faces it) + sky hemisphere,
    // rebuilt in the bake's transport units from the env block
    // (sunIntensity rides sunColor.w, skyIntensity skyZenith.w).
    float3 sunDir = normalize(env.sunDirection.xyz + 1e-9);
    float ndl = max(dot(N, sunDir), 0.0);
    float3 sunE = env.sunColor.rgb * max(env.sunColor.w, 0.0) * ndl;
    float3 skyMean = (env.skyZenith.rgb + env.skyHorizon.rgb + env.ground.rgb) / 3.0;
    float3 skyE = skyMean * max(env.skyZenith.w, 0.0) * 0.5;
    const float refL = max(dot(sunE + skyE, kLum), 1e-3);
    const float occ = saturate(dot(e, kLum) / refL);
    const float roughFloor = saturate(roughness / 0.5f) * 0.35f;
    return saturate(max(occ, roughFloor));
}

// Distance fog factor in 0..1 from view depth (linear eye depth, metres),
// world Y, and the packed env block. fogMode: 0 off, 1 exp2, 2 linear.
// Height falloff (fogDensityHeight.z > 0) fades fog above fogHeight.
float ComputeFogFactor(float viewDepth, float worldY, EnvironmentFrameData env)
{
    float fogMode = env.fogEndAndFlags.z;
    if (fogMode < 0.5f)
        return 0.0f;
    float f = 0.0f;
    if (fogMode < 1.5f)
    {
        float d = max(env.fogDensityHeight.x, 0.0f);
        float x = d * max(viewDepth, 0.0f);
        f = 1.0f - exp(-x * x);
    }
    else
    {
        float s = env.fogDensityHeight.w;
        float e = max(env.fogEndAndFlags.x, s + 1e-4f);
        f = saturate((max(viewDepth, 0.0f) - s) / (e - s));
    }
    float falloff = env.fogDensityHeight.z;
    if (falloff > 0.0f)
    {
        float h = env.fogDensityHeight.y;
        float hf = exp(-max(worldY - h, 0.0f) * falloff);
        f *= hf;
    }
    return saturate(f);
}

// ---------------------------------------------------------------------------
// World env tiles (Engine/WorldPartition/WorldEnvTiles.h — the runtime
// rework of the old OBB-volume zone system): the zone tables BAKE onto
// the 64 m world-tile grid (a dirty-hash rebuild at load/edit — NOT a
// per-frame pack) and this resolver walks a TILE ADDRESS instead of
// intersecting OBBs per pixel:
//   cell = floor((p.xz - gridMin) / 64) -> EnvTileCell
//   -> (lower/mid/upper record — the mid band or the splitY half-spaces,
//       weight) -> records[rec]
// records[0] is ALWAYS the base (the default environment block — the old
// zone-zero semantic: outside every zone, and out-of-grid, the base look
// applies at weight 1). The sun stays global. All helpers fall back to
// the plain env path when envZoneCount is 0. FIDELITY: the winner
// selection + weights are the retired walk's math evaluated at the
// tile's sample positions — fade bands quantize to 64 m tile granularity
// (a zone coarser than a tile is faithful; sub-tile detail is not
// representable by design).
// ---------------------------------------------------------------------------

struct EnvZoneLook
{
    float3 skyTint;
    float ambientI;
    float3 fogColor;
    float fogDensity;
    float volumetrics;        // ratio over the pass base (0 = off, 1 = base)
    float volumetricDensity;  // absolute extinction (1/m)
};

// Unpacks one tile record into the blendable look (records[0] = base).
void EnvTileUnpack(EnvTileRecord r, out EnvZoneLook z)
{
    z.skyTint = r.sky.rgb;
    z.ambientI = r.sky.a;
    z.fogColor = r.fog.rgb;
    z.fogDensity = r.fog.a;
    z.volumetrics = r.vol.x;
    z.volumetricDensity = r.vol.y;
}

// Resolves the zone at p by TILE ADDRESS (the retired per-pixel OBB walk's
// replacement). weight = the baked blendWeight*falloff; the look is the
// tile's record. Returns false when no tiles are live (the caller keeps
// the plain look).
bool EnvZoneAt(float3 p, FrameUniforms frame, out float weight,
    out EnvZoneLook z)
{
    weight = 0.0;
    z.skyTint = float3(1.0, 1.0, 1.0);
    z.ambientI = 0.0;
    z.fogColor = float3(0.0, 0.0, 0.0);
    z.fogDensity = 0.0;
    z.volumetrics = 1.0;
    z.volumetricDensity = 0.0;
    const uint n = frame.envZoneCount;
    if (n == 0u || (frame.envZoneFlags & 1u) == 0u)
        return false;

    // Out of grid (or a degenerate grid): the BASE record at weight 1 —
    // the zone-zero semantic; the grid spans the scene bounds + margin,
    // beyond it the default environment applies.
    const float2 rel = (p.xz - float2(frame.envTileMinX, frame.envTileMinZ))
        / 64.0;
    const int2 cellI = int2(floor(rel));
    if (any(cellI < int2(0, 0)) || any(cellI >= int2(frame.envTileDimX, frame.envTileDimZ)))
    {
        const EnvTileRecord base =
            vk::BufferPointer<EnvTileRecord>(frame.envZones).Get();
        EnvTileUnpack(base, z);
        weight = 1.0;
        return true;
    }

    // The cell: up to three records — the mid band (the enclosed room)
    // first, else the splitY half-spaces.
    const EnvTileCell c = vk::BufferPointer<EnvTileCell>(
        frame.envTileCells
        + uint64_t(uint(cellI.y) * frame.envTileDimX + uint(cellI.x)) * 32u
    ).Get();
    uint rec; float w;
    if (c.midRecord != 0xFFFFFFFFu
        && p.y >= c.splitY && p.y <= c.midHiY) {
        rec = c.midRecord;
        w = c.midWeight;
    } else if (p.y > c.splitY) {
        rec = c.upperRecord;
        w = c.upperWeight;
    } else {
        rec = c.lowerRecord;
        w = c.lowerWeight;
    }
    if (rec >= n)
        return false; // stale/corrupt index: the plain path, never garbage
    const EnvTileRecord r = vk::BufferPointer<EnvTileRecord>(
        frame.envZones + uint64_t(rec) * 48u).Get();
    EnvTileUnpack(r, z);
    weight = w;
    return w > 0.0;
}

// ---------------------------------------------------------------------------
// The per-segment zone resolver (the volumetric march's TILE CACHE).
// EnvZoneAt's full walk — cell fetch, vertical band pick, record fetch,
// unpack — memoized on the (XZ cell, record) pair: the CELL re-reads only
// when the march crosses a 64 m tile boundary, the RECORD only when the
// vertical band changes (y crossing splitY / midHiY) or the pick moves to
// a different record. A 64-step march through one room costs ONE cell
// fetch + ONE record fetch; the segments between reuse the resolve — the
// buffers are NOT re-walked at every step. (rec, weight) are paired per
// band per cell (the weight rides the same cell+band slot as the record),
// so one comparison gates both refetches.
//
// WHY PER SEGMENT (the review's finding): the old march resolved the zone
// ONCE at the camera ray's midpoint and used that single density for the
// whole sightline. A ray through two zones took the middle zone's
// density for the full length; a NEAR WALL changed the midpoint — a
// scene-depth-dependent zone pick that changed with the view without
// the camera moving. Per-segment resolution makes each segment's
// extinction its own zone's.
// ---------------------------------------------------------------------------
static const int2 kEnvZoneCellOutOfGrid = int2(0x7fffffff, 0x7fffffff);

struct EnvZoneMarchCache
{
    int2 cell;          // the cached XZ tile key (kEnvZoneCellOutOfGrid = out of grid)
    EnvTileCell c;      // the cell content (the vertical pick source)
    uint rec;           // the cached record pick (0xFFFFFFFF = none yet)
    float weight;       // the cached blend weight (paired with the pick)
    EnvZoneLook look;   // the cached unpacked record
    bool usable;        // the cached serve is valid (w > 0, rec < count)
    bool live;          // at least one resolve happened
};

// Resolves the zone at p through the cache; same semantics as EnvZoneAt
// (false = the plain path, weight 0 — the caller keeps the pass base).
bool EnvZoneSegment(float3 p, FrameUniforms frame,
    inout EnvZoneMarchCache cache, out float weight, out EnvZoneLook z)
{
    weight = 0.0;
    z.skyTint = float3(1.0, 1.0, 1.0);
    z.ambientI = 0.0;
    z.fogColor = float3(0.0, 0.0, 0.0);
    z.fogDensity = 0.0;
    z.volumetrics = 1.0;
    z.volumetricDensity = 0.0;
    const uint n = frame.envZoneCount;
    if (n == 0u || (frame.envZoneFlags & 1u) == 0u)
        return false; // the caller hoists this; the belt+braces re-check

    // The XZ tile key. Out of grid canonicalizes to ONE sentinel cell so
    // the base-record-at-weight-1 state (EnvZoneAt's zone-zero semantic)
    // caches like any other — the synthesized cell is a no-split
    // lower=upper=record-0 column and the pick below flows through the
    // common path with no special cases.
    const float2 rel = (p.xz - float2(frame.envTileMinX, frame.envTileMinZ))
        / 64.0;
    const int2 cellI = int2(floor(rel));
    int2 cellKey = cellI;
    if (any(cellI < int2(0, 0))
        || any(cellI >= int2(frame.envTileDimX, frame.envTileDimZ)))
        cellKey = kEnvZoneCellOutOfGrid;

    // CELL level: re-read the cell only at a tile-key change.
    if (!cache.live || all(cache.cell != cellKey))
    {
        if (all(cellKey == kEnvZoneCellOutOfGrid))
        {
            // Out of grid: the BASE record at weight 1 (the zone-zero
            // semantic — same as EnvZoneAt).
            cache.c.lowerRecord = 0u;
            cache.c.upperRecord = 0u;
            cache.c.splitY = -3.402823466e+38f;
            cache.c.lowerWeight = 1.0;
            cache.c.upperWeight = 1.0;
            cache.c.midRecord = 0xFFFFFFFFu;
            cache.c.midHiY = -3.402823466e+38f;
            cache.c.midWeight = 0.0;
        }
        else
        {
            cache.c = vk::BufferPointer<EnvTileCell>(
                frame.envTileCells
                + uint64_t(uint(cellI.y) * frame.envTileDimX
                    + uint(cellI.x)) * 32u
            ).Get();
        }
        cache.cell = cellKey;
        cache.live = true;
    }

    // The vertical band pick (cheap branches off the CACHED cell — y
    // crossing splitY/midHiY inside one tile changes the record; the
    // same pick order as EnvZoneAt: mid band first, then the half
    // spaces).
    uint rec; float w;
    if (cache.c.midRecord != 0xFFFFFFFFu
        && p.y >= cache.c.splitY && p.y <= cache.c.midHiY) {
        rec = cache.c.midRecord;
        w = cache.c.midWeight;
    } else if (p.y > cache.c.splitY) {
        rec = cache.c.upperRecord;
        w = cache.c.upperWeight;
    } else {
        rec = cache.c.lowerRecord;
        w = cache.c.lowerWeight;
    }

    // RECORD level: re-read only when the pick (or its paired weight —
    // an adjacent tile of the same zone at a different falloff sample)
    // changes. A stale/corrupt index (rec >= n) caches as unusable for
    // the cell — never garbage, never a re-fetch storm.
    if (rec != cache.rec || w != cache.weight)
    {
        cache.rec = rec;
        cache.weight = w;
        if (rec < n)
        {
            const EnvTileRecord r = vk::BufferPointer<EnvTileRecord>(
                frame.envZones + uint64_t(rec) * 48u).Get();
            EnvTileUnpack(r, cache.look);
            cache.usable = w > 0.0;
        }
        else
        {
            cache.usable = false;
        }
    }
    weight = cache.weight;
    z = cache.look;
    return cache.usable;
}

// Zone-tinted + zone-intensity ambient irradiance for N (the
// EvaluateIrradiance shape with the zone's skyTint applied to the env
// colors and its ambientI scaling the result). Honors the IBL flag like
// the plain path. The caller lerps with the base EvaluateIrradiance by
// EnvZoneAt's weight.
float3 EnvZoneIrradiance(EnvZoneLook z, float3 N, EnvironmentFrameData env)
{
    const float ibl = max(z.ambientI, 0.0) * env.fogEndAndFlags.w;
    if (ibl <= 0.0)
        return float3(0.0, 0.0, 0.0);
    const float exponent = max(env.skyHorizon.w, 0.01);
    const float up = saturate(N.y);
    const float dn = saturate(-N.y);
    const float3 zenith = env.skyZenith.rgb * z.skyTint;
    const float3 horizon = env.skyHorizon.rgb * z.skyTint;
    const float3 groundCol = env.ground.rgb * z.skyTint;
    const float3 skyPart = lerp(horizon, zenith, pow(up, exponent));
    const float3 gndPart = lerp(horizon, groundCol, saturate(dn * 1.5));
    const float3 hemi = lerp(gndPart, skyPart, saturate(N.y * 0.5 + 0.5));
    return hemi * ibl;
}

// Zone fog color (HDR) + density; the fog INTENSITY stays global
// (env.fogColor.w). The caller lerps with the base by the zone weight.
void EnvZoneFog(EnvZoneLook z, FrameUniforms frame,
    out float3 fogHdr, out float fogDensity)
{
    fogHdr = z.fogColor * frame.env.fogColor.w;
    fogDensity = max(z.fogDensity, 0.0);
}

// Exp2 fog factor with an EXPLICIT density (the zone-blended density -
// ComputeFogFactor reads the env's). Same mode/height logic.
float ComputeFogFactorDensity(float viewDepth, float worldY,
    EnvironmentFrameData env, float density)
{
    float fogMode = env.fogEndAndFlags.z;
    if (fogMode < 0.5f)
        return 0.0f;
    float f = 0.0f;
    if (fogMode < 1.5f)
    {
        float d = max(density, 0.0f);
        float x = d * max(viewDepth, 0.0f);
        f = 1.0f - exp(-x * x);
    }
    else
    {
        float s = env.fogDensityHeight.w;
        float e = max(env.fogEndAndFlags.x, s + 1e-4f);
        f = saturate((max(viewDepth, 0.0f) - s) / (e - s));
    }
    float falloff = env.fogDensityHeight.z;
    if (falloff > 0.0f)
    {
        float h = env.fogDensityHeight.y;
        float hf = exp(-max(worldY - h, 0.0f) * falloff);
        f *= hf;
    }
    return saturate(f);
}

// Zone-blended volumetric params (scattering ratio + extinction density).
// The zone's volumetrics is a RATIO over the pass base (1 = the global
// intensity, 0 = off). The caller passes the base values and the zone
// weight; output is already lerped toward the zone.
void EnvZoneVolumetrics(EnvZoneLook z, float weight,
    float baseIntensity, float baseDensity,
    out float intensity, out float density)
{
    intensity = max(baseIntensity, 0.0)
        * lerp(1.0, max(z.volumetrics, 0.0), saturate(weight));
    density = lerp(max(baseDensity, 0.0), max(z.volumetricDensity, 0.0),
        saturate(weight));
}
