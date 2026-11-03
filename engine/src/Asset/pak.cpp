// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/pak.h>

#include <Engine/Asset/appmanifest.h> // kCatalogKind* (the identity-table blob's kind ABI)
#include <Engine/Asset/asset_plan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <Engine/Asset/compression.h>
#include <Engine/Utils/thread_pool.h>

namespace tour::asset {
// (namespace/RHI boundary audit) TU-LOCAL escape hatch: this impl TU
// still spells the smvk names unqualified (ThreadPool,
// TextureStreamMeta, VfsStat...); the header-level broad import is
// GONE (R6), so the TU carries its own contained using-directive -
// the documented pattern. Qualify opportunistically.
using namespace ::smvk;
namespace {

static_assert(std::endian::native == std::endian::little,
	"Pak format is little-endian; add byte-swap helpers for big-endian hosts");

inline constexpr size_t kHeaderSize = 32;
inline constexpr size_t kFooterSize = 24;
// SMP8 TOC entry: hash, kind, flags, offset, size, slot, contentCrc,
// pathLen.
inline constexpr size_t kTocEntryFixed = 8 + 4 + 4 + 8 + 8 + 4 + 4 + 4;
inline constexpr size_t kAlign = 16;

// Images below this stay on the serial CRC/decompress paths (pool spin-up
// would cost more than the work; tests and small appends land here).
// (SMP8 note: the mount CRC is TOC-only now, so the reader itself no
// longer splits thresholds — PakCrc32Parallel stays public for the
// tests + tools that hash big buffers.)

size_t AlignUp(size_t v, size_t align) {
	return (v + align - 1) & ~(align - 1);
}

// Shared reflected CRC-32 lookup table (the 0xEDB88320 polynomial).
// ONE immutable magic-static instance: the C++ runtime guarantees
// exactly-once, THREAD-SAFE initialization. The old lazy
// `static bool init` + unsynchronized mutable fill (both Crc32 feed
// helpers, pre-fix) was a data race on FIRST USE - concurrent first
// appends to different pak files from separate threads read/wrote the
// same table bytes simultaneously (PakCrc32 already carried the
// magic-static form with this exact history; the feeds predated that
// fix). Every CRC step in this TU funnels through this one instance.
const uint32_t* Crc32Table() {
	static const std::array<uint32_t, 256> table = [] {
		std::array<uint32_t, 256> t{};
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			t[i] = c;
		}
		return t;
	}();
	return table.data();
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
	// Zero-worker guard (ThreadPool fail-fast): when every pool shard is
	// leased by live workers elsewhere, the pool refuses to construct —
	// this is a pure ACCELERATION path, so degrade explicitly to the
	// serial CRC instead of failing the pak open.
	struct ChunkCrc {
		uint32_t crc;
		size_t len;
	};
	const size_t chunkCount = (data.size() + chunkBytes - 1) / chunkBytes;
	std::vector<std::future<ChunkCrc>> futs;
	try {
		ThreadPool pool(std::min<size_t>(chunkCount,
			std::thread::hardware_concurrency()));
		futs.reserve(chunkCount);
		for (size_t i = 0; i < chunkCount; ++i) {
			const uint8_t* base = data.data() + i * chunkBytes;
			const size_t len = std::min(chunkBytes, data.size() - i * chunkBytes);
			futs.push_back(pool.enqueue([base, len]() -> ChunkCrc {
				return ChunkCrc{ PakCrc32(base, len), len };
			}));
		}
	}
	catch (const std::runtime_error&) {
		// All shards leased (see ThreadPool): serial CRC, same result.
		return PakCrc32(data.data(), data.size());
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
	// The shared magic-static table (see Crc32Table for the
	// thread-safety history: the old lazy fill raced on first use -
	// PakCrc32Parallel's workers all call this concurrently when a big
	// pak's first CRC is the parallel one).
	const uint32_t* table = Crc32Table();
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
	// SMP8: the STABLE slot (the entry's ordinal - assigned at cook,
	// preserved across append grows; the compiled-plan addressing keys
	// pages by slot, never by TOC position).
	p.meta.slot = static_cast<uint32_t>(entries_.size());
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
	// SMP8: the page integrity - the CRC over the entry's STORED bytes
	// (the LZ4 wrapper included when compressed). Verified at FETCH
	// time by the reader, never at mount.
	p.meta.contentCrc = PakCrc32(p.payload.data(), p.payload.size());
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

	// TOC (SMP8 layout: hash, kind, flags, offset, size, slot,
	// contentCrc, pathLen, path).
	const size_t tocOffset = AlignUp(out.size(), kAlign);
	out.insert(out.end(), tocOffset - out.size(), 0);
	for (const auto& e : entries_) {
		AppendU64(out, e.meta.pathHash);
		AppendU32(out, static_cast<uint32_t>(e.meta.kind));
		AppendU32(out, e.meta.flags);
		AppendU64(out, e.meta.offset);
		AppendU64(out, e.meta.size);
		AppendU32(out, e.meta.slot);
		AppendU32(out, e.meta.contentCrc);
		AppendU32(out, static_cast<uint32_t>(e.meta.path.size()));
		out.insert(out.end(), e.meta.path.begin(), e.meta.path.end());
	}

	// Footer (SMP8): tocOffset, tocCount, the CRC over ONLY the TOC
	// bytes (the mount validates the small metadata cheaply; every
	// blob page verifies through its own contentCrc at fetch), the
	// footer magic. No version field anywhere: the magic IS the
	// version (a breaking format change bumps it and old paks fail
	// closed at Open). NOTE: the CRC range is exactly the TOC - it is
	// computed BEFORE the footer head appends (an off-by-append here
	// would fold the tocOffset/tocCount words into the hash and
	// mismatch the reader's [tocOffset, footerStart) range).
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
	const uint32_t tocCrc = PakCrc32(out.data() + tocOffset,
		out.size() - tocOffset);
	AppendU64(out, static_cast<uint64_t>(tocOffset));
	AppendU64(out, tocCount);
	AppendU32(out, tocCrc);
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
	AppendU32(out, e.slot);
	AppendU32(out, e.contentCrc);
	AppendU32(out, static_cast<uint32_t>(e.path.size()));
	out.insert(out.end(), e.path.begin(), e.path.end());
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
		// SMP8: the page integrity over the stored bytes. The SLOT is
		// assigned after the existing TOC is read (stable ordinals:
		// appended entries take maxExistingSlot + 1).
		e.contentCrc = PakCrc32(payload.data(), payload.size());
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
		// Overflow-safe existing-pak gate (the old
		// `tocOffset + kTocEntryFixed * tocCount > fileSize` WRAPPED:
		// both operands are file-controlled u64s): the TOC must start
		// in [kHeaderSize, fileSize) and its FIXED-size footprint
		// (36 B/entry, a lower bound - paths add more) must fit before
		// EOF. All subtraction; a seek beyond EOF fails the reads
		// below anyway, but this catches it before allocating.
		if (!f || magic != kPakMagic
			|| tocOffset < kHeaderSize || tocOffset > fileSize
			|| tocCount > (fileSize - tocOffset) / kTocEntryFixed) {
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
			// SMP8 fields: the stable slot + the page contentCrc.
			f.read(reinterpret_cast<char*>(&e.slot), 4);
			f.read(reinterpret_cast<char*>(&e.contentCrc), 4);
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
		// included): nothing live is ever overwritten, so direct
		// (offset, size) reads keep resolving across an interrupted
		// append (the crash-ordering contract in pak.h — Open itself
		// stays broken until a later append heals the footer).
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
	// SMP8 SLOTS stay stable: appended entries take
	// max(existing slots) + 1 — an existing page's slot NEVER changes,
	// so slot-keyed addressing (the compiled plans) survives grows.
	{
		uint32_t maxSlot = 0;
		for (const PakTocEntry& e : entries)
			maxSlot = std::max(maxSlot, e.slot);
		for (size_t i = 0; i < newEntries.size(); ++i)
			newEntries[i].slot = maxSlot + 1u + uint32_t(i);
	}
	for (const PakTocEntry& ne : newEntries) {
		bool replaced = false;
		for (PakTocEntry& e : entries) {
			if (e.path == ne.path && e.kind == ne.kind) {
				// Supersede: the old payload turns dead space; the
				// replacement KEEPS the old slot (the stable-ordinal
				// contract - the newest bytes answer the same slot).
				const uint32_t keptSlot = e.slot;
				e = ne;
				e.slot = keptSlot;
				replaced = true;
				break;
			}
		}
		if (!replaced)
			entries.push_back(ne);
	}
	const size_t newTocOffset = AlignUp(size_t(cursor), kAlign);
	uint32_t tocCrc = 0;
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
		// SMP8: the footer CRC covers ONLY the TOC bytes (the small
		// mount metadata; every page verifies through its own
		// contentCrc at fetch).
		tocCrc = PakCrc32(toc.data(), toc.size());
	}
	// Patch the header, then the footer + resize complete the grow.
	// The header is the ONE live byte range rewritten (the crash
	// contract in pak.h: everything else lands after every live old
	// byte; direct (offset, size) reads never touch the header, and
	// Open stays broken from the first payload write until the footer
	// lands — a later append heals it). No version field: the magic IS
	// the version, and appended blobs never change the format.
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
	// Footer (SMP8): tocOffset, tocCount, the TOC CRC (computed above
	// over the in-memory bytes just written), the footer magic + the
	// truncating resize complete the grow.
	const uint64_t footerAt = uint64_t(newTocOffset)
		+ TocEntryBytes(entries);
	const auto count = uint64_t(entries.size());
	f.seekp(std::streamoff(footerAt), std::ios::beg);
	f.write(reinterpret_cast<const char*>(&newTocOffset), 8);
	f.write(reinterpret_cast<const char*>(&count), 8);
	f.write(reinterpret_cast<const char*>(&tocCrc), 4);
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

namespace {

// SMP8 magic for the mesh-plan blob ('MPLN' LE).
inline constexpr uint32_t kMeshPlanMagic = 0x4E4C504Du;

// Shared TOC parser (the image-backed and file-backed opens read the
// same bytes): `toc` is the TOC region, `tocCount` the declared entry
// count, `tocOffset` the region's file/image offset (the payload
// region's upper bound). out starts EMPTY (reserve tocCount). Every
// bounds check is SUBTRACTION-based (attacker-controlled u64/u32
// fields; unsigned addition wraps - see the Open block's history
// note).
bool ParseTocEntries(std::span<const uint8_t> toc, uint64_t tocCount,
	uint64_t tocOffset, std::vector<PakTocEntry>& out,
	std::string& outError) {
	size_t off = 0;
	for (uint64_t i = 0; i < tocCount; ++i) {
		if (kTocEntryFixed > toc.size() - off) {
			outError = "pak TOC overruns image";
			return false;
		}
		PakTocEntry e;
		uint32_t kind = 0, pathLen = 0;
		memcpy(&e.pathHash, toc.data() + off, 8);
		memcpy(&kind, toc.data() + off + 8, 4);
		memcpy(&e.flags, toc.data() + off + 12, 4);
		memcpy(&e.offset, toc.data() + off + 16, 8);
		memcpy(&e.size, toc.data() + off + 24, 8);
		memcpy(&e.slot, toc.data() + off + 32, 4);
		memcpy(&e.contentCrc, toc.data() + off + 36, 4);
		memcpy(&pathLen, toc.data() + off + 40, 4);
		off += kTocEntryFixed;
		if (pathLen > 4096 || pathLen > toc.size() - off) {
			outError = "pak TOC path overruns image";
			return false;
		}
		e.kind = static_cast<PakAssetKind>(kind);
		e.path.assign(reinterpret_cast<const char*>(toc.data() + off), pathLen);
		off += pathLen;
		// Blob ranges live in the PAYLOAD region [kHeaderSize,
		// tocOffset): lower-bound checked, upper bound by subtraction
		// (the old `e.offset + e.size > tocOffset` wrapped).
		if (e.offset < kHeaderSize || e.offset > tocOffset
			|| e.size > tocOffset - e.offset) {
			outError = "pak blob overruns TOC: " + e.path;
			return false;
		}
		if (e.pathHash != PakHashPath(e.path)) {
			outError = "pak path hash mismatch: " + e.path;
			return false;
		}
		if (e.flags & ~PakEntryFlag_Lz4) {
			outError = "pak unknown entry flags: " + e.path;
			return false;
		}
		out.push_back(std::move(e));
	}
	if (off != toc.size()) {
		outError = "pak TOC trailing bytes";
		return false;
	}
	return true;
}

} // namespace

struct PakReader::FileHandle {
	std::fstream f; // the open pak file (positional page reads)
};

PakReader::~PakReader() = default;
PakReader::PakReader(PakReader&&) noexcept = default;
PakReader& PakReader::operator=(PakReader&&) noexcept = default;

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
	// ONE CANONICAL PATH: exactly one accepted magic. Any older epoch
	// (SMP5/SMP6/SMP7) fails closed here — content recooks from zero,
	// never migrates.
	if (magic != kPakMagic) {
		outError = "bad pak magic (stale pak - recook from zero)";
		return std::nullopt;
	}

	// Footer check.
	uint64_t fTocOffset = 0, fTocCount = 0;
	uint32_t fCrc = 0, fMagic = 0;
	const size_t fOff = image.size() - kFooterSize;
	memcpy(&fTocOffset, image.data() + fOff + 0, 8);
	memcpy(&fTocCount, image.data() + fOff + 8, 8);
	memcpy(&fCrc, image.data() + fOff + 16, 4);
	memcpy(&fMagic, image.data() + fOff + 20, 4);
	if (fMagic != kPakFooterMagic) {
		outError = "bad pak footer magic";
		return std::nullopt;
	}
	if (fTocOffset != tocOffset || fTocCount != tocCount) {
		outError = "pak header/footer TOC mismatch";
		return std::nullopt;
	}
	// TOC placement: the TOC region lives in [kHeaderSize, fOff) - it
	// may not start inside the header (payloads always follow the
	// 32-byte header) and may not touch the footer region. This gate
	// also bounds every subtraction below: fOff <= image.size(), so
	// `fOff - off` is well-defined for any off that passes it. (A CRC
	// only proves the bytes are as-authored - a hostile/corrupt blob
	// can carry a RECOMPUTED CRC over any offset field, so the range
	// checks do the actual safety work.)
	if (tocOffset < kHeaderSize || tocOffset > fOff) {
		outError = "pak TOC offset out of range";
		return std::nullopt;
	}
	// SMP8 MOUNT CRC: the SMALL metadata only - the TOC bytes
	// [tocOffset, fOff). Every blob page verifies through its own
	// contentCrc at FETCH time (the lazy per-page contract), so a
	// mount never pays a multi-GB walk; VerifyAll() is the explicit
	// whole-image policy for deployment gates.
	const uint32_t tocCrc = PakCrc32(image.data() + tocOffset,
		fOff - tocOffset);
	if (tocCrc != fCrc) {
		outError = "pak TOC CRC mismatch (corrupt or truncated)";
		return std::nullopt;
	}

	PakReader r;
	r.image_ = image;
	// The reserve is a HINT capped to a sane bound: tocCount is an
	// attacker-controlled u64 and the fuzz corpus sends 2^40 — the
	// loop below is bounded (it dies on the first out-of-region
	// entry), so the reserve must never try to materialize the count.
	r.entries_.reserve(static_cast<size_t>(
		std::min<uint64_t>(tocCount, 1u << 20)));
	if (!ParseTocEntries(
			std::span<const uint8_t>(image.data() + tocOffset,
				fOff - tocOffset),
			tocCount, tocOffset, r.entries_, outError))
		return std::nullopt;
	// LAZY materialization + page verification: Open only parses +
	// CRCs the small metadata — pages fetch (+verify their
	// contentCrc, +decompress when LZ4) on FIRST Payload() access
	// (see Payload). The materialized payloads are cached for the
	// reader's LIFETIME (Payload() hands out spans into
	// decompressed_ — an eviction-based bounded cache would
	// invalidate live spans; the cache is therefore bounded by
	// "everything actually read", always <= the eager equivalent).
	r.decompressed_.assign(r.entries_.size(), {});
	r.decompressDone_.assign(r.entries_.size(), 0);
	r.verified_.assign(r.entries_.size(), 0);
	BuildSlotIndex(r);
	r.lazyMutex_ = std::make_unique<std::mutex>();
	return r;
}

std::optional<PakReader> PakReader::OpenFile(const std::string& path,
	std::string& outError) {
	std::fstream f(path, std::ios::in | std::ios::binary);
	if (!f) {
		outError = "cannot open pak file: " + path;
		return std::nullopt;
	}
	f.seekg(0, std::ios::end);
	const uint64_t fileSize = uint64_t(f.tellg());
	if (fileSize < kHeaderSize + kFooterSize) {
		outError = "pak too small";
		return std::nullopt;
	}
	// Header.
	uint32_t magic = 0;
	uint64_t tocOffset = 0, tocCount = 0;
	f.seekg(0, std::ios::beg);
	f.read(reinterpret_cast<char*>(&magic), 4);
	f.seekg(4, std::ios::cur); // reserved
	f.seekg(4, std::ios::cur); // flags
	f.read(reinterpret_cast<char*>(&tocOffset), 8);
	f.read(reinterpret_cast<char*>(&tocCount), 8);
	if (!f || magic != kPakMagic) {
		outError = "bad pak magic (stale pak - recook from zero)";
		return std::nullopt;
	}
	// Footer.
	uint64_t fTocOffset = 0, fTocCount = 0;
	uint32_t fCrc = 0, fMagic = 0;
	f.seekg(std::streamoff(fileSize - kFooterSize), std::ios::beg);
	f.read(reinterpret_cast<char*>(&fTocOffset), 8);
	f.read(reinterpret_cast<char*>(&fTocCount), 8);
	f.read(reinterpret_cast<char*>(&fCrc), 4);
	f.read(reinterpret_cast<char*>(&fMagic), 4);
	if (!f || fMagic != kPakFooterMagic) {
		outError = "bad pak footer magic";
		return std::nullopt;
	}
	if (fTocOffset != tocOffset || fTocCount != tocCount) {
		outError = "pak header/footer TOC mismatch";
		return std::nullopt;
	}
	const uint64_t fOff = fileSize - kFooterSize;
	if (tocOffset < kHeaderSize || tocOffset > fOff) {
		outError = "pak TOC offset out of range";
		return std::nullopt;
	}
	// Read + verify the TOC region only (the SMP8 small mount).
	std::vector<uint8_t> toc(static_cast<size_t>(fOff - tocOffset));
	f.seekg(std::streamoff(tocOffset), std::ios::beg);
	f.read(reinterpret_cast<char*>(toc.data()),
		std::streamsize(toc.size()));
	if (!f || f.gcount() != std::streamsize(toc.size())) {
		outError = "pak TOC read failed: " + path;
		return std::nullopt;
	}
	if (PakCrc32(toc.data(), toc.size()) != fCrc) {
		outError = "pak TOC CRC mismatch (corrupt or truncated)";
		return std::nullopt;
	}
	PakReader r;
	// Capped reserve hint (the fuzz corpus's 2^40 tocCount must never
	// reach an allocation; the parse loop is bounded by the first
	// out-of-region entry).
	r.entries_.reserve(static_cast<size_t>(
		std::min<uint64_t>(tocCount, 1u << 20)));
	if (!ParseTocEntries(toc, tocCount, tocOffset, r.entries_, outError))
		return std::nullopt;
	r.decompressed_.assign(r.entries_.size(), {});
	r.decompressDone_.assign(r.entries_.size(), 0);
	r.verified_.assign(r.entries_.size(), 0);
	BuildSlotIndex(r);
	r.lazyMutex_ = std::make_unique<std::mutex>();
	r.file_ = std::make_unique<FileHandle>();
	r.file_->f = std::move(f);
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

const PakTocEntry* PakReader::FindBySlot(uint32_t slot) const {
	// The compiled-plan lane's O(1) addressing: the slot index built
	// at open — never a TOC walk.
	if (slot >= entriesBySlot_.size())
		return nullptr;
	const int32_t idx = entriesBySlot_[slot];
	if (idx < 0 || size_t(idx) >= entries_.size())
		return nullptr;
	return &entries_[size_t(idx)];
}

void PakReader::BuildSlotIndex(PakReader& r)
{
	uint32_t maxSlot = 0;
	bool any = false;
	for (const PakTocEntry& e : r.entries_) {
		maxSlot = std::max(maxSlot, e.slot);
		any = true;
	}
	r.entriesBySlot_.assign(any ? size_t(maxSlot) + 1 : 0, -1);
	for (size_t i = 0; i < r.entries_.size(); ++i) {
		const uint32_t slot = r.entries_[i].slot;
		// A duplicate slot is a corrupt TOC (the writer assigns
		// uniquely): first wins — the layout fingerprint (which folds
		// the slot table) catches it loudly at the gate anyway.
		if (r.entriesBySlot_[slot] < 0)
			r.entriesBySlot_[slot] = int32_t(i);
	}
}

std::span<const uint8_t> PakReader::Payload(const PakTocEntry& entry) const {
	// Entries are only ever accessed via pointers into entries_ (Find /
	// BlobsOfKind / FindBySlot), so pointer arithmetic recovers the
	// owned buffer.
	const size_t idx = static_cast<size_t>(&entry - entries_.data());
	// First fetch: materialize (file-backed: positional page read) +
	// VERIFY the page's contentCrc once per reader. Guarded so
	// concurrent first reads (the preload thread + the render thread
	// sharing a backend) materialize exactly once; the result stays
	// cached for the reader's lifetime — the returned span never
	// invalidates.
	std::lock_guard<std::mutex> lock(*lazyMutex_);
	if (!verified_[idx]) {
		if (file_) {
			if (!decompressDone_[idx]) {
				std::vector<uint8_t> stored(static_cast<size_t>(entry.size));
				file_->f.clear();
				file_->f.seekg(std::streamoff(entry.offset), std::ios::beg);
				file_->f.read(reinterpret_cast<char*>(stored.data()),
					std::streamsize(stored.size()));
				if (file_->f.gcount() != std::streamsize(stored.size()))
					throw std::runtime_error(
						"pak page read failed: " + entry.path);
				if (PakCrc32(stored.data(), stored.size())
						!= entry.contentCrc)
					throw std::runtime_error(
						"pak page CRC mismatch (corrupt page): "
						+ entry.path);
				if (entry.flags & PakEntryFlag_Lz4) {
					std::vector<uint8_t> out;
					if (!DecompressPakBlob(stored, true, out))
						throw std::runtime_error(
							"pak LZ4 decompress failed: " + entry.path);
					decompressed_[idx] = std::move(out);
				}
				else {
					decompressed_[idx] = std::move(stored);
				}
				decompressDone_[idx] = 1;
			}
			// else: a previously-fetched page - already materialized
			// AND verified.
		}
		else {
			const std::span<const uint8_t> stored = image_.subspan(
				static_cast<size_t>(entry.offset),
				static_cast<size_t>(entry.size));
			if (PakCrc32(stored.data(), stored.size()) != entry.contentCrc)
				throw std::runtime_error(
					"pak page CRC mismatch (corrupt page): " + entry.path);
			if ((entry.flags & PakEntryFlag_Lz4)
					&& !decompressDone_[idx]) {
				std::vector<uint8_t> out;
				if (!DecompressPakBlob(stored, true, out))
					throw std::runtime_error(
						"pak LZ4 decompress failed: " + entry.path);
				decompressed_[idx] = std::move(out);
				decompressDone_[idx] = 1;
			}
		}
		verified_[idx] = 1;
	}
	if (file_ || (entry.flags & PakEntryFlag_Lz4))
		return std::span<const uint8_t>(decompressed_[idx]);
	return image_.subspan(static_cast<size_t>(entry.offset),
		static_cast<size_t>(entry.size));
}

bool PakReader::VerifyAll(std::string& outError) const {
	// The explicit full-verification policy (deployment gates): hash
	// every entry's STORED bytes against its contentCrc. Pages already
	// fetched were verified at fetch (verified_); the rest hash here
	// WITHOUT materializing (no decompression, no page cache growth).
	std::lock_guard<std::mutex> lock(*lazyMutex_);
	std::vector<uint8_t> scratch;
	for (const PakTocEntry& e : entries_) {
		const size_t idx = static_cast<size_t>(&e - entries_.data());
		if (verified_[idx])
			continue;
		if (file_) {
			scratch.assign(static_cast<size_t>(e.size), 0);
			file_->f.clear();
			file_->f.seekg(std::streamoff(e.offset), std::ios::beg);
			file_->f.read(reinterpret_cast<char*>(scratch.data()),
				std::streamsize(scratch.size()));
			if (file_->f.gcount() != std::streamsize(scratch.size())) {
				outError = "pak page read failed: " + e.path;
				return false;
			}
			if (PakCrc32(scratch.data(), scratch.size()) != e.contentCrc) {
				outError = "pak page CRC mismatch: " + e.path;
				return false;
			}
		}
		else {
			const std::span<const uint8_t> stored = image_.subspan(
				static_cast<size_t>(e.offset),
				static_cast<size_t>(e.size));
			if (PakCrc32(stored.data(), stored.size()) != e.contentCrc) {
				outError = "pak page CRC mismatch: " + e.path;
				return false;
			}
		}
	}
	return true;
}

bool PakReader::MeshPlanFamily(const std::string& familyPath,
	std::vector<PakMeshPlanMember>& outMembers,
	std::string& outError) const {
	outMembers.clear();
	const PakTocEntry* plan = Find(PakAssetKind::Plan, kPakMeshPlanPath);
	if (!plan) {
		outError = "the pak carries no mesh plan (a version skew"
			" between the catalog and the cooked content - recook)";
		return false;
	}
	std::vector<PakMeshPlanFamily> families;
	std::string perr;
	if (!ParseMeshPlanBlob(Payload(*plan), families, perr)) {
		outError = "the pak's mesh plan is unreadable: " + perr;
		return false;
	}
	for (const PakMeshPlanFamily& f : families)
		if (f.familyPath == familyPath) {
			outMembers = f.members;
			return true;
		}
	outError = "the pak's mesh plan has no family '" + familyPath
		+ "' (a version skew between the catalog and the cooked"
		" content - recook)";
	return false;
}

// ---- the cooked mesh-plan blob codec ---------------------------------------

std::vector<uint8_t> WriteMeshPlanBlob(
	const std::vector<PakMeshPlanFamily>& families) {
	std::vector<uint8_t> out;
	AppendU32(out, kMeshPlanMagic);
	AppendU32(out, 1); // version
	AppendU32(out, static_cast<uint32_t>(families.size()));
	for (const PakMeshPlanFamily& fam : families) {
		AppendU32(out, static_cast<uint32_t>(fam.familyPath.size()));
		out.insert(out.end(), fam.familyPath.begin(), fam.familyPath.end());
		AppendU32(out, static_cast<uint32_t>(fam.members.size()));
		for (const PakMeshPlanMember& m : fam.members) {
			AppendU32(out, static_cast<uint32_t>(m.tocPath.size()));
			out.insert(out.end(), m.tocPath.begin(), m.tocPath.end());
			AppendU32(out, m.lodIndex);
		}
	}
	return out;
}

bool ParseMeshPlanBlob(std::span<const uint8_t> blob,
	std::vector<PakMeshPlanFamily>& out, std::string& err) {
	out.clear();
	const auto readU32 = [&blob](size_t off, uint32_t& v) -> bool {
		if (off > blob.size() || 4 > blob.size() - off)
			return false;
		memcpy(&v, blob.data() + off, 4);
		return true;
	};
	uint32_t magic = 0, version = 0, familyCount = 0;
	if (!readU32(0, magic) || !readU32(4, version) || !readU32(8, familyCount)) {
		err = "mesh plan header truncated";
		return false;
	}
	if (magic != kMeshPlanMagic) {
		err = "bad mesh plan magic";
		return false;
	}
	if (version != 1) {
		err = "unsupported mesh plan version";
		return false;
	}
	if (familyCount > 65536) {
		err = "mesh plan family count out of range";
		return false;
	}
	size_t off = 12;
	out.reserve(familyCount);
	for (uint32_t i = 0; i < familyCount; ++i) {
		PakMeshPlanFamily fam;
		uint32_t pathLen = 0, memberCount = 0;
		if (!readU32(off, pathLen)) {
			err = "mesh plan family path truncated";
			return false;
		}
		off += 4;
		if (pathLen > 4096 || pathLen > blob.size() - off) {
			err = "mesh plan family path out of range";
			return false;
		}
		fam.familyPath.assign(
			reinterpret_cast<const char*>(blob.data() + off), pathLen);
		off += pathLen;
		if (!readU32(off, memberCount)) {
			err = "mesh plan member count truncated";
			return false;
		}
		off += 4;
		if (memberCount > 65536) {
			err = "mesh plan member count out of range";
			return false;
		}
		fam.members.reserve(memberCount);
		for (uint32_t m = 0; m < memberCount; ++m) {
			PakMeshPlanMember mem;
			uint32_t mPathLen = 0, lodIndex = 0;
			if (!readU32(off, mPathLen)) {
				err = "mesh plan member path truncated";
				return false;
			}
			off += 4;
			if (mPathLen > 4096 || mPathLen > blob.size() - off) {
				err = "mesh plan member path out of range";
				return false;
			}
			mem.tocPath.assign(
				reinterpret_cast<const char*>(blob.data() + off), mPathLen);
			off += mPathLen;
			if (!readU32(off, lodIndex)) {
				err = "mesh plan member LOD truncated";
				return false;
			}
			off += 4;
			mem.lodIndex = lodIndex;
			fam.members.push_back(std::move(mem));
		}
		out.push_back(std::move(fam));
	}
	if (off != blob.size()) {
		err = "mesh plan trailing bytes";
		return false;
	}
	return true;
}

// ---- the per-pak identity-table blob codec ----------------------------------

inline constexpr uint32_t kAssetIdsMagic = 0x52544449u; // "IDTR" LE
inline constexpr uint32_t kAssetIdsVersion = 1;

std::vector<uint8_t> WriteAssetIdsBlob(
	const std::vector<PakAssetIdRow>& rows) {
	std::vector<uint8_t> out;
	AppendU32(out, kAssetIdsMagic);
	AppendU32(out, kAssetIdsVersion);
	AppendU32(out, static_cast<uint32_t>(rows.size()));
	for (const PakAssetIdRow& r : rows) {
		AppendU64(out, r.id);
		out.push_back(r.kind);
		AppendU32(out, static_cast<uint32_t>(r.path.size()));
		out.insert(out.end(), r.path.begin(), r.path.end());
		AppendU32(out, static_cast<uint32_t>(r.sceneName.size()));
		out.insert(out.end(), r.sceneName.begin(), r.sceneName.end());
	}
	return out;
}

bool ParseAssetIdsBlob(std::span<const uint8_t> blob,
	std::vector<PakAssetIdRow>& out, std::string& err) {
	out.clear();
	const auto readU32 = [&blob](size_t off, uint32_t& v) -> bool {
		if (off > blob.size() || 4 > blob.size() - off)
			return false;
		memcpy(&v, blob.data() + off, 4);
		return true;
	};
	const auto readU64 = [&blob](size_t off, uint64_t& v) -> bool {
		if (off > blob.size() || 8 > blob.size() - off)
			return false;
		memcpy(&v, blob.data() + off, 8);
		return true;
	};
	uint32_t magic = 0, version = 0, rowCount = 0;
	if (!readU32(0, magic) || !readU32(4, version)
			|| !readU32(8, rowCount)) {
		err = "identity table header truncated";
		return false;
	}
	if (magic != kAssetIdsMagic) {
		err = "bad identity table magic";
		return false;
	}
	if (version != kAssetIdsVersion) {
		err = "unsupported identity table version";
		return false;
	}
	if (rowCount > (1u << 20)) {
		err = "identity table row count out of range";
		return false;
	}
	size_t off = 12;
	out.reserve(rowCount);
	for (uint32_t i = 0; i < rowCount; ++i) {
		PakAssetIdRow row;
		if (!readU64(off, row.id)) {
			err = "identity table row id truncated";
			return false;
		}
		off += 8;
		if (off >= blob.size()) {
			err = "identity table row kind truncated";
			return false;
		}
		row.kind = blob[off];
		++off;
		uint32_t pathLen = 0, nameLen = 0;
		if (!readU32(off, pathLen)) {
			err = "identity table row path length truncated";
			return false;
		}
		off += 4;
		if (pathLen == 0 || pathLen > 4096 || pathLen > blob.size() - off) {
			err = "identity table row path out of range";
			return false;
		}
		row.path.assign(
			reinterpret_cast<const char*>(blob.data() + off), pathLen);
		off += pathLen;
		if (!readU32(off, nameLen)) {
			err = "identity table row name length truncated";
			return false;
		}
		off += 4;
		if (nameLen > 4096 || nameLen > blob.size() - off) {
			err = "identity table row name out of range";
			return false;
		}
		row.sceneName.assign(
			reinterpret_cast<const char*>(blob.data() + off), nameLen);
		off += nameLen;
		// The row invariants (the same ones the manifest's [ids]
		// validation enforces — the shared ABI never drifts):
		// a nonzero id, a non-Nil kind byte inside the catalog ABI, and the
		// scene-name rule (names ride SCENE rows only).
		if (row.id == 0) {
			err = "identity table row carries id 0 (the reserved"
				" no-identity value)";
			return false;
		}
		if (row.kind == smvk::kCatalogKindNil
				|| row.kind > smvk::kCatalogKindNavMesh) {
			err = "identity table row kind byte out of the catalog ABI";
			return false;
		}
		if (!row.sceneName.empty() && row.kind != smvk::kCatalogKindScene) {
			err = "identity table row names a non-scene asset";
			return false;
		}
		out.push_back(std::move(row));
	}
	if (off != blob.size()) {
		err = "identity table trailing bytes";
		return false;
	}
	return true;
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
	// The four VFS-addressable kinds (pak.h's interop contract):
	// texture blobs (embedded .ktx2), sound blobs (raw .wav), shader
	// blobs (raw compiled bytecode) and audiograph blobs (authored
	// text, verbatim).
	return reader_->Find(PakAssetKind::Texture, n) != nullptr
		|| reader_->Find(PakAssetKind::Sound, n) != nullptr
		|| reader_->Find(PakAssetKind::Shader, n) != nullptr
		|| reader_->Find(PakAssetKind::AudioGraph, n) != nullptr
		|| reader_->Find(PakAssetKind::SpriteSet, n) != nullptr
		|| reader_->Find(PakAssetKind::StateMachine, n) != nullptr
		|| reader_->Find(PakAssetKind::NavMesh, n) != nullptr;
}

std::optional<std::vector<uint8_t>> PakServeVfsBytes(
	PakAssetKind kind, std::span<const uint8_t> payload,
	const std::string& path) {
	// The shared serve shape (the backend's TryRead + the PAGER lane):
	// texture blobs unwrap the envelope to the embedded .ktx2; sound +
	// shader blobs ARE the payload (the raw .wav / the raw compiled
	// bytecode); audiograph blobs are the authored text VERBATIM.
	// Nullopt on a malformed texture envelope (logged with the path).
	if (kind == PakAssetKind::Texture) {
		PakTexturePayload parsed;
		if (!ParseTexturePayload(payload, parsed)) {
			std::cerr << "PakServeVfsBytes: malformed texture payload: "
				<< path << std::endl;
			return std::nullopt;
		}
		return std::vector<uint8_t>(parsed.ktxBytes.begin(),
			parsed.ktxBytes.end());
	}
	if (kind == PakAssetKind::Sound || kind == PakAssetKind::Shader
		|| kind == PakAssetKind::AudioGraph || kind == PakAssetKind::SpriteSet
		|| kind == PakAssetKind::StateMachine || kind == PakAssetKind::NavMesh)
		return std::vector<uint8_t>(payload.begin(), payload.end());
	return std::nullopt;
}

std::optional<std::vector<uint8_t>> PakBackend::TryRead(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty() || !reader_)
		return std::nullopt;
	// The four VFS-addressable kinds (pak.h's interop contract),
	// served through the shared per-kind helper.
	for (const PakAssetKind kind : { PakAssetKind::Texture,
			PakAssetKind::Sound, PakAssetKind::Shader,
			PakAssetKind::AudioGraph, PakAssetKind::SpriteSet,
			PakAssetKind::StateMachine, PakAssetKind::NavMesh }) {
		const PakTocEntry* e = reader_->Find(kind, n);
		if (!e)
			continue;
		return PakServeVfsBytes(kind, reader_->Payload(*e), n);
	}
	return std::nullopt;
}

std::vector<VfsPath> PakBackend::ListDir(const VfsPath& dir) const {
	std::vector<VfsPath> out;
	if (!reader_)
		return out;
	const VfsPath n = vfs::Normalize(dir);
	const std::string prefix = n.empty() ? std::string{} : n + "/";
	// Every VFS-addressable kind lists (the overlay's union view).
	std::vector<const PakTocEntry*> all;
	all.reserve(reader_->BlobsOfKind(PakAssetKind::Texture).size()
		+ reader_->BlobsOfKind(PakAssetKind::Sound).size()
		+ reader_->BlobsOfKind(PakAssetKind::Shader).size()
		+ reader_->BlobsOfKind(PakAssetKind::AudioGraph).size()
		+ reader_->BlobsOfKind(PakAssetKind::SpriteSet).size()
		+ reader_->BlobsOfKind(PakAssetKind::StateMachine).size()
		+ reader_->BlobsOfKind(PakAssetKind::NavMesh).size());
	for (const PakTocEntry* e : reader_->BlobsOfKind(PakAssetKind::Texture))
		all.push_back(e);
	for (const PakTocEntry* e : reader_->BlobsOfKind(PakAssetKind::Sound))
		all.push_back(e);
	for (const PakTocEntry* e : reader_->BlobsOfKind(PakAssetKind::Shader))
		all.push_back(e);
	for (const PakTocEntry* e : reader_->BlobsOfKind(PakAssetKind::AudioGraph))
		all.push_back(e);
	for (const PakTocEntry* e : reader_->BlobsOfKind(PakAssetKind::SpriteSet))
		all.push_back(e);
	for (const PakTocEntry* e : reader_->BlobsOfKind(PakAssetKind::StateMachine))
		all.push_back(e);
	for (const PakTocEntry* e : reader_->BlobsOfKind(PakAssetKind::NavMesh))
		all.push_back(e);
	for (const PakTocEntry* e : all) {
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
	if (e) {
		PakTexturePayload payload;
		if (!ParseTexturePayload(reader_->Payload(*e), payload))
			return std::nullopt;
		VfsStat s;
		s.size = static_cast<uint64_t>(payload.ktxBytes.size());
		s.backend = Name();
		s.source = name_ + ":" + n;
		return s;
	}
	e = reader_->Find(PakAssetKind::Sound, n);
	if (e) {
		VfsStat s;
		s.size = static_cast<uint64_t>(reader_->Payload(*e).size());
		s.backend = Name();
		s.source = name_ + ":" + n;
		return s;
	}
	e = reader_->Find(PakAssetKind::Shader, n);
	if (e) {
		VfsStat s;
		s.size = static_cast<uint64_t>(reader_->Payload(*e).size());
		s.backend = Name();
		s.source = name_ + ":" + n;
		return s;
	}
	e = reader_->Find(PakAssetKind::AudioGraph, n);
	if (e) {
		VfsStat s;
		s.size = static_cast<uint64_t>(reader_->Payload(*e).size());
		s.backend = Name();
		s.source = name_ + ":" + n;
		return s;
	}
	e = reader_->Find(PakAssetKind::SpriteSet, n);
	if (e) {
		VfsStat s;
		s.size = static_cast<uint64_t>(reader_->Payload(*e).size());
		s.backend = Name();
		s.source = name_ + ":" + n;
		return s;
	}
	for (const PakAssetKind kind :
			{ PakAssetKind::StateMachine, PakAssetKind::NavMesh }) {
		e = reader_->Find(kind, n);
		if (!e) continue;
		VfsStat s;
		s.size = static_cast<uint64_t>(reader_->Payload(*e).size());
		s.backend = Name();
		s.source = name_ + ":" + n;
		return s;
	}
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// THE LAYOUT FINGERPRINTS (the compiled static plans' fail-closed
// contract — see Engine/Asset/asset_plan.h): ONE shared fold, used by
// BOTH the generator (tools) and the runtime gate — two drifting
// implementations would be a silent-accept bug. Content bytes are
// deliberately NOT folded (the SMP8 per-page contentCrc owns them):
// equal fingerprints + different bytes = a payload-only redeploy, no
// recompile.
// ---------------------------------------------------------------------------

namespace {

// FNV-1a 64 over the canonical text (same constants as PakHashPath).
std::string Fnva64Hex(const std::string& canonical) {
	uint64_t h = 14695981039346656037ull;
	for (unsigned char c : canonical) {
		h ^= c;
		h *= 1099511628211ull;
	}
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%016llx",
		static_cast<unsigned long long>(h));
	return std::string(buf);
}

void AppendFpU32(std::string& out, uint32_t v) {
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%u ", v);
	out += buf;
}
void AppendFpU64(std::string& out, uint64_t v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%llu ",
		static_cast<unsigned long long>(v));
	out += buf;
}

} // namespace

std::string PakLayoutFingerprint(const PakReader& reader) {
	// The canonical form: the slot table in SLOT order (slot, kind,
	// codec flags, stored length, path hash — everything a compiled
	// read keys or sizes against) + the cooked mesh-plan join blob
	// VERBATIM (the family -> member -> LOD structure the plans
	// validate through). The pak's OWN magic epoch is implicitly
	// covered: the reader refused to open anything but the current
	// one.
	std::string c;
	c += "layout-v1 ";
	AppendFpU64(c, uint64_t(reader.EntryCount()));
	c += "\n";
	std::vector<const PakTocEntry*> bySlot;
	bySlot.reserve(reader.Entries().size());
	for (const PakTocEntry& e : reader.Entries())
		bySlot.push_back(&e);
	std::sort(bySlot.begin(), bySlot.end(),
		[](const PakTocEntry* a, const PakTocEntry* b) {
			return a->slot < b->slot;
		});
	for (const PakTocEntry* e : bySlot) {
		AppendFpU32(c, e->slot);
		AppendFpU32(c, static_cast<uint32_t>(e->kind));
		AppendFpU32(c, e->flags);
		AppendFpU64(c, e->size);
		AppendFpU64(c, e->pathHash);
		c += "\n";
	}
	// The mesh-plan join (a pak without a plan folds as none — the
	// plan lane's family lookups fail loudly either way).
	if (const PakTocEntry* plan =
			reader.Find(PakAssetKind::Plan, kPakMeshPlanPath)) {
		const std::span<const uint8_t> blob = reader.Payload(*plan);
		c += "meshplan ";
		AppendFpU64(c, uint64_t(blob.size()));
		c += "\n";
		c.append(reinterpret_cast<const char*>(blob.data()),
			blob.size());
		c += "\n";
	}
	else {
		c += "nomeshplan\n";
	}
	return Fnva64Hex(c);
}

std::string FoldPakLayouts(const std::string* pakNames,
	const std::string* pakFingerprints, size_t pakCount) {
	std::string c;
	c += "paks-v1 ";
	AppendFpU64(c, uint64_t(pakCount));
	c += "\n";
	for (size_t i = 0; i < pakCount; ++i) {
		c += pakNames[i];
		c += " ";
		c += pakFingerprints[i];
		c += "\n";
	}
	return Fnva64Hex(c);
}

} // namespace tour::asset
