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

float3 EvaluateSpecularEnv(float3 R, float3 F, float roughness, EnvironmentFrameData env,
    float3 worldPos, FrameUniforms frame, uint tileIdx,
    uint fpLightCount, uint fpMaxPerTile,
    vk::BufferPointer<ForwardPlusTileCountBuffer> fpTileCounts,
    vk::BufferPointer<ForwardPlusTileIndexBuffer> fpTileIndices,
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights)
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
            return envCol2 * F * (gloss * gloss * 0.9f + 0.1f);
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
        uint perTile = min(fpMaxPerTile, (uint)MAX_FP_PER_TILE);
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
        return envCol * F * (gloss * gloss * 0.9f + 0.1f);
    return envCol * ibl * F * (gloss * gloss * 0.9f + 0.1f);
}

// Specular-visibility proxy from the baked GI (Lagarde-style occlusion):
// the pixel's baked irradiance vs the UNOCCCLUDED sun+sky reference in
// the same transport units approximates how much environment the surface
// can actually see. Without it, shaded metals under roofs mirror the full
// sunlit courtyard from the probe capture ("crazy luminous" gold trim) —
// geometry they cannot see through. giScale normalizes the UI GI-intensity
// multiplier back out of the baked term. Surfaces with no baked source
// keep 1 (callers decide).
float SpecularOcclusionFromGI(float3 bakedE, float giScale, float3 N,
    EnvironmentFrameData env)
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
    return saturate(dot(e, kLum) / refL);
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
// Environment zones (tiled world/env separation; see Rendergraph/envzones.h
// + the FrameUniforms envZone* tail). The map is one RGBA8 weight texture
// (R/G/B/A = zone definitions 0..3); the weights normalize at sample so a
// texel blends up to four zones, and the texture's bilinear filtering
// blends BETWEEN texels. Zones own the sky tint + ambient intensity, the
// fog color/density and the volumetric scattering/extinction — the sun
// stays global. All helpers fall back to the plain env path when
// envZoneMapIndex is off (weight 1 on zone 0 == the authored env look when
// zone 0 mirrors it).
// ---------------------------------------------------------------------------

// Normalized zone weights at world XZ (zone 0 when the map is off/empty).
float4 EnvZoneWeights(float2 worldXZ, FrameUniforms frame)
{
    if (frame.envZoneMapIndex == 0xffffffffu)
        return float4(1.0, 0.0, 0.0, 0.0);
    Texture2D map = textures[frame.envZoneMapIndex];
    SamplerState smp = samplers[frame.envZoneSampler];
    float2 uv = worldXZ / max(frame.envZoneWorldSize, 1e-3) + 0.5;
    float4 w = map.Sample(smp, saturate(uv));
    float s = dot(w, float4(1.0, 1.0, 1.0, 1.0));
    return (s > 1e-4) ? w / s : float4(1.0, 0.0, 0.0, 0.0);
}

// Zone-tinted + zone-intensity ambient irradiance for N (the
// EvaluateIrradiance shape with each zone's skyTint applied to the env
// colors and its ambientI scaling the result). w = EnvZoneWeights.
float3 EnvZoneIrradiance(float3 N, float4 w, EnvironmentFrameData env,
    FrameUniforms frame)
{
    // Blend the tints + intensities (weights already normalized).
    float3 tint = w.x * frame.envZoneSky[0].rgb
        + w.y * frame.envZoneSky[1].rgb
        + w.z * frame.envZoneSky[2].rgb
        + w.w * frame.envZoneSky[3].rgb;
    float ambI = w.x * frame.envZoneSky[0].a
        + w.y * frame.envZoneSky[1].a
        + w.z * frame.envZoneSky[2].a
        + w.w * frame.envZoneSky[3].a;
    if (ambI <= 0.0f)
        return float3(0.0, 0.0, 0.0);
    // Tinted hemisphere: the EvaluateIrradiance shape over tinted colors.
    float exponent = max(env.skyHorizon.w, 0.01f);
    float up = saturate(N.y);
    float dn = saturate(-N.y);
    float3 zenith = env.skyZenith.rgb * tint;
    float3 horizon = env.skyHorizon.rgb * tint;
    float3 groundCol = env.ground.rgb * tint;
    float3 skyPart = lerp(horizon, zenith, pow(up, exponent));
    float3 gndPart = lerp(horizon, groundCol, saturate(dn * 1.5f));
    float3 hemi = lerp(gndPart, skyPart, saturate(N.y * 0.5f + 0.5f));
    return hemi * ambI;
}

// Zone-blended fog color (HDR) + density for the analytic fog term. The
// fog INTENSITY stays global (fogColor.w); zones own color + density.
void EnvZoneFog(float4 w, FrameUniforms frame,
    out float3 fogHdr, out float fogDensity)
{
    float3 c = w.x * frame.envZoneFog[0].rgb
        + w.y * frame.envZoneFog[1].rgb
        + w.z * frame.envZoneFog[2].rgb
        + w.w * frame.envZoneFog[3].rgb;
    float d = w.x * frame.envZoneFog[0].a
        + w.y * frame.envZoneFog[1].a
        + w.z * frame.envZoneFog[2].a
        + w.w * frame.envZoneFog[3].a;
    fogHdr = c * frame.env.fogColor.w;
    fogDensity = max(d, 0.0);
}

// Exp2 fog factor with an EXPLICIT density (the zone-blended density —
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

// Zone-blended volumetric params (scattering intensity + extinction
// density) — the volumetric pass blends its march constants at the ray
// start. Both return the plain-env pass values when the map is off.
void EnvZoneVolumetrics(float4 w, FrameUniforms frame,
    float baseIntensity, float baseDensity,
    out float intensity, out float density)
{
    float vi = w.x * frame.envZoneVol[0].x
        + w.y * frame.envZoneVol[1].x
        + w.z * frame.envZoneVol[2].x
        + w.w * frame.envZoneVol[3].x;
    float vd = w.x * frame.envZoneVol[0].y
        + w.y * frame.envZoneVol[1].y
        + w.z * frame.envZoneVol[2].y
        + w.w * frame.envZoneVol[3].y;
    intensity = max(baseIntensity, 0.0) * max(vi, 0.0);
    density = max(vd, 0.0);
}
