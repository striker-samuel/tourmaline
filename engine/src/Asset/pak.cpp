#include <Engine/Asset/pak.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <Engine/Asset/compression.h>

namespace smvk {
namespace {

static_assert(std::endian::native == std::endian::little,
	"Pak format is little-endian; add byte-swap helpers for big-endian hosts");

inline constexpr size_t kHeaderSize = 32;
inline constexpr size_t kFooterSize = 24;
inline constexpr size_t kTocEntryFixed = 8 + 4 + 4 + 8 + 8 + 4; // hash,kind,flags,off,size,pathLen
inline constexpr size_t kAlign = 16;

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

} // namespace

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
	uint64_t ktxSize = 0;
	memcpy(&ktxSize, payload.data() + off, 8);
	off += 8;
	if (off + ktxSize != payload.size())
		return false;
	out.magFilter = samp[0];
	out.minFilter = samp[1];
	out.wrapS = samp[2];
	out.wrapT = samp[3];
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

// ---------------------------------------------------------------------------
// PakWriter
// ---------------------------------------------------------------------------

bool PakWriter::AddBlob(PakAssetKind kind, const std::string& path, std::vector<uint8_t> payload) {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty() || payload.empty())
		return false;
	Pending p;
	p.meta.pathHash = PakHashPath(n);
	p.meta.kind = kind;
	p.meta.flags = PakEntryFlag_None;
	p.meta.path = n;
	if (compression_ == PakCompression::Lz4) {
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
	// checksum covers the final header bytes.
	// Version is v3 when any blob is LZ4-compressed, else v2 (so old
	// v1/v2-only readers keep accepting uncompressed paks).
	const uint64_t tocCount = entries_.size();
	bool anyCompressed = false;
	for (const auto& e : entries_) {
		if (e.meta.flags & PakEntryFlag_Lz4) {
			anyCompressed = true;
			break;
		}
	}
	{
		uint8_t* h = out.data();
		memcpy(h + 0, &kPakMagic, 4);
		const uint32_t version = anyCompressed ? kPakVersion : 2u;
		const uint32_t flags = 0;
		memcpy(h + 4, &version, 4);
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
		uint32_t magic = 0, version = 0;
		uint64_t tocOffset = 0, tocCount = 0;
		f.read(reinterpret_cast<char*>(&magic), 4);
		f.read(reinterpret_cast<char*>(&version), 4);
		f.seekg(4, std::ios::cur); // flags
		f.read(reinterpret_cast<char*>(&tocOffset), 8);
		f.read(reinterpret_cast<char*>(&tocCount), 8);
		if (!f || magic != kPakMagic
			|| version < kPakVersionMin || version > kPakVersion
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
	// previous valid contents). Version stays v3 when any surviving old
	// entry is LZ4-compressed (append itself stores uncompressed only).
	f.seekp(0, std::ios::beg);
	f.write(reinterpret_cast<const char*>(&kPakMagic), 4);
	{
		uint32_t version = 2;
		for (const PakTocEntry& e : entries) {
			if (e.flags & PakEntryFlag_Lz4) {
				version = kPakVersion;
				break;
			}
		}
		f.write(reinterpret_cast<const char*>(&version), 4);
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
	uint32_t magic = 0, version = 0;
	uint64_t tocOffset = 0, tocCount = 0;
	memcpy(&magic, image.data() + 0, 4);
	memcpy(&version, image.data() + 4, 4);
	memcpy(&tocOffset, image.data() + 12, 8);
	memcpy(&tocCount, image.data() + 20, 8);
	if (magic != kPakMagic) {
		outError = "bad pak magic";
		return std::nullopt;
	}
	if (version < kPakVersionMin || version > kPakVersion) {
		outError = "unsupported pak version " + std::to_string(version);
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
	if (PakCrc32(image.data(), fOff + 16) != fCrc) {
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
		if ((e.flags & PakEntryFlag_Lz4) && version < 3) {
			outError = "pak compressed entry in v" + std::to_string(version) + " pak: " + e.path;
			return std::nullopt;
		}
		r.entries_.push_back(std::move(e));
	}
	// Decompress LZ4 blobs into reader-owned buffers so Payload() stays a
	// simple span (transparent to cluster deserialization + PakBackend).
	r.decompressed_.resize(r.entries_.size());
	for (size_t i = 0; i < r.entries_.size(); ++i) {
		const PakTocEntry& e = r.entries_[i];
		if (!(e.flags & PakEntryFlag_Lz4))
			continue;
		std::span<const uint8_t> stored =
			image.subspan(static_cast<size_t>(e.offset), static_cast<size_t>(e.size));
		std::vector<uint8_t> raw;
		if (!DecompressPakBlob(stored, true, raw)) {
			outError = "pak LZ4 decompress failed: " + e.path;
			return std::nullopt;
		}
		r.decompressed_[i] = std::move(raw);
	}
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
	if (idx < decompressed_.size() && !decompressed_[idx].empty())
		return std::span<const uint8_t>(decompressed_[idx]);
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
