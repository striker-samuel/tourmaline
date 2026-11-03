// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Project fragment shader: per-rect pixelation and toggleable scanlines.
// In GameUI the custom uint payload is carried in the unused 72-byte
// instance pad, forwarded by gameui.vert.hlsl as TEXCOORD3. Stock UI ignores it.
#define UI_PASS
#include "common.hlsli"
#include "gameui.hlsli"

struct UiPsIn {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 tint : COLOR0;
    nointerpolation float4 clipRect : TEXCOORD1;
    nointerpolation uint textureIndex : TEXCOORD2;
    nointerpolation uint effect : TEXCOORD3;
};

float4 mainPS(UiPsIn IN) : SV_Target0 {
    const float2 pixel = IN.position.xy;
    if (any(pixel < IN.clipRect.xy) || any(pixel >= IN.clipRect.zw))
        discard;

    float4 color = IN.tint;
    if (IN.textureIndex != 0xffffffffu) {
        float2 uv = IN.uv;
        // Sprite sample textures are 256x256; text atlas uses effect=0.
        // Snap to texel blocks *within* the source atlas, not frame pixels.
        // 0 = off; 2/4/8 = texels per sample (key 5 cycles).
        if (IN.effect >= 2u && IN.effect <= 8u) {
            const float block = float(IN.effect);
            const float2 texel = uv * 256.0f;
            uv = (floor(texel / block) * block + 0.5f*block) / 256.0f;
        }
        color *= textures[NonUniformResourceIndex(IN.textureIndex)]
            .Sample(samplers[NonUniformResourceIndex(gameUiPush.samplerIndex)], uv);
    } else if (IN.effect != 0u && IN.tint.a > 0.35f
        && IN.tint.a < 0.5f) {
        // Only the tinted translucent playfield, not opaque HUD strokes.
        const float scanline = (frac(pixel.y * 0.125f) < 0.5f) ? 0.76f : 1.0f;
        const float radial = saturate(length((IN.uv-0.5f) * 1.4142f));
        color.rgb *= scanline * lerp(1.12f, 0.72f, radial);
    }
    return color;
}
