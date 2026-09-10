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
#include <iostream>
#include <string>
#include <vector>

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
		v.color = { 1, 1, 1 };
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

smvk::StaticMeshUpload MakeGrid(int n) {
	smvk::StaticMeshUpload u;
	for (int iy = 0; iy <= n; ++iy)
		for (int ix = 0; ix <= n; ++ix) {
			smvk::StaticVertex v{};
			v.pos = { (float)ix, (float)iy, 0.1f * sinf((float)ix * 0.7f) * cosf((float)iy * 0.7f) };
			v.color = { 1, 1, 1 };
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
		cluster->SetCookMode(true);
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
	// World chain resolution over cooked "/lodN" siblings (headless: cook
	// cluster registered with a World, no GPU involved).
	{
		auto cluster = std::make_unique<Cluster>(nullptr);
		cluster->SetCookMode(true);
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

		// ---- Halfrate LOD cache (staggered halves) ----------------------
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
	}
}

void TestClusterSerializeCook() {
	using namespace smvk;
	// GPU-free cook cluster: CPU geometry + minted texture ids only.
	auto cluster = std::make_unique<Cluster>(nullptr);
	cluster->SetCookMode(true);
	Check(cluster->IsCookMode(), "cook.mode.on");

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
	const TextureHandle th = TextureHandle::Mint(cluster->AllocCookTextureId());
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

	PakWriter writer;
	ClusterSerializeStats stats;
	std::string err;
	Check(SerializeCluster(*cluster, writer, &stats, &err), "cook.serialize.ok");
	if (!err.empty())
		std::cout << "  serialize err: " << err << "\n";
	Check(stats.meshes == 1 && stats.materials == 1 && stats.textures == 1 &&
		stats.nodes == 2, "cook.serialize.stats");

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

	// Mesh blob contains the retained vertex/index payload.
	const auto meshes = reader->BlobsOfKind(PakAssetKind::Mesh);
	Check(!meshes.empty() &&
		reader->Payload(*meshes[0]).size() ==
			4 * sizeof(StaticVertex) + 6 * sizeof(uint32_t) + (0u /*header*/) +
			// header: uri len+bytes, counts, surfaces(0), aabb, sphere, topo, lod
			4 + meshUri.size() + 4 + 4 + 4 + 0 + 24 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 +
			// trailing meshlet section (v2, empty here): count + verts + tris
			4 + 4 + 4,
		"cook.pak.mesh.payload");
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

	// Packed shading stream: 16 bytes, fixed member offsets (the Vulkan
	// attribute table in mesh.h mirrors these).
	Check(sizeof(VertexShading) == 16, "vpack.sizeof");
	Check(offsetof(VertexShading, color) == 0 &&
		offsetof(VertexShading, uv) == 4 &&
		offsetof(VertexShading, normal) == 8 &&
		offsetof(VertexShading, tangent) == 12, "vpack.offsets");

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
	cluster->SetCookMode(true);

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

} // namespace

int main() {
	TestVfsNormalize();
	TestOverlayMem();
	TestAssetUris();
	TestPakRoundTrip();
	TestLz4Codec();
	TestPakCompressedRoundTrip();
	TestPakBackendServesTextures();
	TestMeshOpt();
	TestLodChain();
	TestLodSelection();
	TestVertexPack();
	TestClusterSerializeCook();
	TestStaticSubtrees();
	TestMeshlets();
	TestStaticCollapse();
	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
