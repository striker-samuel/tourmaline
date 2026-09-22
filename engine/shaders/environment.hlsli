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
// Uses reflection probes (nearest by world position) if available,
// otherwise falls back to global cubemapIndex, then analytic sky.
float3 EvaluateSpecularEnv(float3 R, float3 F, float roughness, EnvironmentFrameData env,
    float3 worldPos, FrameUniforms frame)
{
    float ibl = max(env.ground.w, 0.0f) * env.fogEndAndFlags.w;
    if (ibl <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    
    float3 envCol;
    bool foundProbe = false;
    
    // Find nearest reflection probe within influence radius
    float bestWeight = 0.0f;
    uint bestProbeIndex = 0xffffffffu;
    
    if (frame.reflectionProbeCount > 0) {
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
            } else if (dist <= influenceRadius + fadeRadius) {
                weight = 1.0f - (dist - influenceRadius) / fadeRadius;
            }
            
            if (weight > bestWeight) {
                bestWeight = weight;
                bestProbeIndex = i;
            }
        }
        
        if (bestWeight > 0.0f && bestProbeIndex != 0xffffffffu) {
            // Sample the nearest probe's cubemap
            uint cubemapIdx = (uint)probeBuf.probes[bestProbeIndex].fadeCubemap.y;
            TextureCube cubemap = cubemaps[NonUniformResourceIndex(cubemapIdx)];
            envCol = cubemap.Sample(samplers[0], R).rgb;
            foundProbe = true;
        }
    }
    
    if (!foundProbe && frame.cubemapIndex != 0xffffffffu) {
        // Fallback to global cubemap
        TextureCube cubemap = cubemaps[NonUniformResourceIndex(frame.cubemapIndex)];
        envCol = cubemap.Sample(samplers[0], R).rgb;
        foundProbe = true;
    }
    
    // Normalize cubemap radiance to match analytic specular IBL scale.
    // Analytic path uses 0..1 sky colors * iblIntensity (env.ground.w).
    // Cubemap is baked in absolute radiance (skyIntensity=env.skyZenith.w).
    // Normalize by skyIntensity/iblIntensity ratio.
    if (foundProbe) {
        float skyIntensity = env.skyZenith.w;
        float iblIntensity = max(env.ground.w, 1e-4f);
        envCol *= iblIntensity / max(skyIntensity, 1e-4f);
    }
    
    if (!foundProbe) {
        // Fallback to analytic sky
        float3 zenith = env.skyZenith.rgb;
        float3 horizon = env.skyHorizon.rgb;
        float3 groundCol = env.ground.rgb;
        float3 skyPart = lerp(horizon, zenith, pow(saturate(R.y), 1.5f));
        float3 gndPart = lerp(horizon, groundCol, saturate(-R.y * 2.0f));
        envCol = R.y >= 0.0f ? skyPart : gndPart;
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
