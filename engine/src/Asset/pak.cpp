// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/pak.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <Engine/Asset/compression.h>
#include <Engine/Utils/thread_pool.h>

namespace smvk {
namespace {

static_assert(std::endian::native == std::endian::little,
	"Pak format is little-endian; add byte-swap helpers for big-endian hosts");

inline constexpr size_t kHeaderSize = 32;
inline constexpr size_t kFooterSize = 24;
inline constexpr size_t kTocEntryFixed = 8 + 4 + 4 + 8 + 8 + 4; // hash,kind,flags,off,size,pathLen
inline constexpr size_t kAlign = 16;

// Images below this stay on the serial CRC/decompress paths (pool spin-up
// would cost more than the work; tests and small appends land here).
inline constexpr size_t kCrcParallelMinBytes = 32ull * 1024ull * 1024ull;

size_t AlignUp(size_t v, size_t align) {
	return (v + align - 1) & ~(align - 1);
}

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 4);
}

void AppendU64(std::vector<uint8_t>& out, uint64_t v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 8);
}

// --- CRC-32 combine machinery (zlib crc32_combine style) ------------------
//
// CRC-32 is linear over GF(2): the CRC of A||B can be rebuilt from the two
// part CRCs by running crc1's register through an operator that models
// "process len2 zero bytes". That operator is a 32x32 GF(2) matrix; powers
// of two come from squaring, and len2's bits pick which to apply.
constexpr int kCrcGf2Dim = 32;

struct CrcGf2Matrix {
	uint32_t rows[kCrcGf2Dim];
};

uint32_t CrcGf2MatrixTimes(const CrcGf2Matrix& mat, uint32_t vec) {
	uint32_t sum = 0;
	int row = 0;
	while (vec) {
		if (vec & 1u)
			sum ^= mat.rows[row];
		vec >>= 1;
		++row;
	}
	return sum;
}

void CrcGf2MatrixSquare(CrcGf2Matrix& square, const CrcGf2Matrix& mat) {
	for (int n = 0; n < kCrcGf2Dim; ++n)
		square.rows[n] = CrcGf2MatrixTimes(mat, mat.rows[n]);
}

} // namespace

uint32_t PakCrc32Combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
	if (len2 == 0)
		return crc1;
	CrcGf2Matrix even{};
	CrcGf2Matrix odd{};
	// Operator for one zero bit: row 0 = reflected polynomial, row n holds
	// bit n-1 shifted toward the register's high end.
	odd.rows[0] = 0xEDB88320u;
	uint32_t row = 1;
	for (int n = 1; n < kCrcGf2Dim; ++n) {
		odd.rows[n] = row;
		row <<= 1;
	}
	CrcGf2MatrixSquare(even, odd); // two zero bits
	CrcGf2MatrixSquare(odd, even); // four zero bits
	// Walk len2's bits; the first squaring reaches one zero BYTE, so the
	// bit positions of the byte count len2 pick byte-power operators.
	do {
		CrcGf2MatrixSquare(even, odd);
		if (len2 & 1)
			crc1 = CrcGf2MatrixTimes(even, crc1);
		len2 >>= 1;
		if (len2 == 0)
			break;
		CrcGf2MatrixSquare(odd, even);
		if (len2 & 1)
			crc1 = CrcGf2MatrixTimes(odd, crc1);
		len2 >>= 1;
	} while (len2 != 0);
	return crc1 ^ crc2;
}

uint32_t PakCrc32Parallel(std::span<const uint8_t> data, size_t chunkBytes) {
	if (chunkBytes == 0)
		chunkBytes = 1;
	if (data.size() <= chunkBytes)
		return PakCrc32(data.data(), data.size());
	// Chunk-split, one CRC per worker, then fold right-to-left with the
	// combine primitive (acc always covers [i+1, chunkCount)).
	struct ChunkCrc {
		uint32_t crc;
		size_t len;
	};
	const size_t chunkCount = (data.size() + chunkBytes - 1) / chunkBytes;
	ThreadPool pool(std::min<size_t>(chunkCount,
		std::thread::hardware_concurrency()));
	std::vector<std::future<ChunkCrc>> futs;
	futs.reserve(chunkCount);
	for (size_t i = 0; i < chunkCount; ++i) {
		const uint8_t* base = data.data() + i * chunkBytes;
		const size_t len = std::min(chunkBytes, data.size() - i * chunkBytes);
		futs.push_back(pool.enqueue([base, len]() -> ChunkCrc {
			return ChunkCrc{ PakCrc32(base, len), len };
		}));
	}
	ChunkCrc acc = futs.back().get();
	futs.pop_back();
	while (!futs.empty()) {
		const ChunkCrc head = futs.back().get();
		futs.pop_back();
		acc.crc = PakCrc32Combine(head.crc, acc.crc, acc.len);
		acc.len += head.len;
	}
	return acc.crc;
}

