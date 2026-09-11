#pragma once
// ---------------------------------------------------------------------------
// Texture-streaming GPU feedback (stage 2, opt-in).
//
// Skull & Bones style (GPU Zen 3, "GPU Readback Texture Streaming"): pixel
// shaders report the mip they actually needed; the CPU reads the buffer
// back every N frames and the streamer upgrades/downgrades residency.
//
// ABI (must match Engine/Renderer/texture_feedback.h):
//   * g_texFeedback[bindlessSlot] = min wanted full-chain mip, or 0xFF.
//   * Publish with InterlockedMin (sharpest demand wins).
//
// WIRING (not done yet – including this header alone changes nothing):
//   1. Global set layout gains binding 4 (read-write structured buffer).
//   2. A compute pass (or subpass epilogue) clears it to 0xFFFFFFFF.
//   3. Each material calls TS_ReportUsage(slot, uv) per sampled texture.
//   4. Frame loop calls TextureFeedback::ConsumeReadback(..., false)
//      every 4-8 frames.
//
// Until then the v1 CPU heuristic drives residency and no translation unit
// includes this file, so existing pipelines are unaffected.
// ---------------------------------------------------------------------------

#ifdef TEXTURE_STREAMING_FEEDBACK

[[vk::binding(4, 0)]]
RWStructuredBuffer<uint> g_texFeedback;

// Report the hardware LOD for `uv` on bindless texture `slot` sampled with
// `samp`. Call once per sampled texture per pixel; the atomic keeps the
// sharpest demand. `textureCount` is the texture's full mip count (for the
// clamp); pass the streamed fullMips, not the resident count.
void TS_ReportUsage(uint slot, Texture2D tex, SamplerState samp, float2 uv, uint textureCount)
{
    float lod = tex.CalculateLevelOfDetail(samp, uv);
    uint want = (uint)ceil(max(lod, 0.0f));
    want = min(want, textureCount > 0u ? textureCount - 1u : 0u);
    InterlockedMin(g_texFeedback[slot], want);
}

#endif // TEXTURE_STREAMING_FEEDBACK
