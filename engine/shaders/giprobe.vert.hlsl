// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GI probe visualization (see Rendergraph/giprobe.h): a DEDICATED pass
// — no scene pc, its own push block (GiProbePush, mirrored by the C++
// struct in giprobe.h). VSIn: the terrain FULL format's attributes 0 + 1
// only (POSITION + NORMAL — the tangent/color attributes are bound but
// undeclared, so they are simply not fetched).
//
// The vertex shader IS the visualization: per instance it Loads the
// probe's EXACT texel from the volume's 7 SG layers (Texture3D.Load —
// never the filtered Sample; the debug answer must be THIS probe's
// bake, not the trilerp of its neighborhood), unpacks the 9 lobe
// amplitudes (SgRegularUnpack) and evaluates
// SgGiDiffuseIrradiance(amps, sphereDirection) — the vertex color is
// the baked directional GI the runtime would answer for a surface
// facing that way at the probe's position. Fragment: pure pass-through.
#define GIPROBE_PASS
#include "common.hlsli"
#include "sg_lightmap_types.hlsli" // SG_GI_VOLUME_LAYERS (7)
#include "sg_regular.hlsli"        // SgRegularUnpack
#include "sggi.hlsli"              // SgGiDiffuseIrradiance + SG_GI_AXES
// (GIPROBE_PASS also gates common.hlsli's scene-pc block — this pass
// pushes its OWN GiProbePush, the probeball convention.)

// Fixed radius (world units) — hardcoded by design (no LOD, no knob).
#define GI_PROBE_RADIUS 0.12
// The instance-table cap (the C++ mirror: smvk::giprobe::
// kGiProbeMaxInstances; the old ImGui overlay's 6000-dot budget, kept
// as the GPU draw budget).
#define GI_PROBE_MAX_INSTANCES 6000

// Mirrors smvk::giprobe::GiProbeInstance (32 B).
struct GiProbeInstance
{
    float3 pos;
    float pad_;
    uint3 grid;          // the exact Texture3D texel (lattice coords)
    uint pad2_;
};
struct GiProbeInstanceArray
{
    GiProbeInstance rows[GI_PROBE_MAX_INSTANCES];
};

// Mirrors smvk::giprobe::GiProbePush (C++, giprobe.h) exactly.
struct GiProbePush
{
    float4x4 viewProj;   // 0  the rendering view's VP (reversed-Z, Y-flipped)
    float4 eyeMaxDist;   // 64 xyz camera, w the distance filter (m)
    vk::BufferPointer<FrameUniforms> frame; // 72 the giVolumeLayers slots
    vk::BufferPointer<GiProbeInstanceArray> instances; // 80 this volume's rows
    uint volumeIndex;    // 88 the giVolumeLayers row / 7
    uint pad_[3];        // 92 (16-align tail; 112 total)
};

[[vk::push_constant]]
ConstantBuffer<GiProbePush> pb;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 normal : NORMAL;
};

struct VSOut
{
    float4 pos : SV_Position;
    // The evaluated baked irradiance (linear HDR radiance units — the
    // same E(n) the lit body's ambient consumes, pre-albedo/pre-scale).
    [[vk::location(0)]] float3 color : COLOR0;
};

VSOut mainVS(VSIn IN, uint instanceIndex : SV_InstanceID)
{
    VSOut OUT;
    const GiProbeInstance inst = pb.instances.Get().rows[instanceIndex];
    const float3 worldPos = inst.pos + IN.position * GI_PROBE_RADIUS;

    // The old ImGui overlay's camera-distance filter, GPU-side: the
    // test uses the instance CENTER (identical for every vertex of the
    // instance), so a culled instance collapses to ONE degenerate point
    // — all 80 triangles zero-area, zero pixels, no per-triangle cost.
    {
        const float3 d = inst.pos - pb.eyeMaxDist.xyz;
        if (dot(d, d) > pb.eyeMaxDist.w * pb.eyeMaxDist.w)
        {
            OUT.pos = float4(2.0, 2.0, 2.0, 1.0); // outside every clip plane
            OUT.color = float3(0.0, 0.0, 0.0);
            return OUT;
        }
    }

    // The probe's EXACT texel: 7 point-fetches (Load = OpImageFetch —
    // no sampler filtering), the same slot math SgGiVolumeSample uses
    // (giVolumeLayers[vol * 7 + t]). The layer index rides a
    // wave-uniform loop counter, so the plain bindless index keeps the
    // descriptor fetch scalar.
    FrameUniforms frame = pb.frame.Get();
    float4 layers[SG_GI_VOLUME_LAYERS];
    [unroll]
    for (uint t = 0; t < SG_GI_VOLUME_LAYERS; ++t)
        layers[t] = volumeTextures[
            frame.giVolumeLayers[pb.volumeIndex * SG_GI_VOLUME_LAYERS + t]]
            .Load(int4(inst.grid, 0));
    float3 amps[SG_GI_LOBES];
    SgRegularUnpack(layers, amps);

    // The unit sphere direction IS the shading normal (pos == normal on
    // the icosphere). The color is the WHITE-LAMBERTIAN probe answer —
    // E(n) * giScale / PI, albedo 1 — the same quantity a matte white
    // surface at the probe's position renders as ambient (the lit body's
    // kD * baseColor * (E * giScale / PI) with baseColor = 1): the
    // spheres read as scene surfaces, not raw radiance (the first cut
    // showed E directly and the balls washed out ~PI x too white through
    // the tonemap - "too white, darker zones only grayer").
    // Interpolation across each triangle = the directional sweep.
    OUT.color = SgGiDiffuseIrradiance(amps, normalize(IN.position))
        * (max(frame.giIntensity, 0.0f) / 3.14159265f);
    OUT.pos = mul(pb.viewProj, float4(worldPos, 1.0));
    return OUT;
}
