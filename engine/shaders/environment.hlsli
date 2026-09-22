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
float3 EvaluateSpecularEnv(float3 R, float3 F, float roughness, EnvironmentFrameData env,
    float3 worldPos, FrameUniforms frame, uint tileIdx,
    uint fpLightCount, uint fpMaxPerTile,
    vk::BufferPointer<ForwardPlusTileCountBuffer> fpTileCounts,
    vk::BufferPointer<ForwardPlusTileIndexBuffer> fpTileIndices,
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights)
{
    // Specular gate: iblIntensity (ground.w) ONLY. Deliberately NOT the
    // fogEndAndFlags.w diffuse-isolation flag (ambient modes): smooth
    // surfaces cannot do mirrors from the broad baked lobes, so they keep
    // env reflections in every mode (see the lightmap/probe specular code
    // that lerps toward this). Gating this to zero would turn every chrome
    // ball black in Probes/Lightmaps/Off modes. The cubemap toggle rides
    // useBaked below (off = analytic sky, never black).
    float ibl = max(env.ground.w, 0.0f);
    if (ibl <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    // Baked-cubemap kill switch (UI): off falls back to the analytic sky,
    // never to black — probe-covered pixels must not go dark just because
    // the bake is disabled.
    bool useBaked = frame.cubemapIntensity > 0.0f;
    uint cubeDbg = frame.cubemapDebug;

    SamplerState cubeSampler = samplers[NonUniformResourceIndex(frame.cubemapSampler)];
    float lodBias = frame.cubemapLodBias;

    float3 envCol = float3(0.0f, 0.0f, 0.0f);
    bool foundProbe = false;
    float bestWeight = 0.0f;
    uint bestCubemapIdx = 0xffffffffu;
    float bestMips = 1.0f;

    // Tile-bucketed probe scan. The cull's outer-sphere test is
    // conservative (influence + fade vs tile frustum + depth range), so a
    // tile miss implies a global miss: when the lists are fresh and the
    // tile holds no contributing probe, the pixel takes the analytic sky
    // (then the global cubemap below) with no fallback scan. Upload order
    // is widest-influence first and nothing beats weight 1.0 (ties keep
    // the first entry), so break on full weight — exact, and usually the
    // first tile entry.
    // Index space: lights [0, fpLightCount), probes [fpLightCount,
    // fpLightCount + frame.probeEntries). The bound MUST include the
    // probe tail — a lights-only bound skips every probe entry and the
    // walk silently degrades to the global fallback cubemap. Scenes with
    // zero lights (sponza) still walk: entryTotal > 0 via the tail.
    const uint entryTotal = fpLightCount + frame.probeEntries;
    bool tileFresh = useBaked && tileIdx != 0xffffffffu && tileIdx < (uint)MAX_FP_TILES
        && frame.probeTileFresh != 0u && entryTotal > 0u;
    if (tileFresh) {
        uint perTile = min(fpMaxPerTile, (uint)MAX_FP_PER_TILE);
        uint stride = max(fpMaxPerTile, 1u);
        uint listCount = min(fpTileCounts.Get().counts[tileIdx], perTile);
        [loop]
        for (uint s = 0; s < listCount; ++s) {
            uint li = fpTileIndices.Get().indices[tileIdx * stride + s];
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
            if (weight > bestWeight) {
                bestWeight = weight;
                bestCubemapIdx = cubemapIdx;
                bestMips = max(L.direction.z, 1.0f);
                if (bestWeight >= 1.0f)
                    break;
            }
        }
        if (bestWeight > 0.0f && bestCubemapIdx != 0xffffffffu) {
            float lod = clamp(roughness * (bestMips - 1.0f) + lodBias, 0.0f, bestMips - 1.0f);
            TextureCube cubemap = cubemaps[NonUniformResourceIndex(bestCubemapIdx)];
            envCol = cubemap.SampleLevel(cubeSampler, R, lod).rgb;
            foundProbe = true;
        }
    }

    if (!foundProbe && useBaked && frame.cubemapIndex != 0xffffffffu) {
        // Fallback to global cubemap (sharp LOD + UI bias; no per-probe
        // mip count is carried for this path, so bias only).
        TextureCube cubemap = cubemaps[NonUniformResourceIndex(frame.cubemapIndex)];
        envCol = cubemap.SampleLevel(cubeSampler, R, max(lodBias, 0.0f)).rgb;
        foundProbe = true;
        bestWeight = 1.0f;
    }

    if (!foundProbe) {
        // Fallback to analytic sky
        float3 zenith = env.skyZenith.rgb;
        float3 horizon = env.skyHorizon.rgb;
        float3 groundCol = env.ground.rgb;
        float3 skyPart = lerp(horizon, zenith, pow(saturate(R.y), 1.5f));
        float3 gndPart = lerp(horizon, groundCol, saturate(-R.y * 2.0f));
        envCol = R.y >= 0.0f ? skyPart : gndPart;
    } else {
        // Baked cubemap: UI intensity. Convention: bakes store sky-relative
        // colors (cook divides out skyIntensity), so the shader applies ibl
        // uniformly to baked and analytic paths below — the IBL slider
        // scales both, and no bake-time intensity is frozen in.
        envCol *= max(frame.cubemapIntensity, 0.0f);
    }

    // Debug views (UI): raw cubemap without Fresnel/roughness weighting,
    // or the probe blend weight as heat.
    if (cubeDbg == 1u && foundProbe) {
        return envCol * ibl;
    }
    if (cubeDbg == 2u) {
        float w = foundProbe ? bestWeight : 0.0f;
        return float3(w, 1.0f - abs(w - 0.5f) * 2.0f * 0.5f, 1.0f - w) * ibl;
    }

    float gloss = 1.0f - clamp(roughness, 0.0f, 1.0f);
    return envCol * ibl * F * (gloss * gloss * 0.9f + 0.1f);
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
