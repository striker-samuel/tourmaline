#pragma once
// ---------------------------------------------------------------------------
// Texture-streaming GPU feedback (Skull & Bones style, GPU Zen 3).
//
// Pixel shaders report the mip they actually sampled per texture; the CPU
// reads the buffer back every 4th frame and the streamer upgrades or
// downgrades residency (see Engine/Renderer/texture_streamer.h +
// texture_feedback.h).
//
// ABI (must match texture_feedback.h):
//   * g_texFeedback[bindlessSlot] = min resident-relative LOD, or 0xFF.
//   * Published with InterlockedMin (sharpest demand wins). The shader
//     reports RESIDENT-relative LOD (level 0 == sharpest resident mip) so
//     no per-texture metadata is needed on the GPU; the CPU adds the
//     texture's residentBase to recover the full-chain mip.
//   * Decimation: only 1 in 4 pixels reports (pixel hash + frame parity).
//     Min-tracking stays exact in practice (a sharp demand covers many
//     pixels); atomic traffic drops 4x.
//
// Call once per sampled STREAMED texture per pixel, right after Sample().
// Post-process / rendertarget samples (tonemap, bloom, ...) must NOT
// report: those slots hold no streamed texture and the atomic would be
// pure overhead (unknown slots are ignored on consume, but still cost).
// ---------------------------------------------------------------------------

// Report the hardware LOD for `uv` on bindless texture `slot` sampled with
// `samp`. `pixPos` is SV_Position.xy (pixel coords) for decimation.
// `feedbackFrame` is true on 1 in 4 frames (cpu sets via textureFeedbackEnabled).
void TS_ReportUsage(
    uint slot,
    Texture2D tex,
    SamplerState samp,
    float2 uv,
    float2 pixPos,
    bool feedbackFrame)
{
    if (!feedbackFrame)
        return;

    // Executed with normal quad participation.
    float lod = tex.CalculateLevelOfDetail(samp, uv);
    uint want = (uint) ceil(max(lod, 0.0f));

    // One publisher per 2x2 quad.
    uint2 p = (uint2) pixPos;
    if (((p.x | p.y) & 1u) == 0u)
        InterlockedMin(g_texFeedback[slot], want);
}