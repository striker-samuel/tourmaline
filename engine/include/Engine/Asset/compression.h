#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace smvk {

// ---------------------------------------------------------------------------
// LZ4 blob compression for cooked paks.
//
// The pak stores opaque blobs (mesh / material / texture / scene). Large
// blobs — vertex/index payloads, embedded .ktx2 bytes — compress well with
// LZ4 (fast decompression, decent ratio, no runtime dependency beyond the
// vendored external/lz4 block codec).
//
// Wire format for a compressed blob (see pak.h):
//   u64 uncompressedSizeLE | LZ4 block bytes (LZ4_compress_fast output)
// The TOC entry's flags carry PakEntryFlag_Lz4, size is the stored size
// (8 + compressed bytes). Uncompressed blobs are stored verbatim with
// flags == PakEntryFlag_None, so v1/v2 paks keep working.
// ---------------------------------------------------------------------------

enum class PakCompression : uint8_t {
	None = 0,
	Lz4 = 1,
};

// Upper bound for a single blob accepted by the codec. LZ4 itself caps at
// 2GB (LZ4_MAX_INPUT_SIZE); we stay well below to keep decompression
// buffers sane and to reject corrupt size prefixes early.
inline constexpr size_t kMaxBlobBytes = 512u * 1024u * 1024u;

// Minimum raw size considered for compression. Tiny blobs (materials,
// small scenes) gain nothing and would grow due to the 8-byte prefix.
inline constexpr size_t kCompressMinBytes = 256;

// Maximum overhead of the LZ4 block format for an incompressible input.
int Lz4MaxCompressedSize(size_t srcSize);

// Compresses src with LZ4_compress_fast(acceleration). Returns false when
// src is empty/oversize or the output does not fit (should not happen when
// dst is sized via Lz4MaxCompressedSize). acceleration==1 is best ratio,
// higher values trade ratio for speed.
bool Lz4Compress(
	std::span<const uint8_t> src, std::vector<uint8_t>& outCompressed, int acceleration = 1);

// Decompresses an LZ4 block into out (resized to uncompressedSize).
// Returns false on malformed input, size mismatch, or oversize requests.
bool Lz4Decompress(
	std::span<const uint8_t> compressed, size_t uncompressedSize, std::vector<uint8_t>& out);

// Compresses a full pak blob payload into its stored form:
//   - returns {stored, true} when LZ4 shrinks the payload (stored = u64 LE
//     size prefix + compressed bytes),
//   - returns {raw copy, false} when compression is disabled, too small, or
//     not beneficial (caller stores verbatim with flags == None).
std::pair<std::vector<uint8_t>, bool> CompressPakBlob(
	std::span<const uint8_t> raw, PakCompression mode, int lz4Acceleration = 1);

// Inverse of CompressPakBlob for a stored blob with known flags.
// isCompressed mirrors PakEntryFlag_Lz4. Returns false on corrupt input.
bool DecompressPakBlob(
	std::span<const uint8_t> stored, bool isCompressed, std::vector<uint8_t>& outRaw);

} // namespace smvk
