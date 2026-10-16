// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// SgRegularVolume sampling: Ready-at-Dawn-style regular SG lattice.
//
// CPU side: Engine/GI/regular_volume.h (SgRegularVolume, PackGpuLayers).
// GPU layout: 7 RGBA16F 3D textures, one texel per lattice voxel.
// Texel t of a voxel carries stream halves [t*4, t*4+3] of the 27-half
// lobe-major RGB stream (h = lobe*3 + channel); slot 27 is zero padding.
//
// Runtime: worldPos -> OBB uvw -> 7 hardware-filtered 3D reads ->
// unpack 27 coeffs -> evaluate 9 SGs with the shared SgDiffuseIrradiance /
// SgGiSpecular paths. No support radius, no visibility, no fallback:
// walls are volume boundaries (one volume per room/street), and
// trilinear filtering is the spatial low-pass.
//
// The GPU OBB descriptor (SgRegularObbGpu, sg_lightmap_types.hlsli) uses
// float4s with explicit padding; helpers here take it as input.

#define SG_REGULAR_LAYERS 7

// Unpacks one voxel's 27 coefficients from the 7 filtered RGBA texels.
// Each Texture3D<float4> holds fp16 data sampled with Linear/Clamp;
// filtering happens per-channel in half precision, exactly like the CPU
// trilinear mirror (SgRegularVolume::Sample).
void SgRegularUnpack(float4 layers[SG_REGULAR_LAYERS], out float3 amps[SG_GI_LOBES])
{
    [unroll]
    for (uint lobe = 0; lobe < SG_GI_LOBES; ++lobe)
    {
        const uint h = lobe * 3;
        float rgb[3];
        [unroll]
        for (uint c = 0; c < 3; ++c)
        {
            const uint s = h + c;
            const uint t = s >> 2;
            const uint k = s & 3;
            rgb[c] = (k == 0) ? layers[t].x : (k == 1) ? layers[t].y : (k == 2) ? layers[t].z : layers[t].w;
        }
        amps[lobe] = float3(rgb[0], rgb[1], rgb[2]);
    }
}

// World position -> OBB uvw in [0,1]^3. Returns false when outside.
bool SgRegularUvw(SgRegularObbGpu obb, float3 pos, out float3 uvw)
{
    const float3 d = pos - obb.center.xyz;
    const float3 local = float3(dot(d, obb.axisX.xyz) / max(obb.halfExtents.x, 1e-6),
                                dot(d, obb.axisY.xyz) / max(obb.halfExtents.y, 1e-6),
                                dot(d, obb.axisZ.xyz) / max(obb.halfExtents.z, 1e-6));
    uvw = local * 0.5 + 0.5;
    return all(uvw >= 0.0) && all(uvw <= 1.0);
}

// Samples volume `vol` (0..7) of the frame's regular grids at world pos:
// 7 hardware-trilinear 3D reads through the bindless volumeTextures
// mirror binding (samplers[2] = linear clamp), then unpacks the 9-lobe
// amplitudes. Returns false when the position is outside every volume
// (caller falls back to the analytic ambient).
bool SgRegularSampleFrame(FrameUniforms frame, float3 pos,
    out float3 amps[SG_GI_LOBES])
{
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        amps[i] = float3(0, 0, 0);
    const uint count = min(frame.regCount, (uint)SG_REG_MAX_VOLUMES);
    [loop]
    for (uint v = 0; v < count; ++v)
    {
        const SgRegularObbGpu obb = frame.regObbs.Get().obbs[v];
        float3 uvw;
        if (!SgRegularUvw(obb, pos, uvw))
            continue;
        float4 layers[SG_REGULAR_LAYERS];
        [unroll]
        for (uint t = 0; t < SG_REGULAR_LAYERS; ++t)
        {
            // v/t are wave-uniform loop counters over uniform memory
            // (FrameUniforms) — plain bindless indices keep the descriptor
            // fetches scalar; NonUniformResourceIndex would force divergent
            // descriptor paths for indices that cannot diverge.
            layers[t] = volumeTextures[frame.regLayers[v * 7u + t]]
                .Sample(samplers[2], uvw);
        }
        SgRegularUnpack(layers, amps);
        return true;
    }
    return false;
}
