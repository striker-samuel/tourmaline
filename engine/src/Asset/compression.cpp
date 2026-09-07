#include <Engine/Asset/compression.h>

#include <cstring>
#include <limits>

extern "C" {
#include "lz4.h"
}

namespace smvk {

int Lz4MaxCompressedSize(size_t srcSize) {
	if (srcSize == 0 || srcSize > kMaxBlobBytes)
		return 0;
	if (srcSize > static_cast<size_t>((std::numeric_limits<int>::max)()))
		return 0;
	return LZ4_compressBound(static_cast<int>(srcSize));
}

bool Lz4Compress(std::span<const uint8_t> src, std::vector<uint8_t>& outCompressed, int acceleration) {
	outCompressed.clear();
	if (src.empty() || src.size() > kMaxBlobBytes)
		return false;
	if (acceleration < 1)
		acceleration = 1;
	const int srcSize = static_cast<int>(src.size());
	const int bound = LZ4_compressBound(srcSize);
	if (bound <= 0)
		return false;
	outCompressed.resize(static_cast<size_t>(bound));
	const int n = LZ4_compress_fast(
		reinterpret_cast<const char*>(src.data()),
		reinterpret_cast<char*>(outCompressed.data()),
		srcSize, bound, acceleration);
	if (n <= 0) {
		outCompressed.clear();
		return false;
	}
	outCompressed.resize(static_cast<size_t>(n));
	return true;
}

bool Lz4Decompress(
	std::span<const uint8_t> compressed, size_t uncompressedSize, std::vector<uint8_t>& out) {
	out.clear();
	if (uncompressedSize == 0 || uncompressedSize > kMaxBlobBytes)
		return false;
	if (compressed.empty() || compressed.size() > kMaxBlobBytes)
		return false;
	if (compressed.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
		uncompressedSize > static_cast<size_t>((std::numeric_limits<int>::max)()))
		return false;
	out.resize(uncompressedSize);
	const int n = LZ4_decompress_safe(
		reinterpret_cast<const char*>(compressed.data()),
		reinterpret_cast<char*>(out.data()),
		static_cast<int>(compressed.size()), static_cast<int>(uncompressedSize));
	if (n < 0 || static_cast<size_t>(n) != uncompressedSize) {
		out.clear();
		return false;
	}
	return true;
}

std::pair<std::vector<uint8_t>, bool> CompressPakBlob(
	std::span<const uint8_t> raw, PakCompression mode, int lz4Acceleration) {
	if (mode != PakCompression::Lz4 || raw.size() < kCompressMinBytes || raw.size() > kMaxBlobBytes)
		return { std::vector<uint8_t>(raw.begin(), raw.end()), false };
	std::vector<uint8_t> comp;
	if (!Lz4Compress(raw, comp, lz4Acceleration))
		return { std::vector<uint8_t>(raw.begin(), raw.end()), false };
	if (comp.size() + sizeof(uint64_t) >= raw.size())
		return { std::vector<uint8_t>(raw.begin(), raw.end()), false }; // not beneficial
	std::vector<uint8_t> stored;
	stored.reserve(sizeof(uint64_t) + comp.size());
	const uint64_t rawSize = static_cast<uint64_t>(raw.size());
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&rawSize);
	stored.insert(stored.end(), p, p + sizeof(uint64_t));
	stored.insert(stored.end(), comp.begin(), comp.end());
	return { std::move(stored), true };
}

bool DecompressPakBlob(std::span<const uint8_t> stored, bool isCompressed, std::vector<uint8_t>& outRaw) {
	outRaw.clear();
	if (!isCompressed) {
		outRaw.assign(stored.begin(), stored.end());
		return true;
	}
	if (stored.size() < sizeof(uint64_t) + 1)
		return false;
	uint64_t rawSize = 0;
	memcpy(&rawSize, stored.data(), sizeof(uint64_t));
	if (rawSize < kCompressMinBytes || rawSize > kMaxBlobBytes)
		return false;
	if (stored.size() - sizeof(uint64_t) >= rawSize)
		return false; // stored form must be smaller, else corrupt/pointless
	return Lz4Decompress(stored.subspan(sizeof(uint64_t)), static_cast<size_t>(rawSize), outRaw);
}

} // namespace smvk