uint64_t PakHashPath(const std::string& normalizedPath) {
	// FNV-1a 64.
	uint64_t h = 14695981039346656037ull;
	for (unsigned char c : normalizedPath) {
		h ^= c;
		h *= 1099511628211ull;
	}
	return h;
}

uint32_t PakCrc32(const void* data, size_t size) {
	static uint32_t table[256];
	static bool init = false;
	if (!init) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		init = true;
	}
	uint32_t crc = 0xFFFFFFFFu;
	const uint8_t* p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; ++i)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

// Forward: v4 payload parse needs the digest validator defined below.
bool ParseTextureDigest(std::span<const uint8_t> digest, TextureStreamMeta& out);

bool ParseTexturePayload(std::span<const uint8_t> payload, PakTexturePayload& out) {
	size_t off = 0;
	uint32_t uriLen = 0;
	if (payload.size() < 4)
		return false;
	memcpy(&uriLen, payload.data(), 4);
	off = 4;
	if (uriLen > 64 * 1024 || off + uriLen + 16 + 8 > payload.size())
		return false;
	out.uri.assign(reinterpret_cast<const char*>(payload.data() + off), uriLen);
	off += uriLen;
	int32_t samp[4];
	memcpy(samp, payload.data() + off, 16);
	off += 16;
	// Shape A (v2/v3): u64 ktxSize | ktx bytes, end-exact.
	uint64_t ktxSize = 0;
	memcpy(&ktxSize, payload.data() + off, 8);
	if (off + 8 + ktxSize == payload.size() && ktxSize > 0) {
		out.magFilter = samp[0];
		out.minFilter = samp[1];
		out.wrapS = samp[2];
		out.wrapT = samp[3];
		out.digest = {};
		out.ktxBytes = payload.subspan(off + 8, static_cast<size_t>(ktxSize));
		return true;
	}
	// Shape B (v4 digest-less): u32 0 | u64 ktxSize | ktx bytes, end-exact.
	uint32_t dlen = 0;
	memcpy(&dlen, payload.data() + off, 4);
	if (dlen == 0 && off + 12 <= payload.size()) {
		memcpy(&ktxSize, payload.data() + off + 4, 8);
		if (off + 12 + ktxSize == payload.size() && ktxSize > 0) {
			out.magFilter = samp[0];
			out.minFilter = samp[1];
			out.wrapS = samp[2];
			out.wrapT = samp[3];
			out.digest = {};
			out.ktxBytes = payload.subspan(off + 12, static_cast<size_t>(ktxSize));
			return true;
		}
	}
	return false;
}

bool ParseTexturePayloadV4(std::span<const uint8_t> payload, PakTexturePayload& out) {
	size_t off = 0;
	uint32_t uriLen = 0;
	if (payload.size() < 4)
		return false;
	memcpy(&uriLen, payload.data(), 4);
	off = 4;
	if (uriLen > 64 * 1024 || off + uriLen + 16 + 4 > payload.size())
		return false;
	uint32_t dlen = 0;
	memcpy(&dlen, payload.data() + off + uriLen + 16, 4);
	if (dlen == 0 || dlen > 1024 * 1024)
		return false;
	if (off + uriLen + 16 + 4 + dlen + 8 > payload.size())
		return false;
	TextureStreamMeta probe;
	if (!ParseTextureDigest(
			payload.subspan(off + uriLen + 16 + 4, static_cast<size_t>(dlen)), probe))
		return false;
	uint64_t ktxSize = 0;
	memcpy(&ktxSize, payload.data() + off + uriLen + 16 + 4 + dlen, 8);
	if (ktxSize == 0
		|| off + uriLen + 16 + 4 + dlen + 8 + ktxSize != payload.size())
		return false;
	out.uri.assign(reinterpret_cast<const char*>(payload.data() + off), uriLen);
	off += uriLen;
	int32_t samp[4];
	memcpy(samp, payload.data() + off, 16);
	off += 16;
	out.magFilter = samp[0];
	out.minFilter = samp[1];
	out.wrapS = samp[2];
	out.wrapT = samp[3];
	off += 4;
	out.digest = payload.subspan(off, static_cast<size_t>(dlen));
	off += dlen + 8;
	out.ktxBytes = payload.subspan(off, static_cast<size_t>(ktxSize));
	return true;
}

