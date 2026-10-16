// Level blob contract (PakAssetKind::Level): build/parse round-trip,
// pak carry, sub-blob addressing by canonical keys, fail-closed parses.
// GPU-less. The wire format lives in Engine/Asset/level.h.

#include <cstring>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <Engine/Asset/level.h>
#include <Engine/Asset/pak.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

} // namespace

int main() {
	using namespace smvk;

	// The sub-blob set: a scene payload + the cubemap set (manifest +
	// two KTX2 rows) + a GI payload — the bake association the level
	// owns, addressed by the canonical keys.
	const std::string sceneBytes = "SCENE-PAYLOAD-BYTES";
	const std::string manifest =
		"gi/cubemaps/world_0.ktx2 1 2 3 10 5\n"
		"gi/cubemaps/world_1.ktx2 4 5 6 10 5 pin\n";
	const std::string ktx0 = "KTX2-ROW-ZERO";
	const std::string ktx1 = "KTX2-ROW-ONE-ISH";
	const std::string sggi = "SGGI-WORLD-FIELD";

	std::vector<LevelSubInput> subs;
	subs.push_back({ PakAssetKind::Scene, "scene",
		std::span<const uint8_t>(
			reinterpret_cast<const uint8_t*>(sceneBytes.data()),
			sceneBytes.size()) });
	subs.push_back({ PakAssetKind::Texture,
		"gi/cubemaps/world.cubemanifest",
		std::span<const uint8_t>(
			reinterpret_cast<const uint8_t*>(manifest.data()),
			manifest.size()) });
	subs.push_back({ PakAssetKind::Texture, "gi/cubemaps/world_0.ktx2",
		std::span<const uint8_t>(
			reinterpret_cast<const uint8_t*>(ktx0.data()),
			ktx0.size()) });
	subs.push_back({ PakAssetKind::Texture, "gi/cubemaps/world_1.ktx2",
		std::span<const uint8_t>(
			reinterpret_cast<const uint8_t*>(ktx1.data()),
			ktx1.size()) });
	subs.push_back({ PakAssetKind::Gi, "gi/world.sggi",
		std::span<const uint8_t>(
			reinterpret_cast<const uint8_t*>(sggi.data()),
			sggi.size()) });

	// Build + parse round-trip (borrowed spans, byte-exact).
	std::vector<uint8_t> blob;
	Check(BuildLevelBlob("town", "scene", subs, blob), "level.build.ok");
	{
		LevelBlob lv;
		Check(ParseLevelBlob(blob, lv), "level.parse.ok");
		Check(lv.name == "town", "level.name");
		Check(lv.sceneKey == "scene", "level.sceneKey");
		Check(lv.subs.size() == subs.size(), "level.subcount");
		Check(lv.Find("scene") != nullptr, "level.find-scene");
		Check(lv.Find("gi/cubemaps/world_0.ktx2") != nullptr,
			"level.find-row0");
		Check(lv.Find("missing") == nullptr, "level.find-miss");
		// Byte-exact sub-blobs (the borrowed payload spans).
		const auto sceneSpan = lv.Bytes("scene");
		Check(sceneSpan.size() == sceneBytes.size()
			&& std::memcmp(sceneSpan.data(), sceneBytes.data(),
				sceneSpan.size()) == 0, "level.bytes-scene");
		const auto manifestSpan = lv.Bytes("gi/cubemaps/world.cubemanifest");
		Check(manifestSpan.size() == manifest.size()
			&& std::memcmp(manifestSpan.data(), manifest.data(),
				manifestSpan.size()) == 0, "level.bytes-manifest");
		const auto row1 = lv.Bytes("gi/cubemaps/world_1.ktx2");
		Check(row1.size() == ktx1.size()
			&& std::memcmp(row1.data(), ktx1.data(), row1.size()) == 0,
			"level.bytes-row1");
		const auto sggiSpan = lv.Bytes("gi/world.sggi");
		Check(sggiSpan.size() == sggi.size()
			&& std::memcmp(sggiSpan.data(), sggi.data(), sggiSpan.size())
				== 0, "level.bytes-sggi");
		// Sub-blob kinds round-trip.
		Check(lv.Find("scene")->kind == PakAssetKind::Scene,
			"level.kind-scene");
		Check(lv.Find("gi/cubemaps/world_0.ktx2")->kind
			== PakAssetKind::Texture, "level.kind-row");
		Check(lv.Find("gi/world.sggi")->kind == PakAssetKind::Gi,
			"level.kind-gi");
	}

	// Fail-closed parses: bad magic, truncation, oversized sub-blob,
	// trailing bytes, bad kind.
	{
		LevelBlob lv;
		std::vector<uint8_t> bad = blob;
		bad[0] ^= 0xFF;
		Check(!ParseLevelBlob(bad, lv), "level.reject-magic");
		bad = blob;
		bad.pop_back();
		Check(!ParseLevelBlob(bad, lv), "level.reject-truncated");
		// Oversized sub-blob: corrupt the first sub's size column (the
		// u64 right after the offset column). Locate it by rebuilding with
		// a known shape instead: 1 sub, hand-forge the size huge.
		std::vector<uint8_t> forge;
		forge.insert(forge.end(), { 0x4C, 0x56, 0x4C, 0x31 }); // LVL1 LE
		const char* n = "x";
		uint32_t nlen = 1;
		forge.insert(forge.end(), (const uint8_t*)&nlen,
			(const uint8_t*)&nlen + 4);
		forge.push_back(uint8_t(*n));
		uint32_t sklen = 0;
		forge.insert(forge.end(), (const uint8_t*)&sklen,
			(const uint8_t*)&sklen + 4);
		uint32_t count = 1;
		forge.insert(forge.end(), (const uint8_t*)&count,
			(const uint8_t*)&count + 4);
		uint32_t kind = 1;
		forge.insert(forge.end(), (const uint8_t*)&kind,
			(const uint8_t*)&kind + 4);
		uint32_t klen = 1;
		forge.insert(forge.end(), (const uint8_t*)&klen,
			(const uint8_t*)&klen + 4);
		forge.push_back('k');
		uint64_t off = 0, size = 1ull << 40; // huge
		forge.insert(forge.end(), (const uint8_t*)&off,
			(const uint8_t*)&off + 8);
		forge.insert(forge.end(), (const uint8_t*)&size,
			(const uint8_t*)&size + 8);
		forge.push_back(0xAB);
		Check(!ParseLevelBlob(forge, lv), "level.reject-oversize");
		// Bad kind (0 or > 10): reuse the forge with kind=99.
		forge.clear();
		forge.insert(forge.end(), { 0x4C, 0x56, 0x4C, 0x31 });
		forge.insert(forge.end(), (const uint8_t*)&nlen,
			(const uint8_t*)&nlen + 4);
		forge.push_back(uint8_t(*n));
		forge.insert(forge.end(), (const uint8_t*)&sklen,
			(const uint8_t*)&sklen + 4);
		forge.insert(forge.end(), (const uint8_t*)&count,
			(const uint8_t*)&count + 4);
		kind = 99;
		forge.insert(forge.end(), (const uint8_t*)&kind,
			(const uint8_t*)&kind + 4);
		forge.insert(forge.end(), (const uint8_t*)&klen,
			(const uint8_t*)&klen + 4);
		forge.push_back('k');
		off = 0;
		size = 1;
		forge.insert(forge.end(), (const uint8_t*)&off,
			(const uint8_t*)&off + 8);
		forge.insert(forge.end(), (const uint8_t*)&size,
			(const uint8_t*)&size + 8);
		forge.push_back(0xAB);
		Check(!ParseLevelBlob(forge, lv), "level.reject-bad-kind");
		// Trailing bytes past the last sub-blob end: rejected.
		std::vector<uint8_t> trailing = blob;
		trailing.push_back(0x00);
		Check(!ParseLevelBlob(trailing, lv), "level.reject-trailing");
	}

	// Pak carry: the level rides as PakAssetKind::Level at
	// levels/<name>.level (uncompressed: Payload() = the span), lists +
	// parses through the production reader.
	{
		PakWriter writer;
		Check(writer.AddBlob(PakAssetKind::Level, "levels/town.level",
			std::vector<uint8_t>(blob)), "level.pak-add");
		std::vector<uint8_t> image = writer.Finalize();
		std::string err;
		auto reader = PakReader::Open(
			std::span<const uint8_t>(image), err);
		Check(reader.has_value(), "level.pak-open");
		if (!reader)
			return 1;
		auto levels = ListPakLevels(*reader);
		Check(levels.size() == 1, "level.pak-list");
		if (!levels.empty()) {
			LevelBlob lv;
			Check(ParseLevelBlob(reader->Payload(*levels[0]), lv),
				"level.pak-parse");
			Check(lv.name == "town", "level.pak-name");
			const auto row0 = lv.Bytes("gi/cubemaps/world_0.ktx2");
			Check(row0.size() == ktx0.size()
				&& std::memcmp(row0.data(), ktx0.data(), row0.size()) == 0,
				"level.pak-bytes-row0");
		}
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
