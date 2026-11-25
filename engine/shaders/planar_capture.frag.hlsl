// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "light.hlsli"
#include "common.hlsli"

// Pairs with default.vert. No main-view tile lists, shadow maps, fog,
// specular, SSR, GI, or post-processing. Alpha is coverage, not opacity.
struct PSIn {
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
    [[vk::location(10)]] nointerpolation uint lightLayerMask : TEXCOORD9;
    bool front : SV_IsFrontFace;
};
float4 mainPS(PSIn IN) : SV_Target0 {
    MaterialValues mat = pc.bMats.Get().mats[IN.meshMaterial];
    // Layered materials use the dominant layer for a bounded sampling cost.
    bool layered = mat.layerMaterials.w > 1;
    if (layered) {
        float best = IN.color.x;
        int id = int(IN.meshMaterial);
        if (mat.layerMaterials.w > 1 && IN.color.y > best) {
            best = IN.color.y; id = mat.layerMaterials.x;
        }
        if (mat.layerMaterials.w > 2 && IN.color.z > best) {
            best = IN.color.z; id = mat.layerMaterials.y;
        }
        if (mat.layerMaterials.w > 3 && IN.color.w > best)
            id = mat.layerMaterials.z;
        if (id >= 0 && id < MAX_MATERIALS)
            mat = pc.bMats.Get().mats[id];
    }
    float4 base = mat.diffuse * (layered ? float4(1,1,1,1) : IN.color);
    if (mat.tex.x >= 0) {
        Texture2D t = textures[NonUniformResourceIndex(uint(mat.tex.x))];
        base *= t.Sample(samplers[0], IN.uv);
    }
    if (mat.params2.w == 2.0f)
        discard;
    if ((mat.params.w == 2.0f || mat.params2.w == 1.0f) && base.a < mat.params.x)
        discard;
    Light sun = pc.bLights.Get().lights[0];
    // Mirrored culling is flipped; do not derive normal sign from SV_IsFrontFace.
    float3 N = normalize(IN.normal);
    float3 L = -sun.direction * rsqrt(max(dot(sun.direction, sun.direction), 1e-8f));
    float3 energy = sun.color.rgb * sun.intensity;
    if ((asuint(sun.flags) & IN.lightLayerMask) == 0u)
        energy = 0;
    float3 emissive = mat.emissiveFactor.rgb * mat.params2.z;
    if (mat.tex2.x >= 0) {
        Texture2D t = textures[NonUniformResourceIndex(uint(mat.tex2.x))];
        emissive *= t.Sample(samplers[0], IN.uv).rgb;
    }
    float3 radiance = base.rgb * (mat.ambient.rgb +
        energy * (saturate(dot(N, L)) / 3.14159265f)) + emissive;
    return float4(SanitizeHdr(radiance), 1.0f);
}