std::vector<uint8_t> BuildTexturePayload(
	const std::string& uri, int32_t magFilter, int32_t minFilter,
	int32_t wrapS, int32_t wrapT, std::span<const uint8_t> ktxBytes) {
	std::vector<uint8_t> out;
	AppendU32(out, static_cast<uint32_t>(uri.size()));
	out.insert(out.end(), uri.begin(), uri.end());
	const int32_t samp[4] = { magFilter, minFilter, wrapS, wrapT };
	const uint8_t* sp = reinterpret_cast<const uint8_t*>(samp);
	out.insert(out.end(), sp, sp + 16);
	AppendU64(out, static_cast<uint64_t>(ktxBytes.size()));
	out.insert(out.end(), ktxBytes.begin(), ktxBytes.end());
	return out;
}

std::vector<uint8_t> BuildTexturePayloadV4(
	const std::string& uri, int32_t magFilter, int32_t minFilter,
	int32_t wrapS, int32_t wrapT, std::span<const uint8_t> ktxBytes,
	std::span<const uint8_t> digest) {
	std::vector<uint8_t> out;
	AppendU32(out, static_cast<uint32_t>(uri.size()));
	out.insert(out.end(), uri.begin(), uri.end());
	const int32_t samp[4] = { magFilter, minFilter, wrapS, wrapT };
	const uint8_t* sp = reinterpret_cast<const uint8_t*>(samp);
	out.insert(out.end(), sp, sp + 16);
	AppendU32(out, static_cast<uint32_t>(digest.size()));
	out.insert(out.end(), digest.begin(), digest.end());
	AppendU64(out, static_cast<uint64_t>(ktxBytes.size()));
	out.insert(out.end(), ktxBytes.begin(), ktxBytes.end());
	return out;
}

// --- tail-upload digest (TextureStreamMeta serialization) ----------------
// Minimal by design: kind(=1) + tailFirstMip only. The loader derives
// everything else (format, extents, regions) from the live ktx object it
// already parsed for the transcode, so the digest can never disagree
// with the bytes it describes.
namespace {
constexpr uint32_t kTextureDigestKind = 1;
constexpr size_t kTextureDigestFixed = 4 + 4; // kind, tailFirstMip

void AppendDigestU32(std::vector<uint8_t>& out, uint32_t v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 4);
}
} // namespace

bool BuildTextureDigest(const TextureStreamMeta& meta, std::vector<uint8_t>& out) {
	out.clear();
	AppendDigestU32(out, kTextureDigestKind);
	AppendDigestU32(out, meta.tailFirstMip);
	return true;
}

bool ParseTextureDigest(std::span<const uint8_t> digest, TextureStreamMeta& out) {
	if (digest.size() != kTextureDigestFixed)
		return false;
	uint32_t kind = 0, firstMip = 0;
	memcpy(&kind, digest.data(), 4);
	memcpy(&firstMip, digest.data() + 4, 4);
	if (kind != kTextureDigestKind)
		return false;
	out = TextureStreamMeta{};
	out.tailFirstMip = firstMip;
	return true;
}

// ---------------------------------------------------------------------------
// PakWriter
// ---------------------------------------------------------------------------

bool PakWriter::AddBlob(PakAssetKind kind, const std::string& path, std::vector<uint8_t> payload,
	bool storeUncompressed) {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty() || payload.empty())
		return false;
	Pending p;
	p.meta.pathHash = PakHashPath(n);
	p.meta.kind = kind;
	p.meta.flags = PakEntryFlag_None;
	p.meta.path = n;
	if (compression_ == PakCompression::Lz4 && !storeUncompressed) {
		auto [stored, compressed] = CompressPakBlob(
			std::span<const uint8_t>(payload), PakCompression::Lz4, lz4Acceleration_);
		if (compressed) {
			p.meta.flags = PakEntryFlag_Lz4;
			p.payload = std::move(stored);
		}
		else {
			p.payload = std::move(payload);
		}
	}
	else {
		p.payload = std::move(payload);
	}
	metas_.push_back(p.meta);
	entries_.push_back(std::move(p));
	return true;
}

size_t PakWriter::CompressedEntryCount() const {
	size_t n = 0;
	for (const auto& e : entries_) {
		if (e.meta.flags & PakEntryFlag_Lz4)
			++n;
	}
	return n;
}

