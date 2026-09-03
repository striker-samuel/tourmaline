#include <Engine/Asset/pak.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>

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
	p.payload = std::move(payload);
	metas_.push_back(p.meta);
	entries_.push_back(std::move(p));
	return true;
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
	const uint64_t tocCount = entries_.size();
	{
		uint8_t* h = out.data();
		memcpy(h + 0, &kPakMagic, 4);
		const uint32_t version = kPakVersion;
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
	if (version != kPakVersion) {
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
		r.entries_.push_back(std::move(e));
	}
	return r;
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
