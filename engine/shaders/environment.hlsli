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
// Probe selection is tile-bucketed when the Forward+ cull filed per-tile
// probe lists (tileIdx != 0xffffffffu and probeMaxPerTile > 0): the pixel
// then scans only its tile's handful of entries. Otherwise (stale lists)
// it falls back to the global widest-first scan with break-on-full-weight.
// Either way the baked chains are GGX-preconvolved (roughness =
// mip/(mips-1)), so sampling selects the LOD from the material roughness
// (+ the UI LOD bias); the clamp sampler avoids cube-seam wrap bleeding.
// tileIdx is the caller's 16px-tile index (same tiling as the FP light
// lists), or 0xffffffffu when the caller has no tile (global scan).
float3 EvaluateSpecularEnv(float3 R, float3 F, float roughness, EnvironmentFrameData env,
    float3 worldPos, FrameUniforms frame, uint tileIdx)
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
    uint bestProbeIndex = 0xffffffffu;

    // Tile-bucketed fast path. The cull's outer-sphere test is conservative
    // (influence + fade vs tile frustum + depth range), so a tile miss
    // implies a global miss: when the lists are fresh and the tile finds
    // nothing, the pixel takes the analytic sky directly with no fallback
    // scan. Overflow (count > perTile) drops far locals — the accepted
    // Forward+ trade-off, same as lights/decals.
    bool tileFresh = useBaked && tileIdx != 0xffffffffu && tileIdx < (uint)MAX_FP_TILES
        && frame.probeMaxPerTile > 0u && frame.reflectionProbeCount > 0u;
    if (tileFresh) {
        ReflectionProbeBuffer probeBuf = frame.reflectionProbes.Get();
        uint perTile = min(frame.probeMaxPerTile, (uint)MAX_FP_PROBES_PER_TILE);
        uint stride = max(frame.probeMaxPerTile, 1u);
        uint listCount = min(frame.probeTileCounts.Get().counts[tileIdx], perTile);
        [loop]
        for (uint s = 0; s < listCount; ++s) {
            uint pi = frame.probeTileIndices.Get().indices[tileIdx * stride + s];
            if (pi >= frame.reflectionProbeCount)
                continue;
            ReflectionProbe probe = probeBuf.probes[pi];
            if ((uint)probe.fadeCubemap.y == 0xffffffffu)
                continue;
            float dist = length(worldPos - probe.posInfluence.xyz);
            float influenceRadius = probe.posInfluence.w;
            float fadeRadius = probe.fadeCubemap.x;
            float weight = 0.0f;
            if (dist <= influenceRadius) {
                weight = 1.0f;
            } else if (fadeRadius > 0.0f && dist <= influenceRadius + fadeRadius) {
                weight = 1.0f - (dist - influenceRadius) / fadeRadius;
            }
            if (weight > bestWeight) {
                bestWeight = weight;
                bestProbeIndex = pi;
                if (bestWeight >= 1.0f)
                    break;
            }
        }
        if (bestWeight > 0.0f && bestProbeIndex != 0xffffffffu) {
            ReflectionProbe best = probeBuf.probes[bestProbeIndex];
            uint cubemapIdx = (uint)best.fadeCubemap.y;
            float mips = max(best.fadeCubemap.z, 1.0f);
            float lod = clamp(roughness * (mips - 1.0f) + lodBias, 0.0f, mips - 1.0f);
            TextureCube cubemap = cubemaps[NonUniformResourceIndex(cubemapIdx)];
            envCol = cubemap.SampleLevel(cubeSampler, R, lod).rgb;
            foundProbe = true;
        }
    }

    // Find nearest reflection probe within influence radius.
    // PERF: this global scan runs per lit pixel over BDA-backed rows, so it
    // is frame-rate memory bandwidth — it only runs when the tile lists are
    // stale (probeMaxPerTile == 0). Rows are 32 bytes, widest-influence
    // first, break at the first full-weight probe (exact: nothing beats
    // weight 1.0, ties keep the first row).
    if (!tileFresh && !foundProbe && useBaked && frame.reflectionProbeCount > 0) {
        ReflectionProbeBuffer probeBuf = frame.reflectionProbes.Get();
        [loop]
        for (uint i = 0; i < frame.reflectionProbeCount; ++i) {
            ReflectionProbe probe = probeBuf.probes[i];
            float3 probePos = probe.posInfluence.xyz;
            float influenceRadius = probe.posInfluence.w;
            float fadeRadius = probe.fadeCubemap.x;
            uint cubemapIdx = (uint)probe.fadeCubemap.y;

            if (cubemapIdx == 0xffffffffu) continue;

            float dist = length(worldPos - probePos);
            float weight = 0.0f;
            if (dist <= influenceRadius) {
                weight = 1.0f;
            } else if (fadeRadius > 0.0f && dist <= influenceRadius + fadeRadius) {
                weight = 1.0f - (dist - influenceRadius) / fadeRadius;
            }

            if (weight > bestWeight) {
                bestWeight = weight;
                bestProbeIndex = i;
                if (bestWeight >= 1.0f)
                    break;
            }
        }

        if (bestWeight > 0.0f && bestProbeIndex != 0xffffffffu) {
            // Sample the nearest probe's GGX chain at the roughness LOD.
            ReflectionProbe best = probeBuf.probes[bestProbeIndex];
            uint cubemapIdx = (uint)best.fadeCubemap.y;
            float mips = max(best.fadeCubemap.z, 1.0f);
            float lod = clamp(roughness * (mips - 1.0f) + lodBias, 0.0f, mips - 1.0f);
            TextureCube cubemap = cubemaps[NonUniformResourceIndex(cubemapIdx)];
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