std::vector<uint8_t> PakWriter::Finalize() const {
	if (entries_.empty())
		throw std::runtime_error("PakWriter: no entries");

	std::vector<uint8_t> out;
	out.resize(kHeaderSize, 0); // patched below

	// Blobs (16-aligned).
	for (auto& e : entries_) {
		const size_t aligned = AlignUp(out.size(), kAlign);
		out.insert(out.end(), aligned - out.size(), 0);
		const_cast<PakTocEntry&>(e.meta).offset = out.size();
		const_cast<PakTocEntry&>(e.meta).size = e.payload.size();
		out.insert(out.end(), e.payload.begin(), e.payload.end());
	}

	// TOC.
	const size_t tocOffset = AlignUp(out.size(), kAlign);
	out.insert(out.end(), tocOffset - out.size(), 0);
	for (const auto& e : entries_) {
		AppendU64(out, e.meta.pathHash);
		AppendU32(out, static_cast<uint32_t>(e.meta.kind));
		AppendU32(out, e.meta.flags);
		AppendU64(out, e.meta.offset);
		AppendU64(out, e.meta.size);
		AppendU32(out, static_cast<uint32_t>(e.meta.path.size()));
		out.insert(out.end(), e.meta.path.begin(), e.meta.path.end());
	}

	// Footer: tocOffset, tocCount, crc32 over [0, footerCrcOff), footer magic.
	// NOTE: the header placeholder is patched *before* the CRC so the
	// checksum covers the final header bytes. No version field anywhere:
	// the magic IS the version (a breaking format change bumps it and old
	// paks fail closed at Open).
	const uint64_t tocCount = entries_.size();
	{
		uint8_t* h = out.data();
		memcpy(h + 0, &kPakMagic, 4);
		const uint32_t reserved = 0;
		memcpy(h + 4, &reserved, 4);
		const uint32_t flags = 0;
		memcpy(h + 8, &flags, 4);
		memcpy(h + 12, &tocOffset, 8);
		memcpy(h + 20, &tocCount, 8);
		// bytes [28,32) reserved.
	}
	AppendU64(out, static_cast<uint64_t>(tocOffset));
	AppendU64(out, tocCount);
	const uint32_t crc = PakCrc32(out.data(), out.size());
	AppendU32(out, crc);
	AppendU32(out, kPakFooterMagic);

	if (out.size() < kHeaderSize + kFooterSize)
		throw std::runtime_error("PakWriter: image too small");
	return out;
}

// ---------------------------------------------------------------------------
// AppendPakBlobs (scratch store growth; see pak.h for the contract)
// ---------------------------------------------------------------------------

namespace {

// TOC entry count/size helpers shared by the append path.
size_t TocEntryBytes(const std::vector<PakTocEntry>& entries) {
	size_t n = 0;
	for (const PakTocEntry& e : entries)
		n += kTocEntryFixed + e.path.size();
	return n;
}

void WriteTocEntry(std::vector<uint8_t>& out, const PakTocEntry& e) {
	AppendU64(out, e.pathHash);
	AppendU32(out, static_cast<uint32_t>(e.kind));
	AppendU32(out, e.flags);
	AppendU64(out, e.offset);
	AppendU64(out, e.size);
	AppendU32(out, static_cast<uint32_t>(e.path.size()));
	out.insert(out.end(), e.path.begin(), e.path.end());
}

// Rolling reflected CRC-32 (same table/step as PakCrc32, unfinalized):
// the footer's first 16 bytes (tocOffset + tocCount) are COVERED by
// the pak CRC but not on disk until the footer write — the caller
// streams the file range, then feeds those bytes from memory.
// Caller seeds crc with 0xFFFFFFFF, finalizes with ^0xFFFFFFFF.
bool Crc32FeedFile(std::istream& f, uint64_t end, uint32_t& crc) {
	static uint32_t table[256];
	static bool init = false;
	if (!init) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		init = true;
	}
	std::vector<uint8_t> buf(1 << 20);
	uint64_t pos = 0;
	f.clear();
	f.seekg(0, std::ios::beg);
	while (pos < end) {
		const uint64_t want = std::min<uint64_t>(end - pos, buf.size());
		f.read(reinterpret_cast<char*>(buf.data()),
			static_cast<std::streamsize>(want));
		if (f.gcount() != static_cast<std::streamsize>(want))
			return false;
		for (uint64_t i = 0; i < want; ++i)
			crc = table[(crc ^ buf[size_t(i)]) & 0xFF] ^ (crc >> 8);
		pos += want;
	}
	return true;
}

void Crc32FeedBytes(const void* data, size_t size, uint32_t& crc) {
	static uint32_t table[256];
	static bool init = false;
	if (!init) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		init = true;
	}
	const uint8_t* p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; ++i)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
}

} // namespace

