#pragma once
// Convex GI volumes (Engine/GI/cubemap_volume.h): shaped local probe
// grids cooked by sparse cubemap-reprojection bakes, blended over the
// world field by ordered priority compositing.
//
// Blend model (TPP/HZD/BO3 lineage — volumes blend, grids never
// cross-interpolate):
//
//   E = SampleGlobalOctree(P, N)      // or the sky fallback
//   for (v : volumesSortedLowToHighPriority)
//       E = lerp(E, SampleVolume(v, P, N), v.Weight(P))
//
// Per-face fades make a doorway transition gradual while walls stay
// tight (FOX innerScales): the weight is the MIN over planes of a
// smoothstep across the plane (nearest face dominates, so a wide
// doorway fade survives next to a tight floor).
//
// Runtime per volume: 7 hardware-trilinear RGBA16F 3D reads through the
// bindless volumeTextures mirror (samplers[2] = linear clamp) — the
// same GPU layout and unpack as the regular grids (SgRegularUnpack).
//
// CPU parity: CvPlaneWeight / CvVolumeDef::Weight (cubemap_volume.h)
// MUST match SgCvPlaneWeight / SgCvWeight here (same numbers, same
// order; unit tested).

// Per-plane fade weight. d = signed distance INSIDE the plane
// (n.P + d <= 0 inside). smoothstep(-outsideFade, +insideFade, d):
// 0 outside beyond the outside fade, 1 inside past the inside fade.
float SgCvPlaneWeight(CvPlaneGpu p, float3 pos)
{
    const float inside = -(dot(p.normal_d.xyz, pos) + p.normal_d.w);
    const float e0 = -p.fades.y; // -outsideFade
    const float e1 = p.fades.x;  // +insideFade
    if (e1 - e0 < 1e-5)
        return inside >= e1 ? 1.0 : 0.0;
    const float t = saturate((inside - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

// Volume weight at a world position: min over planes (nearest face
// dominates; early-out keeps the fully-outside pixel cheap).
float SgCvWeight(CvVolumeGpu v, float3 pos)
{
    const uint count = min(v.planeCount_pad.x, (uint)SG_CV_MAX_PLANES);
    float w = 1.0;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        w = min(w, SgCvPlaneWeight(v.planes[i], pos));
        if (w <= 0.0)
            return 0.0;
    }
    return w;
}

// World position -> lattice uvw in [0,1]^3 (clamped: the fade band
// extends a little past the AABB, clamp-to-edge keeps those taps
// defined; the weight function scales their contribution anyway).
float3 SgCvUvw(CvVolumeGpu v, float3 pos)
{
    const float3 dims = max(float3(v.dims_priority.xyz), float3(1.0, 1.0, 1.0));
    const float3 scale = max(v.gridMin_spacing.w * dims,
        float3(1e-6, 1e-6, 1e-6));
    const float3 rel = (pos - v.gridMin_spacing.xyz) / scale;
    return saturate(rel);
}

// Samples one volume's dense grid at world pos: 7 hardware-trilinear
// 3D reads + the shared 27-half unpack. Mirrors CvVolume::Sample.
void SgCvSampleVolume(FrameUniforms frame, uint vol, float3 pos,
    out float3 amps[SG_GI_LOBES])
{
    const CvVolumeGpu v = frame.cvolVolumes.Get().volumes[vol];
    const float3 uvw = SgCvUvw(v, pos);
    float4 layers[SG_CV_LAYERS];
    [unroll]
    for (uint t = 0; t < SG_CV_LAYERS; ++t)
    {
        layers[t] = volumeTextures[
            NonUniformResourceIndex(frame.cvolLayers[vol * 7u + t])]
            .Sample(samplers[2], uvw);
    }
    SgRegularUnpack(layers, amps);
}
