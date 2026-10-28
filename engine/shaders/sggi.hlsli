// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// SHARED SG-GI evaluation math (the SG basis + the diffuse/specular
// response). The octree probe walkers (SgGiSample/SgGiSampleMean + the
// probe helpers) are RETIRED WITH THE .sggi RUNTIME (stage 3 of the
// octree retirement): the baked-GI consumers are the convex GI volumes
// (sg_gi_volume.hlsli), the regular grids (sg_regular.hlsli) and the
// lightmaps (sg_lightmap.hlsli), and every one of them evaluates its
// lobe amps through THIS file's irradiance/specular kernels. Types +
// the fixed 9-lobe basis live in sggi_types.hlsli (common.hlsli
// includes both; the FrameUniforms gi* mirror fields stay as the
// zeroed legacy ABI).
//
// The SgGiBasisValue/SgGiProductIntegral pair is the SG basis algebra
// (the CPU twin is Engine/GI/sg.cpp) — preserved as shared utility
// even with no current in-shader caller.

// Lobe basis value for a direction.// Lobe basis value for a direction.
float SgGiBasisValue(uint lobe, float3 dir)
{
    return exp(SG_GI_SHARPNESS * (dot(SG_GI_AXES[lobe], dir) - 1.0));
}

// Closed-form product integral of basis lobe x an external SG.
float SgGiProductIntegral(uint lobe, float otherAmp, float otherSharp, float3 otherAxis)
{
    const float3 sum = SG_GI_SHARPNESS * SG_GI_AXES[lobe] + otherSharp * otherAxis;
    const float lp = length(sum);
    [branch]
    if (lp < 1e-6)
        return otherAmp * 12.566371 * exp(-(SG_GI_SHARPNESS + otherSharp));
    const float amp = otherAmp * exp(lp - SG_GI_SHARPNESS - otherSharp);
    return amp * (6.2831853 / lp) * (1.0 - exp(-2.0 * lp));
}

// Clamped-cosine kernel K(t) for a unit basis lobe: degree-6 Chebyshev
// fit (Clenshaw), tail clamped to >= 0. The true kernel is smooth and
// strictly positive on [-1, 1] (fat lobe tails), so no cutoff is needed.
// Mirrors SgCosKernel in Engine/GI/sg.cpp.
float SgGiCosKernel(float t)
{
    t = clamp(t, -1.0, 1.0);
    float b1 = 0.0;
    float b2 = 0.0;
    [unroll]
    for (int k = SG_GI_CHEB_TERMS - 1; k >= 1; --k)
    {
        const float next = 2.0 * t * b1 - b2 + SG_GI_COS_CHEB[k];
        b2 = b1;
        b1 = next;
    }
    const float kernel = SG_GI_COS_CHEB[0] + t * b1 - b2;
    return max(kernel, 0.0);
}

// Diffuse irradiance E(n) = int L(omega) (n.omega)+ domega via the
// precomputed clamped-cosine kernel (radiance units; caller applies
// albedo / PI).
float3 SgGiDiffuseIrradiance(float3 amps[SG_GI_LOBES], float3 n)
{
    float3 sum = float3(0, 0, 0);
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        sum += amps[i] * SgGiCosKernel(dot(SG_GI_AXES[i], n));
    return sum;
}

// Rough GGX specular response: the GGX reflection kernel K(w) = D(h(w))/4
// (view assumed along the normal, the standard ambient-specular
// simplification; integral == 1 for a constant environment), approximated
// by a normalized SG around the reflection vector with sharpness
// 1 / (4 alpha^2) (GGX is Gaussian-ish in tan(H-angle) with sigma = alpha
// and reflection doubles the angle). Each lobe integrates against it
// analytically. Caller supplies Fresnel; blend to the analytic env for
// smooth surfaces.
float3 SgGiSpecular(float3 amps[SG_GI_LOBES], float3 r, float roughness)
{
    const float a = clamp(roughness, 0.04, 1.0);
    const float alpha = a * a;
    const float sharp = 1.0 / (4.0 * alpha * alpha + 1e-4);
    const float norm = sharp / (6.2831853 * (1.0 - exp(-2.0 * sharp)));
    float3 sum = float3(0, 0, 0);
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        sum += amps[i] * SgGiProductIntegral(i, norm, sharp, r);
    return sum;
}