bool AppendPakBlobs(const std::string& pakPath, PakAssetKind kind,
	const std::vector<std::pair<std::string,
		std::span<const uint8_t>>>& blobs,
	std::vector<PakAppendInfo>& out, std::string& err) {
	out.clear();
	if (blobs.empty()) {
		err = "no blobs to append";
		return false;
	}
	// Normalize + validate up front (fail before touching the file).
	std::vector<PakTocEntry> newEntries;
	newEntries.reserve(blobs.size());
	for (const auto& [rawUri, payload] : blobs) {
		PakTocEntry e;
		e.kind = kind;
		e.flags = 0; // uncompressed: direct (offset, size) reads
		e.path = vfs::Normalize(rawUri);
		if (e.path.empty() || payload.empty()) {
			err = "invalid append blob uri/payload: " + rawUri;
			return false;
		}
		e.pathHash = PakHashPath(e.path);
		e.size = payload.size();
		newEntries.push_back(std::move(e));
	}

	std::error_code fec;
	std::filesystem::create_directories(
		std::filesystem::path(pakPath).parent_path(), fec);
	// in|out|binary: read header+TOC, then append. The in flag makes
	// plain open fail on a MISSING file (POSIX "r+"); create it first
	// (trunc is safe — there is nothing to lose) and reopen.
	std::fstream f(pakPath,
		std::ios::in | std::ios::out | std::ios::binary);
	if (!f) {
		{
			std::ofstream create(pakPath,
				std::ios::out | std::ios::binary | std::ios::trunc);
			if (!create) {
				err = "cannot create pak: " + pakPath;
				return false;
			}
		}
		f.open(pakPath,
			std::ios::in | std::ios::out | std::ios::binary);
		if (!f) {
			err = "cannot open pak for append: " + pakPath;
			return false;
		}
	}
	f.seekg(0, std::ios::end);
	const auto fileSize = uint64_t(f.tellg());

	// Existing TOC (empty for a fresh file).
	std::vector<PakTocEntry> entries;
	uint64_t payloadBase = kHeaderSize; // fresh: payloads right after header
	if (fileSize >= kHeaderSize + kFooterSize) {
		f.seekg(0, std::ios::beg);
		uint32_t magic = 0, reserved = 0;
		uint64_t tocOffset = 0, tocCount = 0;
		f.read(reinterpret_cast<char*>(&magic), 4);
		f.read(reinterpret_cast<char*>(&reserved), 4);
		f.seekg(4, std::ios::cur); // flags
		f.read(reinterpret_cast<char*>(&tocOffset), 8);
		f.read(reinterpret_cast<char*>(&tocCount), 8);
		if (!f || magic != kPakMagic
			|| tocOffset + kTocEntryFixed * tocCount > fileSize) {
			err = "existing pak unreadable: " + pakPath;
			return false;
		}
		f.seekg(std::streamoff(tocOffset), std::ios::beg);
		for (uint64_t i = 0; i < tocCount && f; ++i) {
			PakTocEntry e;
			uint32_t kindRaw = 0, pathLen = 0;
			f.read(reinterpret_cast<char*>(&e.pathHash), 8);
			f.read(reinterpret_cast<char*>(&kindRaw), 4);
			f.read(reinterpret_cast<char*>(&e.flags), 4);
			f.read(reinterpret_cast<char*>(&e.offset), 8);
			f.read(reinterpret_cast<char*>(&e.size), 8);
			f.read(reinterpret_cast<char*>(&pathLen), 4);
			if (!f || pathLen > 4096) {
				err = "pak TOC unreadable: " + pakPath;
				return false;
			}
			std::string path(size_t(pathLen), '\0');
			f.read(path.data(), std::streamsize(pathLen));
			e.kind = static_cast<PakAssetKind>(kindRaw);
			e.path = std::move(path);
			entries.push_back(std::move(e));
		}
		if (!f) {
			err = "pak TOC truncated: " + pakPath;
			return false;
		}
		// New payloads land AFTER every live old byte (old TOC+footer
		// included): an interrupted append keeps the old pak valid.
		payloadBase = AlignUp(size_t(tocOffset) + TocEntryBytes(entries)
			+ kFooterSize, kAlign);
	}

	// Write payloads + collect their (offset, size).
	uint64_t cursor = payloadBase;
	for (size_t i = 0; i < newEntries.size(); ++i) {
		const size_t aligned = AlignUp(size_t(cursor), kAlign);
		if (aligned > cursor) {
			std::vector<char> pad(aligned - size_t(cursor), 0);
			f.write(pad.data(), std::streamsize(pad.size()));
		}
		cursor = aligned;
		f.seekp(std::streamoff(cursor), std::ios::beg);
		f.write(reinterpret_cast<const char*>(
				blobs[i].second.data()),
			std::streamsize(blobs[i].second.size()));
		if (!f) {
			err = "payload write failed: " + pakPath;
			return false;
		}
		newEntries[i].offset = cursor;
		newEntries[i].size = blobs[i].second.size();
		cursor += blobs[i].second.size();
		out.push_back(PakAppendInfo{ newEntries[i].offset,
			newEntries[i].size });
	}

	// New TOC: old entries with duplicates replaced, then the new ones
	// (first-match Find keeps resolving the latest registration).
	for (const PakTocEntry& ne : newEntries) {
		bool replaced = false;
		for (PakTocEntry& e : entries) {
			if (e.path == ne.path && e.kind == ne.kind) {
				e = ne; // supersede: old payload turns dead space
				replaced = true;
				break;
			}
		}
		if (!replaced)
			entries.push_back(ne);
	}
	const size_t newTocOffset = AlignUp(size_t(cursor), kAlign);
	{
		std::vector<uint8_t> toc;
		toc.reserve(TocEntryBytes(entries));
		for (const PakTocEntry& e : entries)
			WriteTocEntry(toc, e);
		f.seekp(std::streamoff(newTocOffset), std::ios::beg);
		f.write(reinterpret_cast<const char*>(toc.data()),
			std::streamsize(toc.size()));
		if (!f) {
			err = "TOC write failed: " + pakPath;
			return false;
		}
	}
	// Patch the header LAST (see pak.h: interrupted appends keep the
	// previous valid contents). No version field: the magic IS the
	// version, and appended blobs never change the format.
	f.seekp(0, std::ios::beg);
	f.write(reinterpret_cast<const char*>(&kPakMagic), 4);
	{
		const uint32_t reserved = 0;
		f.write(reinterpret_cast<const char*>(&reserved), 4);
		const uint32_t flags = 0;
		f.write(reinterpret_cast<const char*>(&flags), 4);
		f.write(reinterpret_cast<const char*>(&newTocOffset), 8);
		const auto count = uint64_t(entries.size());
		f.write(reinterpret_cast<const char*>(&count), 8);
	}
	f.flush();
	if (!f) {
		err = "header patch failed: " + pakPath;
		return false;
	}
	// Footer: the pak CRC covers [0, footerCrcOff) = file bytes plus the
	// footer's leading (tocOffset, tocCount) — stream the file, then
	// feed those 16 bytes from memory before finalizing.
	const uint64_t footerAt = uint64_t(newTocOffset)
		+ TocEntryBytes(entries);
	uint32_t crc = 0xFFFFFFFFu;
	if (!Crc32FeedFile(f, footerAt, crc)) {
		err = "crc stream failed: " + pakPath;
		return false;
	}
	const auto count = uint64_t(entries.size());
	{
		uint8_t footerHead[16];
		memcpy(footerHead + 0, &newTocOffset, 8);
		memcpy(footerHead + 8, &count, 8);
		Crc32FeedBytes(footerHead, sizeof(footerHead), crc);
	}
	crc ^= 0xFFFFFFFFu;
	f.seekp(std::streamoff(footerAt), std::ios::beg);
	f.write(reinterpret_cast<const char*>(&newTocOffset), 8);
	f.write(reinterpret_cast<const char*>(&count), 8);
	f.write(reinterpret_cast<const char*>(&crc), 4);
	f.write(reinterpret_cast<const char*>(&kPakFooterMagic), 4);
	const auto finalSize = footerAt + kFooterSize;
	f.flush();
	f.close();
	std::filesystem::resize_file(pakPath, finalSize, fec);
	return true;
}

