// asset_tests: headless unit tests for VFS + pak container + asset URIs +
// the meshopt cook pass. No GPU required (nothing here constructs a
// ResourceManager); run via CTest.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/App/static_merge.h>
#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/compression.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Asset/pak.h>
#include <Engine/Renderer/vertex_pack.h>
#include <Engine/Scene/scene.h>
#include <Engine/Vfs/vfs.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

void TestVfsNormalize() {
	using smvk::vfs::Normalize;
	Check(Normalize("models/Sponza/sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.norm.plain");
	Check(Normalize("models\\Sponza\\sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.norm.backslash");
	Check(Normalize("./models//Sponza/./sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.norm.dots");
	Check(Normalize("models/Sponza/../Sponza/sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.norm.dotdot");
	Check(Normalize("") == "", "vfs.norm.empty");
	Check(Normalize("/absolute/path") == "", "vfs.norm.absolute");
	Check(Normalize("C:/win/path") == "", "vfs.norm.drive");
	Check(Normalize("../escape") == "", "vfs.norm.escape");
	Check(Normalize("a/../../escape") == "", "vfs.norm.escape2");
	Check(smvk::vfs::Join("models/Sponza", "sponza.gltf") == "models/Sponza/sponza.gltf", "vfs.join");
	Check(smvk::vfs::BaseDir("models/Sponza/sponza.gltf") == "models/Sponza", "vfs.basedir");
	Check(smvk::vfs::FileName("models/Sponza/sponza.gltf") == "sponza.gltf", "vfs.filename");
}

void TestOverlayMem() {
	smvk::OverlayVfs vfs;
	auto base = std::make_shared<smvk::MemBackend>("base");
	auto over = std::make_shared<smvk::MemBackend>("over");
	base->Write("textures/a.ktx2", { 1, 2, 3 });
	base->Write("textures/b.ktx2", { 4, 5 });
	over->Write("textures/a.ktx2", { 9, 9, 9, 9 });
	vfs.Mount(base, 10, "base");
	vfs.Mount(over, 0, "over"); // higher precedence

	auto a = vfs.TryRead("textures/a.ktx2");
	Check(a && a->size() == 4 && (*a)[0] == 9, "vfs.overlay.precedence");
	auto b = vfs.TryRead("textures/b.ktx2");
	Check(b && b->size() == 2, "vfs.overlay.fallthrough");
	Check(!vfs.Exists("textures/missing.ktx2"), "vfs.overlay.miss");
	auto providers = vfs.FindProviders("textures/a.ktx2");
	Check(providers.size() == 2, "vfs.overlay.providers");
	auto dir = vfs.ListDir("textures");
	Check(dir.size() == 2, "vfs.overlay.listdir");
	bool threw = false;
	try {
		vfs.ReadBinary("nope.bin");
	}
	catch (const std::runtime_error&) {
		threw = true;
	}
	Check(threw, "vfs.overlay.throw");
	Check(vfs.Unmount("over"), "vfs.unmount");
	auto a2 = vfs.TryRead("textures/a.ktx2");
	Check(a2 && a2->size() == 3, "vfs.unmount.fallback");
}

void TestAssetUris() {
	auto [f, frag] = smvk::SplitAssetUri("models/s.gltf#mesh.1/prim.0");
	Check(f == "models/s.gltf" && frag == "mesh.1/prim.0", "uri.split");
	Check(smvk::NormalizeAssetUri("models\\s.gltf#mesh.1") == "models/s.gltf#mesh.1", "uri.normalize");
	Check(smvk::NormalizeAssetUri("../evil#x") == "", "uri.escape");
	Check(smvk::MakeSubUri("models/s.gltf", "mat.3") == "models/s.gltf#mat.3", "uri.mksub");
	Check(smvk::AssetFilePart("models/s.gltf#mat.3") == "models/s.gltf", "uri.filepart");
}

void TestStringViewArena() {
	using namespace smvk;
	// StringViewT basics: slicing, search, compare, char* interop.
	StringViewT v = "models/b.gltf#mesh.3";
	Check(v.size() == 19 && !v.empty(), "sv.size");
	Check(v.substr(0, 10) == "models/b.g", "sv.substr");
	Check(v.find('#') == 12, "sv.find");
	Check(v.find("mesh") == 13, "sv.findstr");
	Check(v.starts_with("models") && v.ends_with(".3"), "sv.prefix");
	Check(v.contains('#') && !v.contains('?'), "sv.contains");
	Check(StringViewT("a/b").rfind('/') == 1, "sv.rfind");
	Check(StringViewT("  hi  ").trim() == "hi", "sv.trim");
	Check(StringViewT() == "" && StringViewT("").valid(), "sv.emptyview");
	Check(StringViewT("abc").Str() == std::string("abc"), "sv.str");
	Check(StringViewT("b") > StringViewT("a") && StringViewT("a") < StringViewT("ab"), "sv.order");

	// Arena: bump allocation, string copies, format, reset.
	Arena a(256);
	StringViewT s = a.AllocateString("hello world");
	Check(s == "hello world", "arena.string");
	const uint64_t* big = a.Create<uint64_t>(0xDEADBEEFCAFEull);
	Check(*big == 0xDEADBEEFCAFEull, "arena.create");
	StringViewT f = a.FormatString("%s/%u", "mesh", 7u);
	Check(f == "mesh/7", "arena.format");
	const size_t used = a.UsedBytes();
	Check(used > 0, "arena.used");
	Check(a.BlockCount() >= 1, "arena.blocks");
	// Oversized request gets a dedicated block.
	void* huge = a.Allocate(a.BlockCount() * 1024 * 1024 + 4096);
	Check(huge != nullptr, "arena.huge");
	a.Reset();
	Check(a.UsedBytes() == 0 && a.BlockCount() == 0, "arena.reset");

	// ConcatInto composes without std::string.
	Arena b(256);
	Check(ConcatInto(b, "models", "/q.gltf") == "models/q.gltf", "arena.concat");
	Check(ConcatInto(b, "", "x") == "x" && ConcatInto(b, "x", "") == "x", "arena.concat.empty");

	// UriMap: insert/find/erase/tombstones/growth over string keys.
	UriMap<uint32_t> m;
	Check(m.Find("missing") == nullptr, "umap.find.miss");
	m.Insert("models/a.gltf#mesh.0", 0);
	m.Insert("models/a.gltf#mesh.1", 1);
	m.Insert("models/b.gltf#mesh.0", 2);
	Check(m.Size() == 3, "umap.size");
	Check(m.Find("models/a.gltf#mesh.0") && *m.Find("models/a.gltf#mesh.0") == 0, "umap.find");
	Check(*m.Find("models/b.gltf#mesh.0") == 2, "umap.find2");
	m.Insert("models/a.gltf#mesh.1", 11); // replace keeps one entry
	Check(m.Size() == 3 && *m.Find("models/a.gltf#mesh.1") == 11, "umap.replace");
	Check(m.Erase("models/a.gltf#mesh.0"), "umap.erase");
	Check(!m.Find("models/a.gltf#mesh.0"), "umap.erase.gone");
	// Stress growth + tombstone probing.
	UriMap<int> big2;
	std::vector<StringViewT> keys;
	keys.reserve(500);
	for (int i = 0; i < 500; ++i) {
		keys.push_back(AppArena().FormatString("stress/key.%d", i));
		big2.Insert(keys.back(), i);
		if (i % 3 == 0)
			big2.Erase(keys.back());
	}
	int found = 0, missing = 0;
	for (int i = 0; i < 500; ++i) {
		if (i % 3 == 0) {
			if (!big2.Find(keys[i]))
				++missing;
		}
		else if (big2.Find(keys[i]) && *big2.Find(keys[i]) == i)
			++found;
	}
	Check(found == 334 && missing == 167, "umap.stress");
}

void TestPakRoundTrip() {
	smvk::PakWriter w;
	Check(!w.AddBlob(smvk::PakAssetKind::Texture, "../evil", { 1 }), "pak.reject.escape");
	Check(w.AddBlob(smvk::PakAssetKind::Texture, "textures/a.ktx2", { 1, 2, 3, 4 }), "pak.add.tex");
	std::vector<uint8_t> scene = { 9, 9, 9 };
	Check(w.AddBlob(smvk::PakAssetKind::Scene, "<cluster-local>#scene", scene), "pak.add.scene");
	std::vector<uint8_t> img = w.Finalize();
	Check(img.size() > 64, "pak.finalize.size");

	std::string err;
	auto r = smvk::PakReader::Open(std::span<const uint8_t>(img), err);
	Check(r.has_value(), "pak.open.ok");
	if (!r) {
		std::cout << "  pak error: " << err << "\n";
		return;
	}
	Check(r->EntryCount() == 2, "pak.entry.count");
	const auto* tex = r->Find(smvk::PakAssetKind::Texture, "textures/a.ktx2");
	Check(tex != nullptr, "pak.find.tex");
	if (tex) {
		auto payload = r->Payload(*tex);
		Check(payload.size() == 4 && payload[0] == 1, "pak.payload");
	}
	Check(r->Find(smvk::PakAssetKind::Mesh, "textures/a.ktx2") == nullptr, "pak.find.kindmiss");

	// Corruption is detected.
	std::vector<uint8_t> bad = img;
	bad[40] ^= 0xFF;
	std::string err2;
	Check(!smvk::PakReader::Open(std::span<const uint8_t>(bad), err2), "pak.corrupt.detected");

	// Texture payload helpers.
	auto tp = smvk::BuildTexturePayload("textures/a.ktx2", 1, 2, 3, 4,
		std::span<const uint8_t>(std::vector<uint8_t>{ 7, 8 }));
	smvk::PakTexturePayload parsed;
	Check(smvk::ParseTexturePayload(tp, parsed), "pak.texpayload.parse");
	Check(parsed.uri == "textures/a.ktx2" && parsed.magFilter == 1 &&
		parsed.minFilter == 2 && parsed.wrapS == 3 && parsed.wrapT == 4 &&
		parsed.ktxBytes.size() == 2 && parsed.ktxBytes[0] == 7, "pak.texpayload.fields");
}

// CRC-32 combine (the primitive behind the parallel whole-image verify
// in PakReader::Open): linear over GF(2), so the CRC of a concatenation
// rebuilds from part CRCs. Deterministic LCG buffers, no <random> dep.
void TestPakCrcCombine() {
	auto fill = [](std::vector<uint8_t>& v, uint32_t seed) {
		uint32_t s = seed;
		for (auto& b : v) {
			s = s * 1664525u + 1013904223u;
			b = static_cast<uint8_t>(s >> 24);
		}
	};
	std::vector<uint8_t> a(4096), b(2048), c(513);
	fill(a, 1);
	fill(b, 2);
	fill(c, 3);
	const uint32_t crcA = smvk::PakCrc32(a.data(), a.size());
	const uint32_t crcB = smvk::PakCrc32(b.data(), b.size());
	const uint32_t crcC = smvk::PakCrc32(c.data(), c.size());
	std::vector<uint8_t> ab(a);
	ab.insert(ab.end(), b.begin(), b.end());
	std::vector<uint8_t> abc(ab);
	abc.insert(abc.end(), c.begin(), c.end());
	const uint32_t whole = smvk::PakCrc32(abc.data(), abc.size());
	// Two-way combine matches the CRC of the concatenation.
	Check(smvk::PakCrc32Combine(crcA, crcB, b.size())
		== smvk::PakCrc32(ab.data(), ab.size()), "pak.crc.combine.two");
	// Right-to-left fold (exactly what PakCrc32Parallel does).
	const uint32_t bc = smvk::PakCrc32Combine(crcB, crcC, c.size());
	Check(smvk::PakCrc32Combine(crcA, bc, b.size() + c.size()) == whole,
		"pak.crc.combine.three");
	// Empty tail keeps crc1.
	Check(smvk::PakCrc32Combine(0xDEADBEEFu, 0x12345678u, 0) == 0xDEADBEEFu,
		"pak.crc.combine.empty");
}

// Chunk-parallel CRC: pool-folded chunks must equal the serial walk.
// Small chunk sizes keep the test light while covering the multi-chunk
// fold path (the 8 MB default only kicks in for real multi-GB paks).
void TestPakCrcParallel() {
	std::vector<uint8_t> data(256 * 1024);
	uint32_t s = 7;
	for (auto& b : data) {
		s = s * 1664525u + 1013904223u;
		b = static_cast<uint8_t>(s >> 24);
	}
	const uint32_t serial = smvk::PakCrc32(data.data(), data.size());
	Check(smvk::PakCrc32Parallel(std::span<const uint8_t>(data),
			100 * 1024) == serial, "pak.crc.parallel.fold");
	// Chunk >= size stays serial inside.
	Check(smvk::PakCrc32Parallel(std::span<const uint8_t>(data),
			data.size() + 1) == serial, "pak.crc.parallel.singlechunk");
	// Degenerate empty input.
	Check(smvk::PakCrc32Parallel({}) == 0u, "pak.crc.parallel.empty");
}

// Tail-upload digest: kind + tailFirstMip round-trip, embedded in a v4
// payload and read back through both payload parsers (V4 finds the
// digest; the legacy parser tolerates the digest-less shape only).
void TestPakTextureDigest() {
	smvk::TextureStreamMeta meta{};
	meta.tailFirstMip = 2;
	std::vector<uint8_t> digestBytes;
	Check(smvk::BuildTextureDigest(meta, digestBytes), "pak.digest.build.ok");
	Check(digestBytes.size() == 8, "pak.digest.build.bytes");
	smvk::TextureStreamMeta back{};
	Check(smvk::ParseTextureDigest(std::span<const uint8_t>(digestBytes), back),
		"pak.digest.parse.ok");
	Check(back.tailFirstMip == 2, "pak.digest.fields");
	// Garbage is rejected (wrong kind, short/long buffer).
	std::vector<uint8_t> bad = digestBytes;
	bad[0] ^= 0xFF;
	smvk::TextureStreamMeta junk{};
	Check(!smvk::ParseTextureDigest(std::span<const uint8_t>(bad), junk),
		"pak.digest.reject.kind");
	Check(!smvk::ParseTextureDigest(
			std::span<const uint8_t>(digestBytes.data(), 7), junk),
		"pak.digest.reject.short");
	Check(!smvk::ParseTextureDigest(
			std::span<const uint8_t>(digestBytes.data(), 9), junk),
		"pak.digest.reject.long");
	// Payload embedding: V4 parser finds the digest, legacy parser
	// rejects the digested shape but accepts the digest-less shape.
	const std::vector<uint8_t> ktx = { 9, 8, 7, 6 };
	auto blob = smvk::BuildTexturePayloadV4("textures/d.ktx2", 1, 2, 3, 4,
		std::span<const uint8_t>(ktx),
		std::span<const uint8_t>(digestBytes));
	smvk::PakTexturePayload p4;
	Check(smvk::ParseTexturePayloadV4(std::span<const uint8_t>(blob), p4),
		"pak.digest.payload.v4.parse");
	smvk::TextureStreamMeta m4{};
	Check(smvk::ParseTextureDigest(p4.digest, m4) && m4.tailFirstMip == 2,
		"pak.digest.payload.v4.fields");
	Check(p4.ktxBytes.size() == 4
		&& p4.ktxBytes[0] == 9 && p4.uri == "textures/d.ktx2",
		"pak.digest.payload.v4.bytes");
	smvk::PakTexturePayload legacy;
	Check(!smvk::ParseTexturePayload(std::span<const uint8_t>(blob), legacy),
		"pak.digest.payload.legacy.rejects.digest");
	auto blobLegacy = smvk::BuildTexturePayload("textures/e.ktx2", 1, 2, 3, 4,
		std::span<const uint8_t>(ktx));
	smvk::PakTexturePayload p3;
	Check(smvk::ParseTexturePayload(std::span<const uint8_t>(blobLegacy), p3),
		"pak.digest.payload.legacy.parse");
	Check(p3.digest.empty() && p3.ktxBytes.size() == 4,
		"pak.digest.payload.legacy.fields");
}

void TestPakAppend() {
	namespace fs = std::filesystem;
	const auto dir = fs::temp_directory_path() / "tourmaline_pak_append";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const std::string pakPath = (dir / "grown.pak").string();

	std::string err;
	std::vector<smvk::PakAppendInfo> infos;
	// Fresh file: batch of two, then read back through PakReader.
	{
		const std::vector<uint8_t> a = { 1, 2, 3, 4, 5 };
		const std::vector<uint8_t> b(200, 0x5A);
		std::vector<std::pair<std::string, std::span<const uint8_t>>>
			blobs = { { "textures/a.bin", a }, { "textures/b.bin", b } };
		Check(smvk::AppendPakBlobs(pakPath,
				smvk::PakAssetKind::Texture, blobs, infos, err),
			"pak.append.fresh");
		Check(infos.size() == 2
			&& infos[1].offset > infos[0].offset, "pak.append.info");
	}
	{
		std::vector<uint8_t> img;
		std::ifstream f(pakPath, std::ios::binary);
		f.seekg(0, std::ios::end);
		img.resize(size_t(f.tellg()));
		f.seekg(0);
		f.read(reinterpret_cast<char*>(img.data()),
			std::streamsize(img.size()));
		auto r = smvk::PakReader::Open(
			std::span<const uint8_t>(img), err);
		Check(r.has_value(), "pak.append.reader.open");
		if (r) {
			Check(r->EntryCount() == 2, "pak.append.entries");
			const auto* eA = r->Find(smvk::PakAssetKind::Texture,
				"textures/a.bin");
			Check(eA != nullptr, "pak.append.find.a");
			if (eA) {
				auto p = r->Payload(*eA);
				Check(p.size() == 5 && p[0] == 1, "pak.append.payload.a");
				// Direct (offset, size) read matches Find (the
				// registry-read contract: uncompressed entries).
				Check(eA->offset == infos[0].offset
					&& eA->size == infos[0].size,
					"pak.append.direct.addressing");
			}
			const auto* eB = r->Find(smvk::PakAssetKind::Texture,
				"textures/b.bin");
			Check(eB != nullptr && r->Payload(*eB).size() == 200,
				"pak.append.payload.b");
		}
	}
	// Grow the same file with a third blob + a superseding update: the
	// TOC must stay unique-by-path and Find returns the LATEST payload.
	{
		const std::vector<uint8_t> c = { 9, 9 };
		const std::vector<uint8_t> a2 = { 6, 7, 8 };
		std::vector<std::pair<std::string, std::span<const uint8_t>>>
			blobs = { { "textures/c.bin", c },
				{ "textures/a.bin", a2 } };
		Check(smvk::AppendPakBlobs(pakPath,
				smvk::PakAssetKind::Texture, blobs, infos, err),
			"pak.append.grow");
	}
	{
		std::vector<uint8_t> img;
		std::ifstream f(pakPath, std::ios::binary);
		f.seekg(0, std::ios::end);
		img.resize(size_t(f.tellg()));
		f.seekg(0);
		f.read(reinterpret_cast<char*>(img.data()),
			std::streamsize(img.size()));
		auto r = smvk::PakReader::Open(
			std::span<const uint8_t>(img), err);
		Check(r.has_value(), "pak.append.grow.reader.open");
		if (r) {
			Check(r->EntryCount() == 3, "pak.append.grow.unique.toc");
			const auto* eA = r->Find(smvk::PakAssetKind::Texture,
				"textures/a.bin");
			Check(eA != nullptr && eA->size == 3
				&& r->Payload(*eA)[0] == 6,
				"pak.append.grow.latest.wins");
			Check(r->Find(smvk::PakAssetKind::Texture, "textures/c.bin")
				!= nullptr, "pak.append.grow.find.c");
		}
	}
	// Interop: a PakWriter-produced (LZ4) pak still grows + reads.
	{
		smvk::PakWriter w;
		w.SetCompression(smvk::PakCompression::Lz4, 1);
		Check(w.AddBlob(smvk::PakAssetKind::Texture,
				"textures/w.ktx2",
				std::vector<uint8_t>(4096, 0x33)), "pak.append.w.add");
		const auto img = w.Finalize();
		const std::string lzPath = (dir / "writer.pak").string();
		{
			std::ofstream f(lzPath, std::ios::binary);
			f.write(reinterpret_cast<const char*>(img.data()),
				std::streamsize(img.size()));
		}
		const std::vector<uint8_t> n = { 4, 4, 4 };
		std::vector<std::pair<std::string, std::span<const uint8_t>>>
			blobs = { { "textures/append.ktx2", n } };
		Check(smvk::AppendPakBlobs(lzPath,
				smvk::PakAssetKind::Texture, blobs, infos, err),
			"pak.append.into.writer.pak");
		std::vector<uint8_t> img2;
		std::ifstream f(lzPath, std::ios::binary);
		f.seekg(0, std::ios::end);
		img2.resize(size_t(f.tellg()));
		f.seekg(0);
		f.read(reinterpret_cast<char*>(img2.data()),
			std::streamsize(img2.size()));
		auto r = smvk::PakReader::Open(
			std::span<const uint8_t>(img2), err);
		Check(r.has_value(), "pak.append.writer.reader.open");
		if (r) {
			Check(r->Find(smvk::PakAssetKind::Texture, "textures/w.ktx2")
				&& r->Find(smvk::PakAssetKind::Texture,
					"textures/append.ktx2") != nullptr,
				"pak.append.writer.both.served");
		}
	}
	fs::remove_all(dir, ec);
}

void TestLz4Codec() {
	// Highly compressible data round-trips.
	std::vector<uint8_t> raw(64 * 1024, 0xAB);
	for (size_t i = 0; i < raw.size(); i += 64)
		raw[i] = static_cast<uint8_t>(i >> 6);
	std::vector<uint8_t> comp;
	Check(smvk::Lz4Compress(std::span<const uint8_t>(raw), comp), "lz4.compress.ok");
	Check(!comp.empty() && comp.size() < raw.size(), "lz4.compress.shrinks");
	std::vector<uint8_t> back;
	Check(smvk::Lz4Decompress(std::span<const uint8_t>(comp), raw.size(), back), "lz4.decompress.ok");
	Check(back == raw, "lz4.roundtrip.bytes");
	// Wrong output size fails instead of overrunning.
	std::vector<uint8_t> wrong;
	Check(!smvk::Lz4Decompress(std::span<const uint8_t>(comp), raw.size() + 1, wrong), "lz4.decompress.sizemiss");
	// Truncated block fails.
	std::vector<uint8_t> trunc(comp.begin(), comp.end() - 1);
	std::vector<uint8_t> truncOut;
	Check(!smvk::Lz4Decompress(std::span<const uint8_t>(trunc), raw.size(), truncOut), "lz4.decompress.trunc");
	// Empty input rejected.
	std::vector<uint8_t> emptyOut;
	Check(!smvk::Lz4Compress(std::span<const uint8_t>{}, emptyOut), "lz4.compress.empty");
	// Blob helpers: tiny payloads stay stored verbatim.
	std::vector<uint8_t> tiny = { 1, 2, 3, 4 };
	auto [tinyStored, tinyFlag] = smvk::CompressPakBlob(
		std::span<const uint8_t>(tiny), smvk::PakCompression::Lz4);
	Check(!tinyFlag && tinyStored == tiny, "lz4.blob.tiny.raw");
	// None mode never compresses.
	auto [rawStored, rawFlag] = smvk::CompressPakBlob(
		std::span<const uint8_t>(raw), smvk::PakCompression::None);
	Check(!rawFlag && rawStored == raw, "lz4.blob.none.raw");
	// LZ4 mode compresses the big blob with the 8-byte size prefix.
	auto [lz4Stored, lz4Flag] = smvk::CompressPakBlob(
		std::span<const uint8_t>(raw), smvk::PakCompression::Lz4);
	Check(lz4Flag && lz4Stored.size() < raw.size(), "lz4.blob.shrinks");
	std::vector<uint8_t> blobBack;
	Check(smvk::DecompressPakBlob(std::span<const uint8_t>(lz4Stored), true, blobBack), "lz4.blob.parse");
	Check(blobBack == raw, "lz4.blob.roundtrip");
}

void TestPakCompressedRoundTrip() {
	smvk::PakWriter w;
	w.SetCompression(smvk::PakCompression::Lz4);
	// Compressible mesh-like payload (repeated vertex-ish pattern).
	std::vector<uint8_t> big(32 * 1024, 0);
	for (size_t i = 0; i < big.size(); ++i)
		big[i] = static_cast<uint8_t>((i * 7) & 0xFF);
	Check(w.AddBlob(smvk::PakAssetKind::Mesh, "models/big#mesh.0", big), "paklz4.add.big");
	std::vector<uint8_t> scene = { 9, 9, 9 };
	Check(w.AddBlob(smvk::PakAssetKind::Scene, "<cluster-local>#scene", scene), "paklz4.add.scene");
	Check(w.CompressedEntryCount() == 1, "paklz4.compressed.count");
	std::vector<uint8_t> img = w.Finalize();
	Check(img.size() > 64, "paklz4.finalize.size");

	std::string err;
	auto r = smvk::PakReader::Open(std::span<const uint8_t>(img), err);
	Check(r.has_value(), "paklz4.open.ok");
	if (!r) {
		std::cout << "  paklz4 error: " << err << "\n";
		return;
	}
	Check(r->CompressedEntryCount() == 1, "paklz4.reader.count");
	const auto* mesh = r->Find(smvk::PakAssetKind::Mesh, "models/big#mesh.0");
	Check(mesh != nullptr, "paklz4.find.mesh");
	if (mesh) {
		auto payload = r->Payload(*mesh);
		Check(payload.size() == big.size() &&
			memcmp(payload.data(), big.data(), big.size()) == 0, "paklz4.payload.bytes");
	}
	// Corruption inside a compressed blob is detected at Open().
	std::vector<uint8_t> bad = img;
	if (bad.size() > 80)
		bad[60] ^= 0xFF;
	std::string err2;
	Check(!smvk::PakReader::Open(std::span<const uint8_t>(bad), err2), "paklz4.corrupt.detected");

	// Compressed textures stay servable through the VFS backend.
	smvk::PakWriter w2;
	w2.SetCompression(smvk::PakCompression::Lz4);
	std::vector<uint8_t> ktx(4096, 0x5A);
	auto tp = smvk::BuildTexturePayload("textures/big.ktx2", 1, 2, 3, 4,
		std::span<const uint8_t>(ktx));
	w2.AddBlob(smvk::PakAssetKind::Texture, "textures/big.ktx2", std::move(tp));
	w2.AddBlob(smvk::PakAssetKind::Scene, "<cluster-local>#scene", { 1 });
	auto img2 = std::make_shared<const std::vector<uint8_t>>(w2.Finalize());
	auto backend = smvk::PakBackend::Open(img2, "lz4.pak", err);
	Check(backend != nullptr, "paklz4.backend.open");
	if (backend) {
		smvk::OverlayVfs vfs;
		vfs.Mount(backend, 0, "lz4.pak");
		auto bytes = vfs.TryRead("textures/big.ktx2");
		Check(bytes && bytes->size() == ktx.size() && (*bytes)[0] == 0x5A, "paklz4.backend.serve");
	}
}

void TestPakBackendServesTextures() {
	smvk::PakWriter w;
	auto tp = smvk::BuildTexturePayload("textures/brick.ktx2", -1, -1, -1, -1,
		std::span<const uint8_t>(std::vector<uint8_t>{ 10, 20, 30 }));
	w.AddBlob(smvk::PakAssetKind::Texture, "textures/brick.ktx2", std::move(tp));
	std::vector<uint8_t> scene = { 1 };
	w.AddBlob(smvk::PakAssetKind::Scene, "<cluster-local>#scene", scene);
	auto img = std::make_shared<const std::vector<uint8_t>>(w.Finalize());
	std::string err;
	auto backend = smvk::PakBackend::Open(img, "test.pak", err);
	Check(backend != nullptr, "pakbackend.open");
	if (!backend) {
		std::cout << "  backend error: " << err << "\n";
		return;
	}
	smvk::OverlayVfs vfs;
	vfs.Mount(backend, 0, "test.pak");
	auto bytes = vfs.TryRead("textures/brick.ktx2");
	Check(bytes && bytes->size() == 3 && (*bytes)[0] == 10, "pakbackend.serve.ktx");
	Check(!vfs.Exists("models/s.gltf"), "pakbackend.nontex.invisible");
	auto dir = vfs.ListDir("textures");
	Check(dir.size() == 1 && dir[0] == "textures/brick.ktx2", "pakbackend.listdir");
}

smvk::StaticMeshUpload MakeQuad() {
	smvk::StaticMeshUpload u;
	for (int i = 0; i < 4; ++i) {
		smvk::StaticVertex v{};
		v.pos = { (i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f, 0.0f };
		v.color = { 1, 1, 1, 1 };
		v.texCoord = { v.pos.x, v.pos.y };
		v.normal = { 0, 0, 1 };
		v.tangent = { 1, 0, 0, 1 };
		u.vertices.push_back(v);
	}
	u.indices = { 0, 1, 2, 0, 2, 3 };
	u.aabbMin = { 0, 0, 0 };
	u.aabbMax = { 1, 1, 0 };
	return u;
}

// 3x3 vertex grid on z=0 (4 quads, 8 triangles): left quads chart 0,
// right quads chart 1, seam column x=1 duplicated per chart with
// DIFFERENT chart-local uvs (position-identical, like the bake's
// per-corner stamping). Chart uvs are affine in position per chart —
// the exact case where simplified triangles reuse LOD0's
// parameterization without error. chartOf receives one chart id per
// vertex (parallel to the vertices); each vertex also carries its id
// in chartId, like a stamped cook upload.
smvk::StaticMeshUpload MakeTwoChartGrid(std::vector<uint32_t>& chartOf) {
	chartOf.clear();
	smvk::StaticMeshUpload u;
	auto vert = [&](float x, float y, uint32_t chart, float cu, float cv) {
		smvk::StaticVertex v{};
		v.pos = { x, y, 0.0f };
		v.color = { 1, 1, 1, 1 };
		v.texCoord = { x * 0.5f, y * 0.5f };
		v.texCoord2 = { cu, cv };
		v.normal = { 0, 0, 1 };
		v.tangent = { 1, 0, 0, 1 };
		v.chartId = chart;
		chartOf.push_back(chart);
		u.vertices.push_back(v);
	};
	// Chart uv extents stay well under kLodCrossChartUvSpan (0.45):
	// within-chart triangles span at most |du|+|dv| = 0.075+0.30 = 0.375,
	// while any cross-chart corner pair spans >= 0.625.
	for (int y = 0; y <= 2; ++y)
		vert(0.0f, float(y), 0, 0.10f, 0.10f + 0.15f * float(y));
	for (int y = 0; y <= 2; ++y)
		vert(1.0f, float(y), 0, 0.175f, 0.10f + 0.15f * float(y));
	for (int y = 0; y <= 2; ++y)
		vert(1.0f, float(y), 1, 0.80f, 0.10f + 0.15f * float(y));
	for (int y = 0; y <= 2; ++y)
		vert(2.0f, float(y), 1, 0.875f, 0.10f + 0.15f * float(y));
	// chart 0: verts 0..5 (x=0: 0,1,2 | x=1: 3,4,5)
	// chart 1: verts 6..11 (x=1: 6,7,8 | x=2: 9,10,11)
	auto id = [](int x, int y, int chart) -> uint32_t {
		return chart == 0
			? (x == 0 ? uint32_t(y) : 3u + uint32_t(y))
			: (x == 1 ? 6u + uint32_t(y) : 9u + uint32_t(y));
	};
	for (int y = 0; y < 2; ++y) {
		// Left quad (chart 0).
		const uint32_t a = id(0, y, 0), b = id(1, y, 0),
			c = id(0, y + 1, 0), d = id(1, y + 1, 0);
		u.indices.insert(u.indices.end(), { a, b, c, b, d, c });
		// Right quad (chart 1).
		const uint32_t a2 = id(1, y, 1), b2 = id(2, y, 1),
			c2 = id(1, y + 1, 1), d2 = id(2, y + 1, 1);
		u.indices.insert(u.indices.end(), { a2, b2, c2, b2, d2, c2 });
	}
	u.aabbMin = { 0.0f, 0.0f, 0.0f };
	u.aabbMax = { 2.0f, 2.0f, 0.0f };
	u.surfaces.push_back({ 0, uint32_t(u.indices.size()), 0 });
	return u;
}

void TestMeshOpt() {
	smvk::MeshOptConfig cfg; // all passes on
	auto quad = MakeQuad();
	smvk::MeshOptStats stats;
	smvk::LodInfo lod;
	Check(smvk::OptimizeStaticMeshUpload(quad, cfg, 7, &stats, &lod), "meshopt.quad.ok");
	Check(quad.vertices.size() == 4 && quad.indices.size() == 6, "meshopt.quad.topology");
	Check(stats.triangles == 2 && stats.lodGroupId == 7, "meshopt.quad.stats");
	Check(stats.acmrAfter > 0.0f && stats.acmrAfter <= stats.acmrBefore + 1e-5f, "meshopt.quad.acmr");
	Check(lod.lodIndex == 0 && lod.screenThreshold < 0.0f, "meshopt.lod.unplugged");
	Check(lod.triCount == 2, "meshopt.lod.tris");

	// Weld merges duplicated vertices.
	auto dup = MakeQuad();
	dup.vertices.push_back(dup.vertices[0]);
	dup.vertices.push_back(dup.vertices[1]);
	dup.vertices.push_back(dup.vertices[2]);
	dup.indices = { 4, 5, 6, 0, 2, 3 };
	smvk::MeshOptStats ws;
	Check(smvk::OptimizeStaticMeshUpload(dup, cfg, 0, &ws, nullptr), "meshopt.weld.ok");
	Check(ws.outVertices == 4, "meshopt.weld.merged");

	// Degenerate input fails softly.
	smvk::StaticMeshUpload empty;
	smvk::LodInfo elod;
	Check(!smvk::OptimizeStaticMeshUpload(empty, cfg, 0, nullptr, &elod), "meshopt.empty.false");
	Check(elod.lodIndex == 0, "meshopt.empty.lodmarker");

	// Disabled config leaves data untouched.
	auto q2 = MakeQuad();
	smvk::MeshOptConfig off;
	off.enabled = false;
	Check(!smvk::OptimizeStaticMeshUpload(q2, off, 0), "meshopt.disabled.false");
	Check(q2.vertices.size() == 4, "meshopt.disabled.untouched");
}

void TestSmoothNormalRepair() {
	// Thin double-sided card modeled as one triangle wound both ways
	// (shared vertices, opposing faces — the decimated-foliage case that
	// produced the distant-sibling black band): the area-weighted
	// accumulation cancels to exactly zero. Pre-repair this left the
	// (0,1,0) fallback, which disagrees with both faces (dot == 0) and
	// shades black where it interpolates backwards (NdotL <= 0 kills
	// the sun, the GI SH samples the dark lobe).
	smvk::StaticMeshUpload u;
	auto vert = [&](float x, float y, float z) {
		smvk::StaticVertex v{};
		v.pos = { x, y, z };
		v.color = { 1, 1, 1, 1 };
		v.texCoord = { x, y };
		v.normal = { 0, 0, 1 };
		v.tangent = { 1, 0, 0, 1 };
		u.vertices.push_back(v);
	};
	vert(0.0f, 0.0f, 0.0f); // 0
	vert(1.0f, 0.0f, 0.0f); // 1
	vert(0.0f, 1.0f, 0.0f); // 2
	vert(5.0f, 5.0f, 5.0f); // 3: unreferenced (keeps the +Y fallback)
	u.indices = { 0, 1, 2, 0, 2, 1 }; // +Z then -Z, same verts
	u.aabbMin = { 0, 0, 0 };
	u.aabbMax = { 1, 1, 0 };
	smvk::ComputeSmoothNormals(u);
	// Every referenced vertex must end unit-length and agreeing with its
	// dominant face (dot > 0). The dominant face is T0 (+Z: first max on
	// the area tie), so the repaired normals must point +Z here.
	for (int i = 0; i < 3; ++i) {
		const glm::vec3& n = u.vertices[i].normal;
		Check(std::isfinite(n.x + n.y + n.z)
			&& fabsf(glm::length(n) - 1.0f) < 1e-5f, "smoothnorm.repair.unit");
		Check(glm::dot(n, glm::vec3(0.0f, 0.0f, 1.0f)) > 0.0f,
			"smoothnorm.repair.agrees");
	}
	// Unreferenced vertices keep the legacy +Y fallback (no face to
	// agree with, so the repair correctly skips them).
	{
		const glm::vec3& n = u.vertices[3].normal;
		Check(fabsf(n.x) < 1e-6f && fabsf(n.y - 1.0f) < 1e-6f
			&& fabsf(n.z) < 1e-6f, "smoothnorm.repair.unreferenced");
	}
	// Clean single-sided quad (consistent CCW winding — note MakeQuad
	// itself mixes windings, so this control builds its own): pure
	// accumulation, repair must not fire (exact +Z preserved).
	smvk::StaticMeshUpload q;
	for (int i = 0; i < 4; ++i) {
		smvk::StaticVertex v{};
		v.pos = { (i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f, 0.0f };
		v.color = { 1, 1, 1, 1 };
		v.texCoord = { v.pos.x, v.pos.y };
		v.normal = { 0, 0, 1 };
		v.tangent = { 1, 0, 0, 1 };
		q.vertices.push_back(v);
	}
	q.indices = { 0, 1, 2, 1, 3, 2 }; // both CCW (+Z)
	q.aabbMin = { 0, 0, 0 };
	q.aabbMax = { 1, 1, 0 };
	smvk::ComputeSmoothNormals(q);
	for (const auto& v : q.vertices)
		Check(fabsf(v.normal.x) < 1e-6f && fabsf(v.normal.y) < 1e-6f
			&& fabsf(v.normal.z - 1.0f) < 1e-6f, "smoothnorm.repair.clean");
}

smvk::StaticMeshUpload MakeGrid(int n) {
	smvk::StaticMeshUpload u;
	for (int iy = 0; iy <= n; ++iy)
		for (int ix = 0; ix <= n; ++ix) {
			smvk::StaticVertex v{};
			v.pos = { (float)ix, (float)iy, 0.1f * sinf((float)ix * 0.7f) * cosf((float)iy * 0.7f) };
			v.color = { 1, 1, 1, 1 };
			v.texCoord = { (float)ix / n, (float)iy / n };
			v.normal = { 0, 0, 1 };
			v.tangent = { 1, 0, 0, 1 };
			u.vertices.push_back(v);
		}
	auto at = [n](int ix, int iy) { return (uint32_t)(iy * (n + 1) + ix); };
	for (int iy = 0; iy < n; ++iy)
		for (int ix = 0; ix < n; ++ix) {
			u.indices.insert(u.indices.end(), { at(ix, iy), at(ix + 1, iy), at(ix, iy + 1) });
			u.indices.insert(u.indices.end(), { at(ix + 1, iy), at(ix + 1, iy + 1), at(ix, iy + 1) });
		}
	u.aabbMin = { 0, 0, -1 };
	u.aabbMax = { (float)n, (float)n, 1 };
	return u;
}

// Flat grid with adversarial checker UVs: geometrically a perfect plane
// (positions-only simplify would nuke it for ~zero error) but every edge
// spans the full texture, so any collapse destroys the parameterization.
smvk::StaticMeshUpload MakeCheckerGrid(int n) {
	smvk::StaticMeshUpload u;
	for (int iy = 0; iy <= n; ++iy)
		for (int ix = 0; ix <= n; ++ix) {
			smvk::StaticVertex v{};
			v.pos = { (float)ix, (float)iy, 0.0f };
			v.color = { 1, 1, 1, 1 };
			const float c = ((ix + iy) & 1) ? 1.0f : 0.0f;
			v.texCoord = { c, c };
			v.normal = { 0, 0, 1 };
			v.tangent = { 1, 0, 0, 1 };
			u.vertices.push_back(v);
		}
	auto at = [n](int ix, int iy) { return (uint32_t)(iy * (n + 1) + ix); };
	for (int iy = 0; iy < n; ++iy)
		for (int ix = 0; ix < n; ++ix) {
			u.indices.insert(u.indices.end(), { at(ix, iy), at(ix + 1, iy), at(ix, iy + 1) });
			u.indices.insert(u.indices.end(), { at(ix + 1, iy), at(ix + 1, iy + 1), at(ix, iy + 1) });
		}
	u.aabbMin = { 0, 0, 0 };
	u.aabbMax = { (float)n, (float)n, 0 };
	return u;
}

void TestLodChain() {
	// LOD chain on a tessellated grid: ratios must yield a strictly
	// coarser chain with finite, non-negative local errors.
	smvk::MeshOptConfig cfg;
	cfg.generateLods = true;
	cfg.lodRatios = { 0.6f, 0.3f, 0.12f };
	cfg.lodTargetError = 0.05f;
	auto grid = MakeGrid(16);
	const size_t baseTris = grid.indices.size() / 3;
	std::vector<smvk::CookedLod> chain;
	Check(smvk::BuildLodChainForUpload(grid, cfg, chain), "lod.grid.valid");
	Check(!chain.empty(), "lod.grid.nonempty");
	bool strictlyCoarser = true;
	size_t prev = baseTris;
	for (const auto& l : chain) {
		if (l.triCount >= prev)
			strictlyCoarser = false;
		prev = l.triCount;
		if (!std::isfinite(l.relativeError) || !std::isfinite(l.localError) ||
			l.relativeError < 0.0f || l.localError < 0.0f)
			strictlyCoarser = false;
		if (l.upload.vertices.empty() || l.upload.indices.empty())
			strictlyCoarser = false;
	}
	Check(strictlyCoarser, "lod.grid.coarser");
	Check(chain.size() <= cfg.lodRatios.size(), "lod.grid.bounded");

	// Sibling normals/tangents must be re-derived from the decimated
	// topology: LOD0's baked frame (here flat (0,0,1) for a bumpy grid)
	// interpolates wrongly over the new triangles and shades black.
	// Independently re-accumulate area-weighted normals per sibling and
	// compare against what was cooked.
	bool framesRecooked = true;
	for (const auto& l : chain) {
		const auto& up = l.upload;
		std::vector<glm::vec3> acc(up.vertices.size(), glm::vec3(0.0f));
		for (size_t t = 0; t + 2 < up.indices.size() + 1; t += 3) {
			const uint32_t i0 = up.indices[t], i1 = up.indices[t + 1], i2 = up.indices[t + 2];
			const glm::vec3 fn = glm::cross(
				up.vertices[i1].pos - up.vertices[i0].pos,
				up.vertices[i2].pos - up.vertices[i0].pos);
			acc[i0] += fn; acc[i1] += fn; acc[i2] += fn;
		}
		for (size_t i = 0; i < up.vertices.size(); ++i) {
			const glm::vec3 n = up.vertices[i].normal;
			if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) ||
				fabsf(glm::length(n) - 1.0f) > 1e-4f) {
				framesRecooked = false; break;
			}
			const glm::vec3 expect = (glm::dot(acc[i], acc[i]) > 1e-24f)
				? glm::normalize(acc[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
			if (glm::dot(n, expect) < 0.999f) { // stale LOD0 (0,0,1) fails here
				framesRecooked = false; break;
			}
			const glm::vec4 tan = up.vertices[i].tangent;
			if (!std::isfinite(tan.x) || !std::isfinite(tan.y) || !std::isfinite(tan.z) ||
				fabsf(glm::length(glm::vec3(tan)) - 1.0f) > 1e-4f ||
				fabsf(fabsf(tan.w) - 1.0f) > 1e-4f ||
				fabsf(glm::dot(glm::vec3(tan), n)) > 1e-3f) {
				framesRecooked = false; break;
			}
		}
		if (!framesRecooked) break;
	}
	Check(framesRecooked, "lod.grid.frames.recooked");

	// Tiny mesh: 2-tri quad must be fully filtered (min triangles = 8).
	auto quad = MakeQuad();
	std::vector<smvk::CookedLod> tiny;
	Check(smvk::BuildLodChainForUpload(quad, cfg, tiny), "lod.tiny.valid");
	Check(tiny.empty(), "lod.tiny.filtered");

	// Cutout path (preserveCoverage): UV-aware simplify + locked borders.
	// Foliage quads sample coverage from the alpha texture via UVs, so a
	// positions-only decimation that collapses borders / stretches UVs
	// flashes background through the leaves on every LOD switch.
	{
		auto cut = MakeGrid(16);
		std::vector<smvk::CookedLod> cchain;
		Check(smvk::BuildLodChainForUpload(cut, cfg, cchain, true), "lod.cutout.valid");
		Check(!cchain.empty(), "lod.cutout.nonempty");
		// Locked borders: every LOD0 edge vertex must survive bit-exact in
		// each kept sibling (border verts are never moved or removed).
		const int n = 16;
		bool bordersKept = true;
		for (const auto& l : cchain) {
			for (const auto& v : cut.vertices) {
				const bool edge = v.pos.x == 0.0f || v.pos.x == (float)n ||
					v.pos.y == 0.0f || v.pos.y == (float)n;
				if (!edge)
					continue;
				bool found = false;
				for (const auto& kv : l.upload.vertices) {
					if (kv.pos == v.pos) { found = true; break; }
				}
				if (!found) { bordersKept = false; break; }
			}
			if (!bordersKept)
				break;
			if (!std::isfinite(l.relativeError) || !std::isfinite(l.localError) ||
				l.relativeError < 0.0f || l.localError < 0.0f) {
				bordersKept = false;
				break;
			}
		}
		Check(bordersKept, "lod.cutout.borders");
	}

	// Degenerate input fails softly.
	smvk::StaticMeshUpload empty;
	std::vector<smvk::CookedLod> deg;
	Check(!smvk::BuildLodChainForUpload(empty, cfg, deg), "lod.empty.false");

	// Albedo-UV awareness: the default sibling path carries texCoord in
	// the error metric (MeshOptConfig::lodUvAware, on by default). On a
	// flat checker-UV sheet the position error is ~zero either way, but
	// the UV term must inflate the reported error vs positions-only; the
	// flag-off path must reproduce the positions-only result.
	{
		smvk::MeshOptConfig uvCfg;
		uvCfg.generateLods = true;
		uvCfg.lodRatios = { 0.6f };
		uvCfg.lodTargetError = 0.05f;
		auto checker = MakeCheckerGrid(8);
		std::vector<smvk::CookedLod> uvChain;
		Check(smvk::BuildLodChainForUpload(checker, uvCfg, uvChain),
			"lod.uv.valid");
		smvk::MeshOptConfig posCfg = uvCfg;
		posCfg.lodUvAware = false;
		std::vector<smvk::CookedLod> posChain;
		Check(smvk::BuildLodChainForUpload(checker, posCfg, posChain),
			"lod.uv.posonly.valid");
		Check(!uvChain.empty() && !posChain.empty(), "lod.uv.nonempty");
		if (!uvChain.empty() && !posChain.empty()) {
			Check(uvChain[0].localError > posChain[0].localError * 1.1f,
				"lod.uv.inflates");
			Check(posChain[0].localError < 1e-3f, "lod.uv.posonly.zeroerr");
		}
	}

	// URI helpers round-trip (views + arenas, no std::string round-trip).
	const std::string base = "models/q.gltf#mesh.0/prim.0";
	const smvk::StringViewT sib = smvk::LodSiblingUri(base, 2);
	Check(sib == base + "/lod2", "lod.uri.sibling");
	Check(smvk::BaseMeshUriForLod(sib) == base, "lod.uri.base");
	Check(smvk::BaseMeshUriForLod(base) == base, "lod.uri.base.passthrough");

	// Node scale: identity 1, max axis wins, translation ignored.
	Check(smvk::MaxScaleOfMatrix(glm::mat4(1.0f)) == 1.0f, "lod.scale.identity");
	{
		glm::mat4 m(1.0f);
		m[0] = glm::vec4(2.0f, 0, 0, 0);
		m[1] = glm::vec4(0, 3.0f, 0, 0);
		m[2] = glm::vec4(0, 0, 4.0f, 0);
		m[3] = glm::vec4(100.0f, 0, 0, 1);
		Check(smvk::MaxScaleOfMatrix(m) == 4.0f, "lod.scale.maxaxis");
	}

	// World-error finalize: local error * max node scale over instances.
	{
		using namespace smvk;
		auto cluster = std::make_unique<Cluster>(nullptr);
		cluster->SetOfflineMode(true);
		auto q = MakeQuad();
		const auto h = cluster->UploadStaticMesh(q);
		const std::string uri = "models/q.gltf#mesh.9/prim.0";
		cluster->RegisterMeshUri(uri, h);
		LodInfo lod0;
		lod0.groupId = 9;
		lod0.lodIndex = 0;
		lod0.candidateCount = 2;
		lod0.triCount = 2;
		cluster->RegisterMeshLod(uri, lod0);
		// Fake LOD1 upload (same quad) with local error 0.5.
		const auto h1 = cluster->UploadStaticMesh(q);
		const smvk::StringViewT uri1 = LodSiblingUri(uri, 1);
		cluster->RegisterMeshUri(uri1, h1);
		LodInfo lod1;
		lod1.groupId = 9;
		lod1.lodIndex = 1;
		lod1.candidateCount = 2;
		lod1.simplifyError = 0.5f;
		lod1.triCount = 1;
		cluster->RegisterMeshLod(uri1, lod1);

		const int root = addNode(cluster->scene, -1, 0);
		const int n1 = addNode(cluster->scene, root, 1);
		const int n2 = addNode(cluster->scene, root, 1);
		cluster->scene.meshForNode[n1] = h.index();
		cluster->scene.meshForNode[n2] = h.index();
		cluster->scene.localTransform[n1] = glm::mat4(1.0f);
		cluster->scene.localTransform[n2] = glm::scale(glm::mat4(1.0f), glm::vec3(3.0f));
		markAsChanged(cluster->scene, root);
		recalculateGlobalTransforms(cluster->scene);
		FinalizeLodWorldErrors(*cluster);
		auto got = cluster->GetMeshLod(uri1);
		Check(got && fabsf(got->simplifyError - 1.5f) < 1e-4f, "lod.finalize.maxscale");
		auto got0 = cluster->GetMeshLod(uri);
		Check(got0 && got0->simplifyError == 0.0f, "lod.finalize.lod0.zero");
	}

	// Finalize idempotence (Sponza regression): the frostbite pipeline
	// finalizes twice (pre-bake sibling cook + post-bake sibling pass).
	// A second run must keep single-scaled errors — compounding the node
	// scale shrinks Sponza's 0.008-root errors 125x per extra run and the
	// renderer drops to the coarsest LOD centimeters from the camera.
	{
		using namespace smvk;
		auto cluster = std::make_unique<Cluster>(nullptr);
		cluster->SetOfflineMode(true);
		auto q = MakeQuad();
		const auto h = cluster->UploadStaticMesh(q);
		const std::string uri = "models/sponza/Sponza.gltf#mesh.0/prim.0";
		cluster->RegisterMeshUri(uri, h);
		LodInfo lod0;
		lod0.groupId = 0;
		lod0.lodIndex = 0;
		lod0.candidateCount = 2;
		lod0.triCount = 2;
		cluster->RegisterMeshLod(uri, lod0);
		const auto h1 = cluster->UploadStaticMesh(q);
		const smvk::StringViewT uri1 = LodSiblingUri(uri, 1);
		cluster->RegisterMeshUri(uri1, h1);
		LodInfo lod1;
		lod1.groupId = 0;
		lod1.lodIndex = 1;
		lod1.candidateCount = 2;
		lod1.simplifyError = 2.5f; // local units; world = 2.5 * 0.008
		lod1.triCount = 1;
		cluster->RegisterMeshLod(uri1, lod1);

		const int root = addNode(cluster->scene, -1, 0);
		const int n1 = addNode(cluster->scene, root, 1);
		cluster->scene.meshForNode[n1] = h.index();
		cluster->scene.localTransform[n1] =
			glm::scale(glm::mat4(1.0f), glm::vec3(0.008f));
		markAsChanged(cluster->scene, root);
		recalculateGlobalTransforms(cluster->scene);
		FinalizeLodWorldErrors(*cluster);
		auto once = cluster->GetMeshLod(uri1);
		Check(once && fabsf(once->simplifyError - 0.02f) < 1e-6f,
			"lod.finalize.once");
		// Second run (post-bake pass): must not compound the scale.
		FinalizeLodWorldErrors(*cluster);
		auto twice = cluster->GetMeshLod(uri1);
		Check(twice && fabsf(twice->simplifyError - 0.02f) < 1e-6f,
			"lod.finalize.idempotent");
		// Changed transforms still re-scale from the local error.
		cluster->scene.localTransform[n1] =
			glm::scale(glm::mat4(1.0f), glm::vec3(0.016f));
		markAsChanged(cluster->scene, root);
		recalculateGlobalTransforms(cluster->scene);
		FinalizeLodWorldErrors(*cluster);
		auto rescaled = cluster->GetMeshLod(uri1);
		Check(rescaled && fabsf(rescaled->simplifyError - 0.04f) < 1e-6f,
			"lod.finalize.rescale");
	}
}

void TestLodSelection() {
	// Error-driven transitions share one formula (cook, tests, renderer).
	using namespace smvk;
	const float fov = 0.7853982f; // 45 deg
	const float h = 1080.0f;
	// LOD0 / degenerate inputs never project.
	Check(LodProjectedPixelError(0.0f, 10.0f, h, fov) == 0.0f, "lod.px.zero");
	Check(LodProjectedPixelError(0.02f, 0.0f, h, fov) == 0.0f, "lod.px.nodist");
	// Known value: 0.02m at 1m, 1080p/45deg ~= 26.07px.
	{
		const float px = LodProjectedPixelError(0.02f, 1.0f, h, fov);
		Check(fabsf(px - 26.07f) < 0.05f, "lod.px.value");
	}
	// Switch distance is the inverse: error projects to exactly the budget.
	{
		const float d = LodSwitchDistance(0.02f, h, fov, 1.0f);
		Check(fabsf(d - 26.07f) < 0.05f, "lod.switch.value");
		Check(fabsf(LodProjectedPixelError(0.02f, d, h, fov) - 1.0f) < 1e-3f,
			"lod.switch.roundtrip");
	}
	// Coarsest-in-budget wins; errors grow coarser so the first miss stops.
	{
		const float errors[] = { 0.0f, 0.01f, 0.05f };
		// d=5: LOD1 projects ~2.6px > 1px budget -> LOD0.
		Check(SelectLodIndexForDistance(errors, 3, 5.0f, h, fov, 1.0f) == 0,
			"lod.sel.near");
		// d=50: LOD1 ~0.26px in budget, LOD2 ~1.3px out -> LOD1.
		Check(SelectLodIndexForDistance(errors, 3, 50.0f, h, fov, 1.0f) == 1,
			"lod.sel.mid");
		// d=500: everything in budget -> coarsest (LOD2).
		Check(SelectLodIndexForDistance(errors, 3, 500.0f, h, fov, 1.0f) == 2,
			"lod.sel.far");
		// Empty chain -> LOD0.
		Check(SelectLodIndexForDistance(nullptr, 0, 500.0f, h, fov, 1.0f) == 0,
			"lod.sel.empty");
	}
	// Hysteresis band (currentLod + hysteresis): coarsen needs the tight
	// budget*(1-h), refine needs the loose budget*(1+h); inside the band
	// the current level is sticky.
	{
		const float errors[] = { 0.0f, 0.01f, 0.05f };
		constexpr float hys = 0.2f;
		// Distances where LOD1 projects exactly 0.9 / 1.1 / 1.3 px
		// (LOD2 stays far out of budget at all of them).
		const float d09 = LodSwitchDistance(0.01f, h, fov, 0.9f);
		const float d11 = LodSwitchDistance(0.01f, h, fov, 1.1f);
		const float d13 = LodSwitchDistance(0.01f, h, fov, 1.3f);
		// Stateless walk picks LOD1 wherever it is in budget (0.9px);
		// the banded walk needs the 0.8px tight bar to coarsen off LOD0.
		Check(SelectLodIndexForDistance(errors, 3, d09, h, fov, 1.0f) == 1,
			"lod.hyst.stateless.09");
		Check(SelectLodIndexForDistance(errors, 3, d09, h, fov, 1.0f, 0, hys) == 0,
			"lod.hyst.fresh.tightbar");
		Check(SelectLodIndexForDistance(errors, 3, d11, h, fov, 1.0f) == 0,
			"lod.hyst.stateless.11");
		// Already at LOD1: sticky inside the band (0.8 < px <= 1.2), refine
		// only past the loose bar.
		Check(SelectLodIndexForDistance(errors, 3, d09, h, fov, 1.0f, 1, hys) == 1,
			"lod.hyst.sticky");
		Check(SelectLodIndexForDistance(errors, 3, d11, h, fov, 1.0f, 1, hys) == 1,
			"lod.hyst.sticky.loose");
		Check(SelectLodIndexForDistance(errors, 3, d13, h, fov, 1.0f, 1, hys) == 0,
			"lod.hyst.refine");
		// h = 0 is the stateless result for any current level.
		for (uint32_t cur = 0; cur < 3; ++cur) {
			Check(SelectLodIndexForDistance(errors, 3, 50.0f, h, fov, 1.0f, cur, 0.0f) == 1,
				"lod.hyst.zero.mid");
			Check(SelectLodIndexForDistance(errors, 3, 5.0f, h, fov, 1.0f, cur, 0.0f) == 0,
				"lod.hyst.zero.near");
			Check(SelectLodIndexForDistance(errors, 3, 500.0f, h, fov, 1.0f, cur, 0.0f) == 2,
				"lod.hyst.zero.far");
		}
		// Zero-error siblings pass both bars (always safe); a stale level
		// beyond a broken chain refines back to the last good LOD.
		{
			const float ez[] = { 0.0f, 0.0f, 0.05f };
			Check(SelectLodIndexForDistance(ez, 3, 5.0f, h, fov, 1.0f, 0, hys) == 1,
				"lod.hyst.zeroerr");
			const float eb[] = { 0.0f, 0.01f, -1.0f };
			Check(SelectLodIndexForDistance(eb, 3, 5000.0f, h, fov, 1.0f, 2, hys) == 1,
				"lod.hyst.broken.refine");
		}
	}
	// World chain resolution over cooked "/lodN" siblings (headless: cook
	// cluster registered with a World, no GPU involved).
	{
		auto cluster = std::make_unique<Cluster>(nullptr);
		cluster->SetOfflineMode(true);
		auto q = MakeQuad();
		const auto mh = cluster->UploadStaticMesh(q);
		const std::string uri = "models/q.gltf#mesh.4/prim.0";
		cluster->RegisterMeshUri(uri, mh);
		LodInfo lod0;
		lod0.groupId = 4;
		lod0.lodIndex = 0;
		lod0.candidateCount = 2;
		lod0.triCount = 2;
		cluster->RegisterMeshLod(uri, lod0);
		const auto h1 = cluster->UploadStaticMesh(q);
		const smvk::StringViewT uri1 = LodSiblingUri(uri, 1);
		cluster->RegisterMeshUri(uri1, h1);
		LodInfo lod1;
		lod1.groupId = 4;
		lod1.lodIndex = 1;
		lod1.candidateCount = 2;
		lod1.simplifyError = 0.5f; // local; finalize scales by node scale
		lod1.triCount = 1;
		cluster->RegisterMeshLod(uri1, lod1);
		const int root = addNode(cluster->scene, -1, 0);
		const int n1 = addNode(cluster->scene, root, 1);
		cluster->scene.meshForNode[n1] = mh.index();
		// Explicit opt-in: the ShaderLodPolicy default is vertex-lit OFF
		// (the derivation only stamps it when DeriveConfig::shaderLods
		// is set), so this test material programs it on by hand.
		MaterialCreateInfo shaderLodCi{};
		shaderLodCi.allowFarVertexLit = true;
		const auto shaderLodMatH = cluster->CreateMaterial(shaderLodCi);
		cluster->scene.materialForNode[n1] = shaderLodMatH.index();
		cluster->scene.localTransform[n1] = glm::scale(glm::mat4(1.0f), glm::vec3(3.0f));
		markAsChanged(cluster->scene, root);
		recalculateGlobalTransforms(cluster->scene);
		FinalizeLodWorldErrors(*cluster); // sibling world error = 1.5

		World world;
		world.RegisterCluster(std::move(cluster));
		world.ResolveAndFixup_clusters();
		Check(world.meshLocations.size() == 1, "lod.world.meshes");
		Check(world.LodChainLength(0) == 2, "lod.world.chainlen");
		Check(world.ResolveLodMesh(0, 0).has_value(), "lod.world.lod0");
		Check(world.ResolveLodMesh(0, 1).has_value(), "lod.world.lod1");
		Check(!world.ResolveLodMesh(0, 2).has_value(), "lod.world.lod2.miss");
		Check(fabsf(world.LodErrorAt(0, 1) - 1.5f) < 1e-4f, "lod.world.error");
		// err 1.5 projects ~19.5px at 100m (over budget -> LOD0),
		// ~0.39px at 5000m (in budget -> LOD1).
		Check(world.SelectLodIndex(0, 100.0f, h, fov, 1.0f) == 0, "lod.world.near");
		Check(world.SelectLodIndex(0, 5000.0f, h, fov, 1.0f) == 1, "lod.world.far");
		Check(world.GetLodMesh(0, 1) != nullptr, "lod.world.mesh");
		// Hysteresis through the World (sibling world error 1.5): 1.1 px is
		// inside the loose side of the band but over the plain bar — the
		// stateless pick is LOD0, the band keeps LOD1; 1.3 px refines.
		{
			const float dW11 = LodSwitchDistance(1.5f, h, fov, 1.1f);
			const float dW13 = LodSwitchDistance(1.5f, h, fov, 1.3f);
			Check(world.SelectLodIndex(0, dW11, h, fov, 1.0f) == 0,
				"lod.world.hyst.stateless");
			Check(world.SelectLodIndex(0, dW11, h, fov, 1.0f, 1) == 1,
				"lod.world.hyst.sticky");
			Check(world.SelectLodIndex(0, dW13, h, fov, 1.0f, 1) == 0,
				"lod.world.hyst.refine");
		}

		// ---- Shader LODs (per-material far-level shading) -----------
		// The farthest level of a cooked chain substitutes vertex-lit
		// when the material programs it (see ShaderLodPolicy), or stays
		// on lit with the material's quality specialization; every other
		// level, out-of-range probes and invalid materials resolve
		// default (full-lit, no spec). World node ids are re-based on
		// merge (the world reserves its own root), so the material is
		// located by (cluster, handle) in materialLocations, not by the
		// cluster-scene node id.
		uint32_t shaderLodMat = UINT32_MAX;
		for (uint32_t i = 0; i < (uint32_t)world.materialLocations.size(); ++i) {
			const auto& loc = world.materialLocations[i];
			if (loc.cluster == world.clusters.front().get()
				&& loc.handle == shaderLodMatH)
				shaderLodMat = i;
		}
		Check(shaderLodMat != UINT32_MAX, "lod.shader.matwired");
		{
			const auto fs0 = world.FarShadingAt(0, 0, shaderLodMat);
			Check(fs0.isDefault() && !fs0.vertexLit, "lod.shader.l0");
			const auto fs1 = world.FarShadingAt(0, 1, shaderLodMat);
			Check(!fs1.isDefault() && fs1.vertexLit, "lod.shader.farthest");
			const auto fs2 = world.FarShadingAt(0, 2, shaderLodMat);
			Check(fs2.isDefault(), "lod.shader.oob");
			const auto fsBad = world.FarShadingAt(0, 1, UINT32_MAX);
			Check(fsBad.isDefault(), "lod.shader.badmat");
		}
		Check(world.LodChainLength(0) == 2, "lod.shader.chainlen");
		// Opt-out: a material that disallows the cheap program stays on
		// lit at the farthest level (default quality = default shading),
		// and a non-default quality resolves specialized (and back).
		if (smvk::Material* m = world.GetMaterial(shaderLodMat)) {
			m->shaderLod.allowFarVertexLit = false;
			const auto fsOpt = world.FarShadingAt(0, 1, shaderLodMat);
			Check(fsOpt.isDefault() && !fsOpt.vertexLit, "lod.shader.optout");
			m->shaderLod.farQuality.specular = false;
			const auto fsSpec = world.FarShadingAt(0, 1, shaderLodMat);
			Check(!fsSpec.isDefault() && !fsSpec.vertexLit
				&& !fsSpec.quality.specular && fsSpec.quality.normalMap,
				"lod.shader.specquality");
			m->shaderLod.farQuality.specular = true;
			m->shaderLod.allowFarVertexLit = true;
			const auto fsBack = world.FarShadingAt(0, 1, shaderLodMat);
			Check(!fsBack.isDefault() && fsBack.vertexLit, "lod.shader.optin");
		}
		else {
			Check(false, "lod.shader.matfetch");
		}

		// ---- Decision cache cadence ------------------------------------
		// Node n1 sits at the origin scaled 3x; drive the eye instead of
		// the node: far eye (5000m) -> LOD1 in budget, near eye (100m)
		// -> LOD0 (err 1.5 projects ~19.5px).
		NodeId rn = 0;
		for (auto& [nd, midx] : world.scene.meshForNode)
			rn = nd; // the single renderable node
		LodFrameInputs in{};
		in.viewportHeight = h;
		in.fovY = fov;
		in.pixelBudget = 1.0f;
		// Full rate (DEFAULT): every node recomputes every frame.
		in.frameId = 1;
		in.eye = glm::vec3(0.0f, 5000.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 1, "lod.fullrate.bootstrap");
		in.frameId = 2;
		in.eye = glm::vec3(0.0f, 100.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 0, "lod.fullrate.refresh");
		// Frame 3 recomputes too (the halfrate mode below serves the
		// stale LOD0 here): the far eye wins immediately.
		in.frameId = 3;
		in.eye = glm::vec3(0.0f, 5000.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 1, "lod.fullrate.everyframe");
		// Same frame, different eye: the stamped decision is served to
		// every consumer (filter/prepass/main consistency within a frame).
		in.eye = glm::vec3(0.0f, 100.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 1, "lod.fullrate.sameframe");
		// Invalidate forces a recompute on the very next call.
		world.InvalidateLodCache();
		in.frameId = 4;
		in.eye = glm::vec3(0.0f, 100.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 0, "lod.fullrate.invalidate");
		// Halfrate opt-out (SetLodFullRate(false), the pre-fullrate
		// behavior): parity split serves 1-frame-stale decisions on
		// off-parity frames. Same eye swings as above — far/near errors
		// (0.39px / 19.5px) sit far outside the hysteresis band, so the
		// band never changes these outcomes.
		world.SetLodFullRate(false);
		world.InvalidateLodCache();
		// Frame 1 (tick 1, odd): even node not due yet, bootstrap computes
		// anyway — far inputs select LOD1.
		in.frameId = 1;
		in.eye = glm::vec3(0.0f, 5000.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 1, "lod.halfrate.bootstrap");
		// Frame 2 (tick 2, even): even node IS due — near inputs recompute
		// to LOD0.
		in.frameId = 2;
		in.eye = glm::vec3(0.0f, 100.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 0, "lod.halfrate.refresh");
		// Frame 3 (tick 3, odd): even node NOT due — far inputs are
		// ignored this frame, the cached LOD0 decision is served (this is
		// the halfrate: 1-frame-stale at worst, half the work per frame).
		in.frameId = 3;
		in.eye = glm::vec3(0.0f, 5000.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 0, "lod.halfrate.stale");
		// Frame 4 (tick 4, even): due again — far inputs win once more.
		in.frameId = 4;
		Check(world.LodIndexForNode(rn, 0, in) == 1, "lod.halfrate.nextrefresh");
		// Same frame, different eye: the stamped decision is served to
		// every consumer (filter/prepass/main consistency within a frame).
		in.eye = glm::vec3(0.0f, 100.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 1, "lod.halfrate.sameframe");
		// Invalidate forces a recompute on the very next call.
		world.InvalidateLodCache();
		in.frameId = 5;
		in.eye = glm::vec3(0.0f, 100.0f, 0.0f);
		Check(world.LodIndexForNode(rn, 0, in) == 0, "lod.halfrate.invalidate");
		world.SetLodFullRate(true);

		// Closest-approach distance (Sponza regression): node n1 sits at
		// the origin scaled 3x over a 2x2 quad, so the LOD0 world sphere
		// is center (3,3,0) radius ~4.24 — not the node origin.
		{
			const float d = world.LodDistanceForNode(rn, 0,
				glm::vec3(0.0f, 100.0f, 0.0f));
			Check(d > 90.0f && d < 100.0f, "lod.dist.sphere");
			Check(world.LodDistanceForNode(rn, 0, glm::vec3(3.0f, 3.0f, 0.0f))
				== 0.0f, "lod.dist.inside");
			// Unknown mesh falls back to the origin distance (100m).
			Check(world.LodDistanceForNode(rn, 999,
				glm::vec3(0.0f, 100.0f, 0.0f)) == 100.0f, "lod.dist.fallback");
		}

		// Uncooked chains (LOD0 alone, no "/lodN" siblings) never downgrade
		// shading: level 0 is the only level and stays full-lit. A fresh
		// resolve must also re-stamp the cooked chain's farthest level.
		// (World mesh indices follow unordered_map iteration order after
		// the re-resolve, so look the two meshes up by URI.)
		{
			Cluster& c = *world.clusters.front();
			const auto h2 = c.UploadStaticMesh(q);
			const std::string uri2 = "models/q.gltf#mesh.5/prim.0";
			c.RegisterMeshUri(uri2, h2);
			const int n2 = addNode(c.scene, 0, 2);
			c.scene.meshForNode[n2] = h2.index();
			markAsChanged(c.scene, 0);
			recalculateGlobalTransforms(c.scene);
			world.ResolveAndFixup_clusters();
			Check(world.meshLocations.size() == 2, "lod.shader.meshes2");
			uint32_t cookedMesh = UINT32_MAX, uncookedMesh = UINT32_MAX;
			for (uint32_t i = 0; i < (uint32_t)world.meshLocations.size(); ++i) {
				const auto& loc = world.meshLocations[i];
				const std::string u(loc.cluster->GetMeshUri(loc.handle).Str());
				if (u == uri2)
					uncookedMesh = i;
				else if (u == uri)
					cookedMesh = i;
			}
			Check(uncookedMesh != UINT32_MAX && cookedMesh != UINT32_MAX,
				"lod.shader.uris");
			uint32_t shaderLodMat2 = UINT32_MAX;
			for (uint32_t i = 0; i < (uint32_t)world.materialLocations.size(); ++i) {
				const auto& loc = world.materialLocations[i];
				if (loc.cluster == world.clusters.front().get()
					&& loc.handle == shaderLodMatH)
					shaderLodMat2 = i;
			}
			Check(shaderLodMat2 != UINT32_MAX, "lod.shader.matwired2");
			Check(world.LodChainLength(uncookedMesh) == 1, "lod.shader.uncooked.len");
			Check(world.FarShadingAt(uncookedMesh, 0, shaderLodMat2).isDefault(),
				"lod.shader.uncooked.lit");
			Check(world.FarShadingAt(uncookedMesh, 3, shaderLodMat2).isDefault(),
				"lod.shader.uncooked.oob");
			{
				const auto fsRe = world.FarShadingAt(cookedMesh, 1, shaderLodMat2);
				Check(!fsRe.isDefault() && fsRe.vertexLit, "lod.shader.restamp");
			}
		}
	}
}

void TestLodLightmapAudit() {
	using namespace smvk;
	std::vector<uint32_t> chartOf;
	auto grid = MakeTwoChartGrid(chartOf);

	// Chart-aware chain: one level from the two-chart grid.
	MeshOptConfig cfg;
	cfg.generateLods = true;
	cfg.lodRatios = { 0.5f };
	cfg.lodTargetError = 0.1f;
	cfg.lodMinTriangles = 2;
	cfg.lodMinReduction = 0.1f;
	std::vector<CookedLod> chain;
	Check(BuildLodChainForUpload(grid, cfg, chain, false, chartOf.data()),
		"lmlod.chain.ok");
	Check(chain.size() == 1, "lmlod.chain.kept");
	// Identity chart transforms (chart-local == atlas) for the audit:
	// one entry per chart of the grid.
	const std::vector<glm::vec4> gridXforms = {
		{ 1.0f, 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f, 0.0f } };
	if (!chain.empty()) {
		// Affine-in-position chart uvs within one chart: the simplified
		// triangles reuse LOD0's parameterization exactly, so the
		// projection audit reports ~0 texel drift and no cross-chart
		// triangle can exist (chart attributes blocked the seam twins).
		Check(chain[0].triCount < 8, "lmlod.chain.reduced");
		LodLightmapAuditReport rep;
		Check(AuditLodLightmap(grid, chain[0].upload, glm::mat4(1.0f),
			gridXforms, 512, 512, 8.0f, 0.5f, 1024, rep), "lmlod.audit.ran");
		Check(rep.samples > 0, "lmlod.audit.samples");
		Check(rep.unresolved == 0, "lmlod.audit.resolved");
		Check(rep.crossChartTris == 0, "lmlod.audit.nocross");
		Check(rep.maxTexels < 1.0f, "lmlod.audit.affine");
	}

	// Wrong parameterization: same geometry, v-flipped chart uvs — the
	// audit must see large texel drift (this is the regression detector
	// for "siblings with garbage chart uvs").
	{
		auto bad = grid;
		for (auto& v : bad.vertices)
			v.texCoord2.y = 0.95f - v.texCoord2.y;
		LodLightmapAuditReport rep;
		Check(AuditLodLightmap(grid, bad, glm::mat4(1.0f),
			gridXforms, 512, 512, 8.0f, 0.5f, 1024, rep), "lmlod.audit.bad.ran");
		Check(rep.samples > 0 && rep.maxTexels > 100.0f,
			"lmlod.audit.detects.drift");
	}

	// Cross-chart triangle: one quad mixing both charts (the
	// catastrophic atlas-smear shape) — counted on every triangle, no
	// projection needed (chart-id mismatch flags it).
	{
		smvk::StaticMeshUpload mix;
		mix.vertices = { grid.vertices[0], grid.vertices[9],
			grid.vertices[11], grid.vertices[2] }; // c0,c1,c1,c0 corners
		mix.indices = { 0, 1, 2, 0, 2, 3 };
		mix.aabbMin = { 0.0f, 0.0f, 0.0f };
		mix.aabbMax = { 2.0f, 2.0f, 0.0f };
		mix.surfaces.push_back({ 0, 6, 0 });
		LodLightmapAuditReport rep;
		Check(AuditLodLightmap(grid, mix, glm::mat4(1.0f),
			gridXforms, 512, 512, 8.0f, 0.5f, 1024, rep), "lmlod.audit.mix.ran");
		Check(rep.crossChartTris == 2, "lmlod.audit.crosschart");
	}
}

void TestCookLodSiblingsLightmap() {
	using namespace smvk;
	auto cluster = std::make_unique<Cluster>(nullptr);
	cluster->SetOfflineMode(true);
	std::vector<uint32_t> chartOf;
	auto grid = MakeTwoChartGrid(chartOf);
	const auto mh = cluster->UploadStaticMesh(grid);
	const std::string uri = "models/lm.gltf#mesh.0/prim.0";
	cluster->RegisterMeshUri(uri, mh);
	{
		LodInfo lod0;
		lod0.groupId = 5;
		lod0.triCount = 8;
		cluster->RegisterMeshLod(uri, lod0);
	}
	// Fake a stamped bake: lobe count + chart transforms on the mesh.
	cluster->staticMeshes.get(mh)->lmChunk = 9;
	cluster->staticMeshes.get(mh)->lmChartXforms = {
		{ 1.0f, 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f, 0.0f } };
	// Node + opaque material (Blend/MASK would divert to the coverage
	// path, which is never lightmapped).
	MaterialCreateInfo ci{};
	const auto matH = cluster->CreateMaterial(ci);
	const int root = addNode(cluster->scene, -1, 0);
	const int n1 = addNode(cluster->scene, root, 1);
	cluster->scene.meshForNode[n1] = mh.index();
	cluster->scene.materialForNode[n1] = matH.index();
	markAsChanged(cluster->scene, root);
	recalculateGlobalTransforms(cluster->scene);

	std::unordered_map<uint32_t, std::vector<uint32_t>> charts;
	charts.emplace(mh.index(), chartOf);
	MeshOptConfig cfg;
	cfg.generateLods = true;
	cfg.lodRatios = { 0.5f };
	cfg.lodTargetError = 0.1f;
	cfg.lodMinTriangles = 2;
	cfg.lodMinReduction = 0.1f;
	MeshletConfig mcfg; // off
	CookLodSiblingsOptions so;
	so.vertexCharts = &charts;
	so.audit = true;
	so.atlasWidth = 512;
	so.atlasHeight = 512;
	so.auditThresholdTexels = 8.0f;
	Check(CookLodSiblings(*cluster, cfg, mcfg, so), "lmlod.cook.ok");

	const std::string uri1 = uri + "/lod1";
	auto sibInfo = cluster->GetMeshLod(uri1);
	Check(sibInfo && sibInfo->lodIndex == 1, "lmlod.sib.lodinfo");
	Check(sibInfo && sibInfo->candidateCount == 2, "lmlod.sib.candidates");
	auto sibH = cluster->FindMeshByUri(uri1);
	Check(sibH && sibH->valid(), "lmlod.sib.handle");
	if (sibH) {
		const StaticMesh* sib = cluster->staticMeshes.get(*sibH);
		Check(sib && sib->lmChunk == 9, "lmlod.sib.lmchunk");
		Check(sib && sib->lmChartXforms.size() == 2, "lmlod.sib.xforms");
		const StaticMeshUpload* up = cluster->GetRetainedUpload(*sibH);
		Check(up && !up->indices.empty(), "lmlod.sib.retained");
		bool spansOk = true;
		bool chartsOk = true;
		bool anyUv = false;
		if (up) {
			for (size_t t = 0; t + 2 < up->indices.size() / 3; ++t) {
				const StaticVertex& va = up->vertices[up->indices[t * 3 + 0]];
				const StaticVertex& vb = up->vertices[up->indices[t * 3 + 1]];
				const StaticVertex& vc = up->vertices[up->indices[t * 3 + 2]];
				// Core invariant: no sibling triangle mixes charts.
				if (va.chartId != vb.chartId || va.chartId != vc.chartId) {
					chartsOk = false;
					break;
				}
				const glm::vec2& a = va.texCoord2;
				const glm::vec2& b = vb.texCoord2;
				const glm::vec2& c = vc.texCoord2;
				const float span = std::max({
					std::abs(a.x - b.x) + std::abs(a.y - b.y),
					std::abs(a.x - c.x) + std::abs(a.y - c.y),
					std::abs(b.x - c.x) + std::abs(b.y - c.y) });
				if (span > kLodCrossChartUvSpan) {
					spansOk = false;
					break;
				}
			}
			for (const auto& v : up->vertices) {
				if (v.texCoord2.x != 0.0f || v.texCoord2.y != 0.0f) {
					anyUv = true;
					break;
				}
			}
		}
		Check(chartsOk, "lmlod.sib.nochartmix");
		Check(spansOk, "lmlod.sib.nocrosschart");
		Check(anyUv, "lmlod.sib.uv2carried");
	}
	auto baseInfo = cluster->GetMeshLod(uri);
	Check(baseInfo && baseInfo->candidateCount == 2, "lmlod.base.candidates");
	Check(baseInfo && baseInfo->simplifyError == 0.0f, "lmlod.base.zeroerr");
	Check(sibInfo && sibInfo->simplifyError > 0.0f, "lmlod.sib.error");

	// Unlightmapped mesh (no charts entry, no lmChunk): siblings still
	// cook (positions-only path) and carry no lightmap chunk.
	std::vector<uint32_t> chartOf2;
	auto grid2 = MakeTwoChartGrid(chartOf2);
	const auto mh2 = cluster->UploadStaticMesh(grid2);
	const std::string uri2 = "models/lm.gltf#mesh.1/prim.0";
	cluster->RegisterMeshUri(uri2, mh2);
	const int n2 = addNode(cluster->scene, root, 1);
	cluster->scene.meshForNode[n2] = mh2.index();
	cluster->scene.materialForNode[n2] = matH.index();
	markAsChanged(cluster->scene, root);
	recalculateGlobalTransforms(cluster->scene);
	Check(CookLodSiblings(*cluster, cfg, mcfg, so), "lmlod.cook2.ok");
	auto sib2H = cluster->FindMeshByUri(uri2 + "/lod1");
	Check(sib2H && sib2H->valid(), "lmlod.unlit.sib");
	if (sib2H) {
		const StaticMesh* sib2 = cluster->staticMeshes.get(*sib2H);
		Check(sib2 && sib2->lmChunk == 0xFFFFFFFFu, "lmlod.unlit.nolmchunk");
	}
}

void TestLightmapChartTable() {
	using namespace smvk;
	// Scene-wide chart transform table (headless: offline cluster
	// registered with a World, no GPU involved). Mesh A carries 2
	// charts, mesh B 1 chart, mesh C is unbaked (contributes nothing).
	auto cluster = std::make_unique<Cluster>(nullptr);
	cluster->SetOfflineMode(true);
	std::vector<uint32_t> chartOf;
	auto gridA = MakeTwoChartGrid(chartOf);
	const auto mhA = cluster->UploadStaticMesh(gridA);
	cluster->RegisterMeshUri("models/lm.gltf#mesh.0/prim.0", mhA);
	cluster->staticMeshes.get(mhA)->lmChunk = 9;
	const std::vector<glm::vec4> xformsA = {
		{ 0.5f, 0.5f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.5f, 0.0f } };
	cluster->staticMeshes.get(mhA)->lmChartXforms = xformsA;
	auto gridB = MakeQuad();
	const auto mhB = cluster->UploadStaticMesh(gridB);
	cluster->RegisterMeshUri("models/lm.gltf#mesh.1/prim.0", mhB);
	cluster->staticMeshes.get(mhB)->lmChunk = 5;
	const std::vector<glm::vec4> xformsB = {
		{ 0.25f, 0.25f, 0.25f, 0.5f } };
	cluster->staticMeshes.get(mhB)->lmChartXforms = xformsB;
	auto gridC = MakeQuad();
	const auto mhC = cluster->UploadStaticMesh(gridC);
	cluster->RegisterMeshUri("models/lm.gltf#mesh.2/prim.0", mhC);
	MaterialCreateInfo ci{};
	const auto matH = cluster->CreateMaterial(ci);
	const int root = addNode(cluster->scene, -1, 0);
	const int nA = addNode(cluster->scene, root, 1);
	const int nB = addNode(cluster->scene, root, 1);
	const int nC = addNode(cluster->scene, root, 1);
	cluster->scene.meshForNode[nA] = mhA.index();
	cluster->scene.meshForNode[nB] = mhB.index();
	cluster->scene.meshForNode[nC] = mhC.index();
	cluster->scene.materialForNode[nA] = matH.index();
	cluster->scene.materialForNode[nB] = matH.index();
	cluster->scene.materialForNode[nC] = matH.index();
	markAsChanged(cluster->scene, root);
	recalculateGlobalTransforms(cluster->scene);

	World world;
	world.RegisterCluster(std::move(cluster));
	world.ResolveAndFixup_clusters();
	Check(world.meshLocations.size() == 3, "lmchart.meshes");
	// World mesh indices follow unordered_map iteration order — look
	// them up by pool handle.
	auto worldIndexOf = [&](uint32_t poolIdx) -> uint32_t {
		for (uint32_t i = 0; i < (uint32_t)world.meshLocations.size(); ++i)
			if (world.meshLocations[i].handle.index() == poolIdx)
				return i;
		return 0xFFFFFFFFu;
	};
	const uint32_t wiA = worldIndexOf(mhA.index());
	const uint32_t wiB = worldIndexOf(mhB.index());
	const uint32_t wiC = worldIndexOf(mhC.index());
	Check(wiA != 0xFFFFFFFFu && wiB != 0xFFFFFFFFu
		&& wiC != 0xFFFFFFFFu, "lmchart.lookup");
	if (wiA == 0xFFFFFFFFu || wiB == 0xFFFFFFFFu || wiC == 0xFFFFFFFFu)
		return;
	// Table holds exactly the baked charts (2 + 1), each mesh's entries
	// in chart order at its own base.
	Check(world.lmChartTable.size() == 3, "lmchart.count");
	const uint32_t baseA = world.ChartOffsetFor(wiA, 0);
	const uint32_t baseB = world.ChartOffsetFor(wiB, 0);
	const uint32_t baseC = world.ChartOffsetFor(wiC, 0);
	Check(baseC == 0, "lmchart.unlit.zero");
	Check(baseA + 2 <= 3 && baseB + 1 <= 3, "lmchart.bounds");
	Check(baseA != baseB, "lmchart.distinct");
	bool tableOk = world.lmChartTable.size() == 3;
	for (size_t i = 0; tableOk && i < xformsA.size(); ++i)
		tableOk = tableOk && world.lmChartTable[baseA + i] == xformsA[i];
	for (size_t i = 0; tableOk && i < xformsB.size(); ++i)
		tableOk = tableOk && world.lmChartTable[baseB + i] == xformsB[i];
	Check(tableOk, "lmchart.layout");
	// CPU mirror of the VS unpack (atlasUv = chartUv * xy + zw) lands
	// inside the owning chart's rect for a stamped vertex.
	{
		const StaticVertex& v = gridA.vertices[6]; // chart 1 corner
		const glm::vec4& xf = world.lmChartTable[baseA + v.chartId];
		const glm::vec2 atlas = v.texCoord2 * glm::vec2(xf.x, xf.y)
			+ glm::vec2(xf.z, xf.w);
		Check(v.chartId == 1, "lmchart.vert.chart");
		Check(atlas.x >= 0.5f && atlas.x <= 1.0f
			&& atlas.y >= 0.0f && atlas.y <= 0.5f, "lmchart.vert.rect");
	}
	// Out-of-range probes resolve 0; a re-resolve bumps the version.
	Check(world.ChartOffsetFor(wiA, 7) == baseA, "lmchart.lod.oob");
	Check(world.ChartOffsetFor(9999, 0) == 0, "lmchart.mesh.oob");
	const uint64_t v0 = world.lmChartVersion;
	world.ResolveAndFixup_clusters();
	Check(world.lmChartVersion != v0, "lmchart.version");
	Check(world.ChartOffsetFor(wiA, 0) == baseA, "lmchart.stable");
}

void TestClusterSerializeCook() {
	using namespace smvk;
	// GPU-free offline cluster: CPU geometry + minted texture ids only.
	auto cluster = std::make_unique<Cluster>(nullptr);
	cluster->SetOfflineMode(true);
		Check(cluster->IsOfflineMode(), "offline.mode.on");

	// Mesh (cook path retains uploads for serialization).
	auto quad = MakeQuad();
	const ClusterStaticMeshHandle mh = cluster->UploadStaticMesh(quad);
	Check(mh.valid(), "cook.mesh.handle");
	Check(cluster->GetRetainedUpload(mh) != nullptr, "cook.mesh.retained");
	Check(cluster->GetRetainedUpload(mh)->vertices.size() == 4, "cook.mesh.retained.verts");
	const std::string meshUri = "models/q.gltf#mesh.0/prim.0";
	Check(cluster->RegisterMeshUri(meshUri, mh), "cook.mesh.uri");
	{
		LodInfo lod;
		lod.triCount = 2;
		cluster->RegisterMeshLod(meshUri, lod);
	}

	// Texture: minted handle (no RHI pool), validated payload stashed.
	const TextureHandle th = TextureHandle::Mint(cluster->AllocOfflineTextureId());
	const std::string texUri = "textures/fake.ktx2";
	Check(th.valid() && th.index() == 0, "cook.tex.mint");
	Check(cluster->RegisterTextureUri(texUri, th), "cook.tex.uri");
	cluster->RegisterTextureSampler(texUri, TextureSamplerDesc{ 9729, 9987, 10497, 10497 });
	// Minimal fake KTX2 payload (magic + pad) — serializer embeds verbatim.
	std::vector<uint8_t> fakeKtx = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
		0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4 };
	cluster->SetTextureSource(texUri, fakeKtx);
	Check(cluster->GetTextureSource(texUri) != nullptr, "cook.tex.src");

	// Material referencing the minted texture slot.
	MaterialCreateInfo ci{};
	ci.baseColorTexture = th;
	ci.metallicFactor = 0.25f;
	ci.roughnessFactor = 0.75f;
	ci.shadingModel = ShadingModel::Masked;
	ci.alphaMode = AlphaMode::Mask;
	ci.alphaCutoff = 0.33f;
	ci.doubleSided = true;
	const ClusterMaterialHandle math = cluster->CreateMaterial(ci);
	Check(math.valid(), "cook.mat.handle");
	Check(cluster->RegisterMaterialUri("models/q.gltf#mat.0", math), "cook.mat.uri");
	Check(cluster->FindMaterialByUri("models/q.gltf#mat.0") == math, "cook.mat.find");
	{
		const Material* m = cluster->materials.get(math);
		Check(m && m->values.textures[0] == 0 && m->doubleSided &&
			m->alphaMode == AlphaMode::Mask, "cook.mat.values");
	}

	// Scene: root + one renderable node.
	const int root = addNode(cluster->scene, -1, 0);
	const int child = addNode(cluster->scene, root, 1);
	cluster->scene.meshForNode[child] = mh.index();
	cluster->scene.materialForNode[child] = math.index();
	setNodeName(cluster->scene, child, "QuadNode");

	// Cooked punctual light carrying the static flag (bake source —
	// lightmap/probe harvest + runtime forward+ exclusion; see
	// SceneLight::kSceneLightFlag_Static). Links from its own node like
	// the glTF importer's group nodes.
	SceneLight lightDef;
	lightDef.type = SceneLight::Type::Spot;
	lightDef.color = glm::vec3(1.0f, 0.5f, 0.25f);
	lightDef.intensity = 64.0f;
	lightDef.range = 12.0f;
	lightDef.innerConeAngle = glm::radians(15.0f);
	lightDef.outerConeAngle = glm::radians(30.0f);
	lightDef.flags = SceneLight::kSceneLightFlag_Static;
	lightDef.name = "STATIC_LampA";
	const ClusterLightHandle lh = cluster->CreateLight(lightDef);
	Check(lh.valid(), "cook.light.handle");
	Check(cluster->RegisterLightUri("models/q.gltf#light.0", lh), "cook.light.uri");
	const int lightNode = addNode(cluster->scene, root, 1);
	cluster->scene.lightForNode[(uint32_t)lightNode] = lh.index();
	setNodeName(cluster->scene, lightNode, "LampNode");

	PakWriter writer;
	ClusterSerializeStats stats;
	std::string err;
	Check(SerializeCluster(*cluster, writer, &stats, &err), "cook.serialize.ok");
	if (!err.empty())
		std::cout << "  serialize err: " << err << "\n";
	Check(stats.meshes == 1 && stats.materials == 1 && stats.textures == 1 &&
		stats.nodes == 3, "cook.serialize.stats");
	Check(stats.lights == 1, "cook.serialize.stats.lights");

	std::vector<uint8_t> img = writer.Finalize();
	std::string openErr;
	auto reader = PakReader::Open(std::span<const uint8_t>(img), openErr);
	Check(reader.has_value(), "cook.pak.open");
	if (!reader)
		return;
	Check(reader->BlobsOfKind(PakAssetKind::Mesh).size() == 1, "cook.pak.mesh1");
	Check(reader->BlobsOfKind(PakAssetKind::Material).size() == 1, "cook.pak.mat1");
	Check(reader->BlobsOfKind(PakAssetKind::Texture).size() == 1, "cook.pak.tex1");
	Check(reader->BlobsOfKind(PakAssetKind::Scene).size() == 1, "cook.pak.scene1");

	// Light blob round-trip: definition fields + the trailing flags
	// section (kSceneLightFlag_Static) the bake harvest reads back.
	{
		const auto lightBlobs = reader->BlobsOfKind(PakAssetKind::Light);
		Check(lightBlobs.size() == 1, "cook.pak.light1");
		if (lightBlobs.size() == 1) {
			const std::span<const uint8_t> p = reader->Payload(*lightBlobs[0]);
			size_t off = 0;
			bool ok = p.size() >= 4;
			auto u32 = [&]() {
				uint32_t v = 0;
				if (off + 4 <= p.size())
					memcpy(&v, p.data() + off, 4);
				else
					ok = false;
				off += 4;
				return v;
			};
			auto f32 = [&]() {
				float v = 0.0f;
				if (off + 4 <= p.size())
					memcpy(&v, p.data() + off, 4);
				else
					ok = false;
				off += 4;
				return v;
			};
			auto str = [&]() {
				const uint32_t n = u32();
				std::string s;
				if (ok && off + n <= p.size()) {
					s.assign(reinterpret_cast<const char*>(p.data() + off), n);
					off += n;
				} else {
					ok = false;
				}
				return s;
			};
			const std::string uri = str();
			const uint32_t type = u32();
			const float cr = f32(), cg = f32(), cb = f32();
			const float intensity = f32();
			const float range = f32();
			const float inner = f32();
			const float outer = f32();
			const std::string name = str();
			const uint32_t flags = u32();
			Check(ok, "cook.pak.light.parse");
			Check(uri == "models/q.gltf#light.0", "cook.pak.light.uri");
			Check(type == 2 /* Spot */, "cook.pak.light.type");
			Check(std::abs(cr - 1.0f) < 1e-6f && std::abs(cg - 0.5f) < 1e-6f &&
				std::abs(cb - 0.25f) < 1e-6f, "cook.pak.light.color");
			Check(std::abs(intensity - 64.0f) < 1e-6f &&
				std::abs(range - 12.0f) < 1e-6f, "cook.pak.light.ir");
			Check(std::abs(inner - glm::radians(15.0f)) < 1e-6f &&
				std::abs(outer - glm::radians(30.0f)) < 1e-6f,
				"cook.pak.light.cones");
		Check(name == "STATIC_LampA", "cook.pak.light.name");
		Check(flags == SceneLight::kSceneLightFlag_Static,
			"cook.pak.light.flags.static");
		// v6 gobo/function section (the test light carries no cookie:
		// empty URI + the function defaults).
		const std::string cookie = str();
		const uint32_t fn = u32();
		const float fnSpeed = f32();
		const float fnScale = f32();
		Check(ok, "cook.pak.light.gobo.parse");
		Check(cookie.empty() && fn == 0
			&& std::abs(fnSpeed - 1.0f) < 1e-6f
			&& std::abs(fnScale - 0.5f) < 1e-6f,
			"cook.pak.light.gobo.defaults");
		Check(off == p.size(), "cook.pak.light.exactend");
		}
	}

	// Texture payload round-trip (bytes + sampler).
	const PakTocEntry* tex = reader->Find(PakAssetKind::Texture, texUri);
	Check(tex != nullptr, "cook.pak.tex.find");
	if (tex) {
		PakTexturePayload p;
		Check(ParseTexturePayload(reader->Payload(*tex), p), "cook.pak.tex.parse");
		Check(p.uri == texUri && p.magFilter == 9729 && p.minFilter == 9987 &&
			p.wrapS == 10497 && p.wrapT == 10497, "cook.pak.tex.sampler");
		Check(p.ktxBytes.size() == fakeKtx.size() &&
			memcmp(p.ktxBytes.data(), fakeKtx.data(), fakeKtx.size()) == 0,
			"cook.pak.tex.bytes");
	}

	// Mesh blob carries the v4 fast-load payload: packed-SoA streams (the
	// exact GPU heap layout) addressed by recorded blob-relative offsets.
	const auto meshes = reader->BlobsOfKind(PakAssetKind::Mesh);
	Check(!meshes.empty(), "cook.pak.mesh.find");
	if (!meshes.empty()) {
		const std::span<const uint8_t> pay = reader->Payload(*meshes[0]);
		// Header: uri len+bytes, counts(3), surfaces(0 entries), aabb(24),
		// sphere, topology, lod(7 fields), lmChunk, lmXformCount(0 here).
		const size_t header = 4 + meshUri.size() + 12 + 24 + 4 + 4 + 28 + 8;
		Check(pay.size() > header + 28, "cook.pak.mesh.v4.section");
		uint32_t marker = 0;
		memcpy(&marker, pay.data() + header, 4);
		Check(marker == 1u, "cook.pak.mesh.v4.marker");
		uint64_t posOff = 0, shadeOff = 0, idxOff = 0;
		memcpy(&posOff, pay.data() + header + 4, 8);
		memcpy(&shadeOff, pay.data() + header + 12, 8);
		memcpy(&idxOff, pay.data() + header + 20, 8);
		const size_t posBytes = 4 * sizeof(VertexPos);
		const size_t shadeBytes = 4 * sizeof(VertexShading);
		const size_t idxBytes = 6 * sizeof(uint32_t);
		// Stream starts: marker section end aligned up to 16, then the
		// three streams back-to-back, then the meshlet trailer (count 0).
		Check(posOff == ((header + 28 + 15) & ~size_t(15)), "cook.pak.mesh.v4.posoff");
		Check(shadeOff == posOff + posBytes && idxOff == shadeOff + shadeBytes,
			"cook.pak.mesh.v4.streamoffs");
		Check(posOff + posBytes + shadeBytes + idxBytes + 12 == pay.size(),
			"cook.pak.mesh.v4.exactend");
		// Stream content: the shared pack helper's output for the quad's
		// vertices + the raw index buffer.
		bool streamsOk = true;
		for (uint32_t v = 0; v < 4; ++v) {
			VertexPos pos{};
			VertexShading shade{};
			PackStaticVertex(quad.vertices[v], pos, shade);
			streamsOk = streamsOk
				&& memcmp(pay.data() + posOff + v * sizeof(VertexPos),
					&pos, sizeof(VertexPos)) == 0
				&& memcmp(pay.data() + shadeOff + v * sizeof(VertexShading),
					&shade, sizeof(VertexShading)) == 0;
		}
		for (uint32_t i = 0; i < 6; ++i) {
			uint32_t idx = 0;
			memcpy(&idx, pay.data() + idxOff + i * 4, 4);
			streamsOk = streamsOk && idx == quad.indices[i];
		}
		Check(streamsOk, "cook.pak.mesh.v4.streams");
	}
}

void TestStaticSubtrees() {
	Scene scene;
	const int root = addNode(scene, -1, 0);
	const int a = addNode(scene, root, 1);
	const int b = addNode(scene, a, 2);
	const int c = addNode(scene, root, 1);
	Check(!IsStaticNode(scene, a), "static.init.dynamic");
	MarkSubtreeStatic(scene, a);
	Check(IsStaticSubtreeRoot(scene, a), "static.root");
	Check(IsStaticNode(scene, a) && IsStaticNode(scene, b), "static.cache.desc");
	Check(!IsStaticNode(scene, root) && !IsStaticNode(scene, c), "static.cache.sibling");
	// Dynamic dirty path skips frozen nodes.
	markAsChangedDynamic(scene, root);
	bool sawStatic = false;
	for (int l = 0; l < MAX_NODE_LEVEL; ++l)
		for (int n : scene.changedAtThisFrame[l])
			if (n == a || n == b)
				sawStatic = true;
	Check(!sawStatic, "static.dirty.skip");
	for (int l = 0; l < MAX_NODE_LEVEL; ++l)
		scene.changedAtThisFrame[l].clear();
	// Dynamic recalc leaves baked globals alone.
	scene.globalTransform[b] = glm::mat4(2.0f);
	markAsChanged(scene, root);
	Check(recalculateGlobalTransformsDynamic(scene), "static.recalc.dynamic");
	Check(scene.globalTransform[b] == glm::mat4(2.0f), "static.recalc.baked");
	ClearSubtreeStatic(scene, a);
	Check(!IsStaticNode(scene, a) && !IsStaticNode(scene, b), "static.clear");
}

void TestMeshlets() {
	smvk::MeshletConfig cfg;
	cfg.enabled = true;
	cfg.maxVertices = 64;
	cfg.maxTriangles = 124;
	auto quad = MakeQuad();
	smvk::CookedMeshlets cooked;
	smvk::MeshletStats st;
	Check(smvk::BuildMeshletsForUpload(quad, cfg, &cooked, &st, nullptr), "meshlet.quad.ok");
	Check(!cooked.empty() && st.meshletCount >= 1, "meshlet.quad.count");
	Check(st.totalTriangles == 2, "meshlet.quad.tris");
	bool windowsOk = true;
	for (const auto& m : cooked.meshlets) {
		if (m.vertexCount == 0 || m.vertexCount > 64 || m.triangleCount == 0 ||
			m.triangleCount > 124 || m.boundRadius <= 0.0f)
			windowsOk = false;
		if ((size_t)m.vertexOffset + m.vertexCount > cooked.vertices.size() ||
			(size_t)m.triangleOffset + (size_t)m.triangleCount * 3 > cooked.triangles.size())
			windowsOk = false;
	}
	Check(windowsOk, "meshlet.quad.windows");
	// Indirect staging expands to the same triangle soup.
	std::vector<uint32_t> idx;
	std::vector<smvk::MeshletDrawCommand> cmds;
	smvk::BuildMeshletIndirectCommands(cooked, idx, cmds);
	Check(cmds.size() == cooked.meshlets.size() && idx.size() == 6, "meshlet.indirect.count");
	// Disabled config fails softly with empty output.
	smvk::MeshletConfig off;
	smvk::CookedMeshlets empty;
	Check(!smvk::BuildMeshletsForUpload(quad, off, &empty, nullptr, nullptr), "meshlet.disabled.false");
	Check(empty.empty(), "meshlet.disabled.empty");
	// Degenerate input fails softly.
	smvk::StaticMeshUpload deg;
	Check(!smvk::BuildMeshletsForUpload(deg, cfg, &empty, nullptr, nullptr), "meshlet.empty.false");

	// Containment invariant: every cooked bound sphere must contain its
	// meshlet's triangles (the HZB occlusion + cone tests rely on it; a
	// violation false-culls visible meshlets). Grid + one long sliver
	// forces disparate verts into shared meshlets (cf. sponza prim.49).
	{
		smvk::StaticMeshUpload u;
		const int N = 8;
		for (int iy = 0; iy <= N; ++iy)
			for (int ix = 0; ix <= N; ++ix) {
				smvk::StaticVertex v{};
				v.pos = { (float)ix, (float)iy, 0.0f };
				u.vertices.push_back(v);
			}
		auto at = [N](int ix, int iy) { return (uint32_t)(iy * (N + 1) + ix); };
		for (int iy = 0; iy < N; ++iy)
			for (int ix = 0; ix < N; ++ix) {
				u.indices.insert(u.indices.end(), { at(ix, iy), at(ix + 1, iy), at(ix, iy + 1) });
				u.indices.insert(u.indices.end(), { at(ix + 1, iy), at(ix + 1, iy + 1), at(ix, iy + 1) });
			}
		// Long sliver: two verts in the grid, one 60 units away.
		uint32_t s0 = (uint32_t)u.vertices.size();
		smvk::StaticVertex a{}; a.pos = { 0.0f, 0.0f, 0.0f };
		smvk::StaticVertex b{}; b.pos = { 0.5f, 0.0f, 0.0f };
		smvk::StaticVertex c{}; c.pos = { 60.0f, 0.5f, 0.0f };
		u.vertices.push_back(a); u.vertices.push_back(b); u.vertices.push_back(c);
		u.indices.insert(u.indices.end(), { s0, s0 + 1, s0 + 2 });
		smvk::MeshletConfig sc = cfg;
		sc.maxVertices = 64;
		sc.maxTriangles = 32;
		smvk::CookedMeshlets sliv;
		Check(smvk::BuildMeshletsForUpload(u, sc, &sliv, nullptr, nullptr), "meshlet.sliver.ok");
		bool contained = !sliv.empty();
		for (const auto& m : sliv.meshlets) {
			for (uint32_t t = 0; t < m.triangleCount && contained; ++t) {
				for (uint32_t k = 0; k < 3 && contained; ++k) {
					uint32_t local = sliv.triangles[m.triangleOffset + t * 3 + k];
					if (local >= m.vertexCount) { contained = false; break; }
					uint32_t vid = sliv.vertices[m.vertexOffset + local];
					if (vid >= u.vertices.size()) { contained = false; break; }
					const auto& p = u.vertices[vid].pos;
					float dx = p.x - m.boundCenter.x;
					float dy = p.y - m.boundCenter.y;
					float dz = p.z - m.boundCenter.z;
					float d = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (d > m.boundRadius + 1e-3f * std::max(1.0f, m.boundRadius))
						contained = false;
				}
			}
		}
		Check(contained, "meshlet.sliver.contained");
	}
}

void TestVertexPack() {
	using namespace smvk;
	using namespace smvk::vertexpack;

	// Packed shading stream: 24 bytes, fixed member offsets (the Vulkan
	// attribute table in mesh.h mirrors these: location 2 fetches uv as
	// RG16_Float, location 5 fetches chartUv as RG16_UNorm, location 6
	// fetches chart as R32_UInt).
	Check(sizeof(VertexShading) == 24, "vpack.sizeof");
	Check(offsetof(VertexShading, color) == 0 &&
		offsetof(VertexShading, uv) == 4 &&
		offsetof(VertexShading, chartUv) == 8 &&
		offsetof(VertexShading, normal) == 12 &&
		offsetof(VertexShading, tangent) == 16 &&
		offsetof(VertexShading, chart) == 20, "vpack.offsets");

	// Normal oct round-trip: axes, diagonals, skewed, degenerate zero.
	const glm::vec3 normalCases[] = {
		{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
		{ 1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, -1.0f },
		{ 0.3f, -0.7f, 0.648f },
		{ 0.0f, 0.0f, 0.0f }, // degenerate -> +Z fallback
	};
	bool normalsOk = true;
	for (const auto& n : normalCases) {
		const glm::vec3 out = UnpackNormalOct(PackNormalOct(n));
		const glm::vec3 ref =
			(glm::dot(n, n) > 1e-24f) ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, 1.0f);
		if (glm::dot(out, ref) < 0.9999f)
			normalsOk = false;
	}
	Check(normalsOk, "vpack.normal.roundtrip");

	// Tangent oct round-trip incl. handedness bit; w==0 packs as +1
	// (matches the fragment-shader placeholder fallback).
	bool tangentsOk = true;
	const glm::vec4 tangentCases[] = {
		{ 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, -1.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f }, { 0.577f, 0.577f, 0.577f, -1.0f },
		{ 0.0f, 0.0f, 0.0f, 1.0f }, // degenerate -> +X fallback
	};
	for (const auto& t : tangentCases) {
		const glm::vec4 out = UnpackTangentOct(PackTangentOct(glm::vec3(t), t.w));
		const glm::vec3 ref = glm::normalize(glm::vec3(t));
		const float refW = (t.w < 0.0f) ? -1.0f : 1.0f;
		if (glm::dot(glm::vec3(out), ref) < 0.999f)
			tangentsOk = false;
		if (out.w != refW)
			tangentsOk = false;
	}
	Check(tangentsOk, "vpack.tangent.roundtrip");

	// uv half round-trip over typical glTF ranges.
	bool uvOk = true;
	const glm::vec2 uvCases[] = {
		{ 0.0f, 0.0f }, { 0.25f, 0.75f }, { 3.5f, -1.25f }, { 17.0f, 0.5f },
	};
	for (const auto& uv : uvCases) {
		const uint32_t packed = PackUvHalf(uv);
		glm::vec2 unpacked = glm::unpackHalf2x16(packed);
		const glm::vec2 err = glm::abs(unpacked - uv);
		if (!(err.x < 0.01f && err.y < 0.01f))
			uvOk = false;
	}
	Check(uvOk, "vpack.uv.roundtrip");
}

void TestStaticCollapse() {
	using namespace smvk;
	auto cluster = std::make_unique<Cluster>(nullptr);
	cluster->SetOfflineMode(true);

	// Two quads sharing one material; second translated +1 on X.
	auto q1 = MakeQuad();
	auto q2 = MakeQuad();
	MaterialCreateInfo ci{};
	const ClusterMaterialHandle math = cluster->CreateMaterial(ci);
	const uint32_t matIdx = math.index();

	const int root = addNode(cluster->scene, -1, 0);
	const int n1 = addNode(cluster->scene, root, 1);
	const int n2 = addNode(cluster->scene, root, 1);
	const ClusterStaticMeshHandle h1 = cluster->UploadStaticMesh(q1);
	const ClusterStaticMeshHandle h2 = cluster->UploadStaticMesh(q2);
	cluster->scene.meshForNode[n1] = h1.index();
	cluster->scene.meshForNode[n2] = h2.index();
	cluster->scene.materialForNode[n1] = matIdx;
	cluster->scene.materialForNode[n2] = matIdx;
	cluster->scene.localTransform[n2] = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	MarkSubtreeStatic(cluster->scene, n1);
	MarkSubtreeStatic(cluster->scene, n2);
	markAsChanged(cluster->scene, root);
	recalculateGlobalTransforms(cluster->scene);

	StaticMergeOptions opts;
	opts.deleteSources = true;
	StaticMergeResult mr = CollapseStaticSubtrees(*cluster, opts);
	Check(mr.ok, "collapse.ok");
	Check(mr.srcNodesConsumed == 2, "collapse.consumed");
	Check(mr.blocks.size() == 1, "collapse.singleblock");
	if (!mr.blocks.empty()) {
		const auto h = cluster->staticMeshes.getHandle(mr.blocks[0].meshPoolIndex);
		const StaticMeshUpload* up = cluster->GetRetainedUpload(h);
		Check(up && up->vertices.size() == 8 && up->indices.size() == 12, "collapse.baked.size");
		if (up && up->vertices.size() == 8) {
			// Second quad baked at +1 X: its min.x must be 1, first quad 0.
			float minAll = up->vertices[0].pos.x;
			float maxAll = up->vertices[0].pos.x;
			for (const auto& v : up->vertices) {
				minAll = std::min(minAll, v.pos.x);
				maxAll = std::max(maxAll, v.pos.x);
			}
			Check(minAll == 0.0f && maxAll == 2.0f, "collapse.baked.transform");
		}
		Check(IsStaticNode(cluster->scene, mr.blocks[0].node), "collapse.block.static");
	}
}

void TestMultiSceneSerialize() {
	using namespace smvk;
	// Two offline clusters sharing one mesh URI: the shared blob must land
	// once, both scenes reference it, discovery lists both in order.
	auto makeScene = [](const char* meshUri, const char* matUri,
			const char* nodeName) {
		auto cluster = std::make_unique<Cluster>(nullptr);
		cluster->SetOfflineMode(true);
		auto quad = MakeQuad();
		const ClusterStaticMeshHandle mh = cluster->UploadStaticMesh(quad);
		cluster->RegisterMeshUri(meshUri, mh);
		MaterialCreateInfo ci{};
		const ClusterMaterialHandle math = cluster->CreateMaterial(ci);
		cluster->RegisterMaterialUri(matUri, math);
		const int root = addNode(cluster->scene, -1, 0);
		const int n = addNode(cluster->scene, root, 1);
		cluster->scene.meshForNode[n] = mh.index();
		cluster->scene.materialForNode[n] = math.index();
		setNodeName(cluster->scene, n, nodeName);
		return cluster;
	};
	auto a = makeScene("m#shared", "m#matA", "NodeA");
	auto b = makeScene("m#shared", "m#matB", "NodeB");
	PakWriter writer;
	ClusterSerializeStats stats;
	std::string err;
	const std::vector<const Cluster*> clusters{ a.get(), b.get() };
	Check(SerializeClusters(clusters, { "Alpha", "Beta" },
		writer, &stats, &err), "multiscene.serialize.ok");
	Check(stats.meshes == 1 && stats.materials == 2,
		"multiscene.stats.shared");
	Check(stats.nodes == 4, "multiscene.stats.nodes");
	std::vector<uint8_t> img = writer.Finalize();
	std::string openErr;
	auto reader = PakReader::Open(std::span<const uint8_t>(img), openErr);
	Check(reader.has_value(), "multiscene.pak.open");
	if (!reader)
		return;
	Check(reader->BlobsOfKind(PakAssetKind::Mesh).size() == 1,
		"multiscene.mesh.dedup");
	Check(reader->BlobsOfKind(PakAssetKind::Scene).size() == 2,
		"multiscene.scene2");
	std::string lerr;
	auto scenes = ListPakScenes(*reader, &lerr);
	Check(scenes.size() == 2, "multiscene.list.count");
	if (scenes.size() == 2) {
		Check(scenes[0].name == "Alpha" && scenes[1].name == "Beta",
			"multiscene.list.names");
		Check(scenes[0].nodes == 2 && scenes[1].nodes == 2,
			"multiscene.list.nodes");
		Check(scenes[0].tocPath == "<cluster-local>#scene",
			"multiscene.list.toc0");
		Check(scenes[1].tocPath == "<cluster-local>#scene.1",
			"multiscene.list.toc1");
	}
	// Named GI sidecars round-trip through one Gi blob (union across
	// scenes; parse needs no cluster). The dither flag rides along so
	// the v6 section walk is covered by the listing below.
	a->RegisterGiBinding(GiBinding::Kind::Lightmap, "town");
	b->RegisterGiBinding(GiBinding::Kind::Probe, "town_probe");
	// Bake-asset GUID names ride the same blob (see
	// Engine/Asset/bake_asset.h): the manifest kinds resolve the guided
	// runtime manifests, the payload names stay opaque strings here.
	a->RegisterGiBinding(GiBinding::Kind::RegManifest,
		"0123456789abcdef0123456789abcdef");
	b->RegisterGiBinding(GiBinding::Kind::CvolManifest,
		"fedcba9876543210fedcba9876543210");
	a->RegisterGiBinding(GiBinding::Kind::Cubemap,
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	MarkDitherCloseup(a->scene, 1);
	PakWriter writerGi;
	Check(SerializeClusters(clusters, { "Alpha", "Beta" }, writerGi,
		nullptr, &err), "multiscene.gi.serialize.ok");
	std::vector<uint8_t> imgGi = writerGi.Finalize();
	auto readerGi = PakReader::Open(std::span<const uint8_t>(imgGi), openErr);
	Check(readerGi.has_value(), "multiscene.gi.open");
	if (readerGi) {
		const auto giBlobs = readerGi->BlobsOfKind(PakAssetKind::Gi);
		Check(giBlobs.size() == 1, "multiscene.gi.oneblob");
		if (!giBlobs.empty()) {
			std::vector<GiBinding> bindings;
			std::string gerr;
			Check(ParseGiRegistryBlob(readerGi->Payload(*giBlobs[0]),
				bindings, gerr), "multiscene.gi.parse");
			Check(bindings.size() == 5, "multiscene.gi.count");
			if (bindings.size() == 5) {
				// Union order: cluster a's bindings (registration order)
				// first, then cluster b's.
				Check(bindings[0].kind == GiBinding::Kind::Lightmap &&
					bindings[0].name == "town", "multiscene.gi.lm");
				Check(bindings[1].kind == GiBinding::Kind::RegManifest &&
					bindings[1].name
						== "0123456789abcdef0123456789abcdef",
					"multiscene.gi.regmanifest");
				Check(bindings[2].kind == GiBinding::Kind::Cubemap &&
					bindings[2].name
						== "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
					"multiscene.gi.cubemap");
				Check(bindings[3].kind == GiBinding::Kind::Probe &&
					bindings[3].name == "town_probe", "multiscene.gi.probe");
				Check(bindings[4].kind == GiBinding::Kind::CvolManifest &&
					bindings[4].name
						== "fedcba9876543210fedcba9876543210",
					"multiscene.gi.cvolmanifest");
			}
		}
	}
	// Legacy single-scene layout still lists exactly one scene.
	PakWriter w2;
	Check(SerializeCluster(*a, w2, nullptr, &err), "multiscene.legacy.ok");
	std::vector<uint8_t> img2 = w2.Finalize();
	auto r2 = PakReader::Open(std::span<const uint8_t>(img2), openErr);
	Check(r2.has_value(), "multiscene.legacy.open");
	if (r2) {
		auto s2 = ListPakScenes(*r2, &lerr);
		Check(s2.size() == 1 && s2[0].nodes == 2, "multiscene.legacy.list");
	}
}

} // namespace

int main() {
	// Unbuffered stdout: a crash mid-run must not lose the tail (PASS lines
	// name the last completed check; stderr markers below name the test).
	setvbuf(stdout, nullptr, _IONBF, 0);
	// Asserts go to stderr, not a modal dialog: a failing assert must kill
	// the run immediately in CI/redirected runs instead of hanging on a
	// window nobody can see (the CRT default pops Retry/Abort/Ignore).
#ifdef _MSC_VER
	{
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	}
#endif
	// Per-test stderr markers: stdout is block-buffered when redirected, so
	// a crash mid-run loses the tail — stderr is unbuffered and names the
	// crashing test immediately.
	auto run = [](const char* name, void (*fn)()) {
		std::cerr << "[test] " << name << std::endl;
		fn();
	};
	run("TestVfsNormalize", TestVfsNormalize);
	run("TestOverlayMem", TestOverlayMem);
	run("TestAssetUris", TestAssetUris);
	run("TestPakRoundTrip", TestPakRoundTrip);
	run("TestPakCrcCombine", TestPakCrcCombine);
	run("TestPakCrcParallel", TestPakCrcParallel);
	run("TestPakTextureDigest", TestPakTextureDigest);
	run("TestPakAppend", TestPakAppend);
	run("TestLz4Codec", TestLz4Codec);
	run("TestPakCompressedRoundTrip", TestPakCompressedRoundTrip);
	run("TestPakBackendServesTextures", TestPakBackendServesTextures);
	run("TestMeshOpt", TestMeshOpt);
	run("TestSmoothNormalRepair", TestSmoothNormalRepair);
	run("TestLodChain", TestLodChain);
	run("TestLodSelection", TestLodSelection);
	run("TestLodLightmapAudit", TestLodLightmapAudit);
	run("TestCookLodSiblingsLightmap", TestCookLodSiblingsLightmap);
	run("TestLightmapChartTable", TestLightmapChartTable);
	run("TestVertexPack", TestVertexPack);
	run("TestClusterSerializeCook", TestClusterSerializeCook);
	run("TestMultiSceneSerialize", TestMultiSceneSerialize);
	run("TestStaticSubtrees", TestStaticSubtrees);
	run("TestMeshlets", TestMeshlets);
	run("TestStaticCollapse", TestStaticCollapse);
	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}

