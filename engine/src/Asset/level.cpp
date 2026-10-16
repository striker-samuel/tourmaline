// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// LEVEL blob writer/reader (see Asset/level.h). Little-endian,
// magic-is-version, fail-closed — the pak discipline.

#include <Engine/Asset/level.h>

#include <cstring>

namespace smvk {

namespace {

// Little-endian put/get (the pak + scene-blob convention).
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
	out.insert(out.end(), reinterpret_cast<const uint8_t*>(&v),
		reinterpret_cast<const uint8_t*>(&v) + 4);
}
void PutU64(std::vector<uint8_t>& out, uint64_t v) {
	out.insert(out.end(), reinterpret_cast<const uint8_t*>(&v),
		reinterpret_cast<const uint8_t*>(&v) + 8);
}
void PutStr(std::vector<uint8_t>& out, std::string_view s) {
	PutU32(out, uint32_t(s.size()));
	out.insert(out.end(), reinterpret_cast<const uint8_t*>(s.data()),
		reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}

struct Cursor {
	const uint8_t* p = nullptr;
	size_t left = 0;
	bool ok = true;
	uint32_t u32() {
		uint32_t v = 0;
		if (!ok || left < 4) {
			ok = false;
			return 0;
		}
		std::memcpy(&v, p, 4);
		p += 4;
		left -= 4;
		return v;
	}
	uint64_t u64() {
		uint64_t v = 0;
		if (!ok || left < 8) {
			ok = false;
			return 0;
		}
		std::memcpy(&v, p, 8);
		p += 8;
		left -= 8;
		return v;
	}
	std::string str() {
		const uint32_t n = u32();
		if (!ok || left < n) {
			ok = false;
			return {};
		}
		std::string s(reinterpret_cast<const char*>(p), n);
		p += n;
		left -= n;
		return s;
	}
};

} // namespace

const LevelSubBlob* LevelBlob::Find(std::string_view key) const {
	for (const LevelSubBlob& s : subs)
		if (s.key == key)
			return &s;
	return nullptr;
}

std::span<const uint8_t> LevelBlob::Bytes(std::string_view key) const {
	const LevelSubBlob* s = Find(key);
	if (!s)
		return {};
	if (s->offset > payload.size()
		|| s->size > payload.size() - s->offset)
		return {}; // parsed bounds already validated; belt + suspenders
	return payload.subspan(size_t(s->offset), size_t(s->size));
}

bool BuildLevelBlob(std::string_view name, std::string_view sceneKey,
	std::span<const LevelSubInput> subs, std::vector<uint8_t>& out) {
	out.clear();
	out.reserve(16 + name.size() + sceneKey.size() + 64);
	PutU32(out, kLevelMagic);
	PutStr(out, name);
	PutStr(out, sceneKey);
	PutU32(out, uint32_t(subs.size()));
	// Column blocks (the wire layout in level.h): kind/key first, then
	// offset/size (payload-relative — computed against the payload that
	// follows), then the payload itself, sub-blob order.
	for (const LevelSubInput& s : subs) {
		PutU32(out, uint32_t(s.kind));
		PutStr(out, s.key);
	}
	{
		uint64_t off = 0;
		for (const LevelSubInput& s : subs) {
			PutU64(out, off);
			PutU64(out, s.bytes.size());
			off += s.bytes.size();
		}
	}
	for (const LevelSubInput& s : subs)
		out.insert(out.end(), s.bytes.begin(), s.bytes.end());
	return true;
}

bool ParseLevelBlob(std::span<const uint8_t> blob, LevelBlob& out) {
	out = LevelBlob{};
	if (blob.size() < 16)
		return false;
	uint32_t magic = 0;
	std::memcpy(&magic, blob.data(), 4);
	if (magic != kLevelMagic)
		return false;
	Cursor c{ blob.data() + 4, blob.size() - 4 };
	out.name = c.str();
	out.sceneKey = c.str();
	const uint32_t subCount = c.u32();
	if (!c.ok || subCount > 4096)
		return false;
	// Pass 1: kind/key columns.
	std::vector<PakAssetKind> kinds(subCount);
	std::vector<std::string> keys(subCount);
	for (uint32_t i = 0; i < subCount; ++i) {
		const uint32_t kind = c.u32();
		if (kind < 1 || kind > 10)
			return false; // unknown kind: fail closed (recook)
		kinds[i] = PakAssetKind(kind);
		keys[i] = c.str();
	}
	// Pass 2: offset/size columns.
	out.subs.resize(subCount);
	uint64_t payloadEnd = 0;
	for (uint32_t i = 0; i < subCount; ++i) {
		const uint64_t off = c.u64();
		const uint64_t size = c.u64();
		if (!c.ok)
			return false;
		LevelSubBlob& s = out.subs[i];
		s.kind = kinds[i];
		s.key = std::move(keys[i]);
		s.offset = off;
		s.size = size;
		if (off + size < off) // overflow
			return false;
		payloadEnd = payloadEnd > off + size ? payloadEnd : off + size;
	}
	// Payload: the remainder after the TOC. Trailing bytes past the last
	// sub-blob end are REJECTED (truncation/padding corruption).
	const size_t payloadStart = size_t(blob.size() - c.left);
	if (payloadStart > blob.size())
		return false;
	if (blob.size() - payloadStart < payloadEnd)
		return false;
	if (blob.size() - payloadStart != payloadEnd)
		return false;
	out.payload = blob.subspan(payloadStart, size_t(payloadEnd));
	return true;
}

std::vector<const PakTocEntry*> ListPakLevels(const PakReader& reader) {
	return reader.BlobsOfKind(PakAssetKind::Level);
}

} // namespace smvk