// ---------------------------------------------------------------------------
// PakReader
// ---------------------------------------------------------------------------

std::optional<PakReader> PakReader::Open(std::span<const uint8_t> image, std::string& outError) {
	if (image.size() < kHeaderSize + kFooterSize) {
		outError = "pak too small";
		return std::nullopt;
	}
	uint32_t magic = 0;
	uint64_t tocOffset = 0, tocCount = 0;
	memcpy(&magic, image.data() + 0, 4);
	memcpy(&tocOffset, image.data() + 12, 8);
	memcpy(&tocCount, image.data() + 20, 8);
	bool legacyEpoch = false;
	if (magic != kPakMagic) {
		if (magic != kPakMagicSmp5) {
			outError = "bad pak magic (stale pak - recook from zero)";
			return std::nullopt;
		}
		// LEGACY HACK (SMP5 without terrain): old bistro/sponza paks keep
		// loading with empty terrain. The footer + per-entry validation
		// below run unchanged; after the TOC parse any Terrain blob fails
		// closed (an SMP5 image must never claim terrain payloads).
		legacyEpoch = true;
	}

	// Footer check.
	uint64_t fTocOffset = 0, fTocCount = 0;
	uint32_t fCrc = 0, fMagic = 0;
	const size_t fOff = image.size() - kFooterSize;
	memcpy(&fTocOffset, image.data() + fOff + 0, 8);
	memcpy(&fTocCount, image.data() + fOff + 8, 8);
	memcpy(&fCrc, image.data() + fOff + 16, 4);
	memcpy(&fMagic, image.data() + fOff + 20, 4);
	const uint32_t wantFooter = legacyEpoch ? kPakFooterMagicKpm5 : kPakFooterMagic;
	if (fMagic != wantFooter) {
		outError = "bad pak footer magic";
		return std::nullopt;
	}
	if (fTocOffset != tocOffset || fTocCount != tocCount) {
		outError = "pak header/footer TOC mismatch";
		return std::nullopt;
	}
	// Whole-image CRC dominates startup for multi-GB paks (the table walk
	// is ~1 GB/s serial); big images verify chunk-parallel instead.
	const size_t crcBytes = fOff + 16;
	const uint32_t crc = (crcBytes >= kCrcParallelMinBytes)
		? PakCrc32Parallel(std::span<const uint8_t>(image.data(), crcBytes))
		: PakCrc32(image.data(), crcBytes);
	if (crc != fCrc) {
		outError = "pak CRC mismatch (corrupt or truncated)";
		return std::nullopt;
	}

	// Parse TOC.
	PakReader r;
	r.image_ = image;
	size_t off = static_cast<size_t>(tocOffset);
	for (uint64_t i = 0; i < tocCount; ++i) {
		if (off + kTocEntryFixed > image.size()) {
			outError = "pak TOC overruns image";
			return std::nullopt;
		}
		PakTocEntry e;
		uint32_t kind = 0, pathLen = 0;
		memcpy(&e.pathHash, image.data() + off, 8);
		memcpy(&kind, image.data() + off + 8, 4);
		memcpy(&e.flags, image.data() + off + 12, 4);
		memcpy(&e.offset, image.data() + off + 16, 8);
		memcpy(&e.size, image.data() + off + 24, 8);
		memcpy(&pathLen, image.data() + off + 32, 4);
		off += kTocEntryFixed;
		if (pathLen > 4096 || off + pathLen > image.size()) {
			outError = "pak TOC path overruns image";
			return std::nullopt;
		}
		e.kind = static_cast<PakAssetKind>(kind);
		e.path.assign(reinterpret_cast<const char*>(image.data() + off), pathLen);
		off += pathLen;
		if (e.offset + e.size > tocOffset) {
			outError = "pak blob overruns TOC: " + e.path;
			return std::nullopt;
		}
		if (e.pathHash != PakHashPath(e.path)) {
			outError = "pak path hash mismatch: " + e.path;
			return std::nullopt;
		}
		if (e.flags & ~PakEntryFlag_Lz4) {
			outError = "pak unknown entry flags: " + e.path;
			return std::nullopt;
		}
		r.entries_.push_back(std::move(e));
	}
	if (legacyEpoch) {
		for (const auto& e : r.entries_) {
			if (e.kind == PakAssetKind::Terrain) {
				outError = "legacy SMP5 pak claims Terrain blobs (recook from zero)";
				return std::nullopt;
			}
		}
	}
	// LAZY LZ4 decompression (finding #18): Open only parses + CRCs the
	// image — compressed entries decompress on FIRST Payload() access
	// (see Payload below) into reader-owned, lifetime-cached buffers.
	// The old eager pass decompressed EVERY compressed entry at Open —
	// a viewer that needs one scene out of a multi-GB pak paid for all
	// of them before the first frame, and a torn LZ4 block surfaced at
	// boot instead of at the entry that needs it.
	// The lazily-decompressed payloads are cached for the reader's
	// LIFETIME (Payload() hands out spans into decompressed_ — an
	// eviction-based bounded cache would invalidate live spans; the
	// cache is therefore bounded by "everything actually read", which
	// is <= what the eager path materialized, always).
	r.decompressed_.assign(r.entries_.size(), {});
	r.decompressDone_.assign(r.entries_.size(), 0);
	r.lazyMutex_ = std::make_unique<std::mutex>();
	return r;
}

