#pragma once

// Solid-pink error texture (255, 0, 255, 255): the visual flag for
// texture payloads that must never ride the render-thread transcode
// path. Two consumers share this:
//   * TextureStreamer::retarget — a streaming pak that still carries a
//     Basis-family payload (stale pak, or a cook whose transcode failed
//     before the v4 pink-embed fallback) refuses the synchronous
//     transcode and swaps the entry's backing image for a pink one
//     INSTEAD OF aborting the process (the error is reported loudly;
//     the pink surface is the "recook the pak" tell).
//   * CookTexturePayload — when a Basis source's BCn transcode (or the
//     transcoded write-back) fails at cook, the pink blob is embedded
//     so a streaming pak NEVER ships Basis-family bytes (the runtime
//     then streams pink cleanly instead of crashing on demand
//     upgrades).
// The mint mirrors the REFUSED/SOURCE texture's structure (dims/
// levels/faces/layers/depth) so every consumer of the original handle,
// view and bindless slot keeps working through the swap. Uncompressed
// RGBA8 by construction: ktxTexture2_NeedsTranscoding is always false.

#include <cstdint>
#include <vector>

struct ktxTexture2;

namespace smvk {

// In-memory form (the streamer's retarget swap feeds it straight into
// RetargetStreamingTexture's plan machinery). Returns null when the
// mint fails (caller logs and falls back to skipping the retarget).
ktxTexture2* CreatePinkKtx2(const ktxTexture2& mirror);

// Standalone form (the built-in "editor://error_texture" cluster asset,
// see assets::EnsureClusterErrorTexture): a plain 2D sRGB RGBA8 pink
// KTX2 with `levels` mips from the given base dims (levels = 0 derives
// the full chain). Returns null when the mint fails.
ktxTexture2* CreatePinkKtx2(uint32_t width, uint32_t height,
	uint32_t levels = 0);

// Serialized form (the cook embeds it into the pak payload). Returns
// false when the mint or serialization fails (caller keeps whatever
// fallback policy it has — for the cook that is failing loudly).
bool WritePinkKtx2Blob(const ktxTexture2& mirror, std::vector<uint8_t>& out);

} // namespace smvk
