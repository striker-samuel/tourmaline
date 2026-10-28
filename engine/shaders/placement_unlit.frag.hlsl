// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_unlit.frag.hlsl — the DIAGNOSTIC fragment for the placement
// + mesh-particle draw paths: the flat per-instance tint (the placement
// VS's white / the mesh-particle VS's spawn tint) with a cheap
// hemisphere-ish normal shade so orientation reads on screen. The
// production path resolves the material system's lit fragments through
// the same interface (the placement VS keeps the standard scene push
// intact, so every existing fragment family works); this one exists so
// the draw chain is independently provable end to end (pixels on
// device) without a material binding.

struct PSIn
{
    float4 pos : SV_Position;

    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
};

float4 mainPS(PSIn IN) : SV_Target0
{
    // Cheap sky/ground shading of the tint — orientation visible, no
    // lighting state needed.
    const float shade = 0.55 + 0.45 * saturate(IN.normal.y * 0.5 + 0.5);
    return float4(IN.color * shade, 1.0);
}