size_t PakReader::CompressedEntryCount() const {
	size_t n = 0;
	for (const auto& e : entries_) {
		if (e.flags & PakEntryFlag_Lz4)
			++n;
	}
	return n;
}

std::vector<const PakTocEntry*> PakReader::BlobsOfKind(PakAssetKind kind) const {
	std::vector<const PakTocEntry*> out;
	for (const auto& e : entries_) {
		if (e.kind == kind)
			out.push_back(&e);
	}
	return out;
}

const PakTocEntry* PakReader::Find(PakAssetKind kind, const std::string& normalizedPath) const {
	const uint64_t h = PakHashPath(normalizedPath);
	for (const auto& e : entries_) {
		if (e.kind == kind && e.pathHash == h && e.path == normalizedPath)
			return &e;
	}
	return nullptr;
}

std::span<const uint8_t> PakReader::Payload(const PakTocEntry& entry) const {
	// Entries are only ever accessed via pointers into entries_ (Find /
	// BlobsOfKind), so pointer arithmetic recovers the owned buffer.
	const size_t idx = static_cast<size_t>(&entry - entries_.data());
	if (entry.flags & PakEntryFlag_Lz4) {
		// Lazy decompress-on-first-use (see Open): guarded so concurrent
		// first reads (the preload thread + the render thread sharing a
		// backend) decompress exactly once; the result stays cached for
		// the reader's lifetime — the returned span never invalidates.
		std::lock_guard<std::mutex> lock(*lazyMutex_);
		if (!decompressDone_[idx]) {
			const std::span<const uint8_t> stored = image_.subspan(
				static_cast<size_t>(entry.offset),
				static_cast<size_t>(entry.size));
			std::vector<uint8_t> out;
			if (!DecompressPakBlob(stored, true, out)) {
				// The CRC already verified the image at Open: a failing
				// LZ4 block means a torn-in-place write that passed CRC —
				// loud (throw), and the host's load path turns it into a
				// failed launch instead of a corrupt scene.
				throw std::runtime_error(
					"pak LZ4 decompress failed: " + entry.path);
			}
			decompressed_[idx] = std::move(out);
			decompressDone_[idx] = 1;
		}
		return std::span<const uint8_t>(decompressed_[idx]);
	}
	return image_.subspan(static_cast<size_t>(entry.offset), static_cast<size_t>(entry.size));
}

// ---------------------------------------------------------------------------
// PakBackend
// ---------------------------------------------------------------------------

std::shared_ptr<PakBackend> PakBackend::Open(
	std::shared_ptr<const std::vector<uint8_t>> image,
	std::string name,
	std::string& outError) {
	if (!image || image->empty()) {
		outError = "empty pak image";
		return nullptr;
	}
	auto backend = std::shared_ptr<PakBackend>(new PakBackend());
	backend->image_ = std::move(image);
	backend->name_ = std::move(name);
	auto reader = PakReader::Open(std::span<const uint8_t>(*backend->image_), outError);
	if (!reader)
		return nullptr;
	backend->reader_ = std::move(reader);
	return backend;
}

bool PakBackend::Exists(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty() || !reader_)
		return false;
	return reader_->Find(PakAssetKind::Texture, n) != nullptr;
}

std::optional<std::vector<uint8_t>> PakBackend::TryRead(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty() || !reader_)
		return std::nullopt;
	const PakTocEntry* e = reader_->Find(PakAssetKind::Texture, n);
	if (!e)
		return std::nullopt;
	PakTexturePayload payload;
	if (!ParseTexturePayload(reader_->Payload(*e), payload)) {
		std::cerr << "PakBackend: malformed texture payload: " << n << std::endl;
		return std::nullopt;
	}
	return std::vector<uint8_t>(payload.ktxBytes.begin(), payload.ktxBytes.end());
}

std::vector<VfsPath> PakBackend::ListDir(const VfsPath& dir) const {
	std::vector<VfsPath> out;
	if (!reader_)
		return out;
	const VfsPath n = vfs::Normalize(dir);
	const std::string prefix = n.empty() ? std::string{} : n + "/";
	for (const PakTocEntry* e : reader_->BlobsOfKind(PakAssetKind::Texture)) {
		const std::string& p = e->path;
		if (n.empty()) {
			const size_t slash = p.find('/');
			out.push_back(slash == std::string::npos ? p : p.substr(0, slash));
		}
		else if (p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0) {
			const std::string rest = p.substr(prefix.size());
			const size_t slash = rest.find('/');
			out.push_back(slash == std::string::npos ? p : prefix + rest.substr(0, slash));
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::optional<VfsStat> PakBackend::Stat(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty() || !reader_)
		return std::nullopt;
	const PakTocEntry* e = reader_->Find(PakAssetKind::Texture, n);
	if (!e)
		return std::nullopt;
	PakTexturePayload payload;
	if (!ParseTexturePayload(reader_->Payload(*e), payload))
		return std::nullopt;
	VfsStat s;
	s.size = static_cast<uint64_t>(payload.ktxBytes.size());
	s.backend = Name();
	s.source = name_ + ":" + n;
	return s;
}

} // namespace smvk
