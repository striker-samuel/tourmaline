// test_cache: artifact-cache library tests (scratch root handling +
// SQLite registry round-trips). No GPU, no editor. Run via CTest.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <artifact_cache/artifact_cache.h>
#include <artifact_cache/assetdb.h>
#include <artifact_cache/store_adapter.h>

#include <Engine/Asset/pak.h>
#include <Engine/Vfs/vfs.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

std::string TempDir() {
	const auto dir = std::filesystem::temp_directory_path() /
		"vulkantoy_cache_test";
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir, ec);
	return dir.generic_string();
}

} // namespace

void TestScratchAndRegistry() {
	using namespace smvk::cache;

	// ---- scratch root ----------------------------------------------------
	Check(!DefaultScratchRoot().empty(), "default root non-empty");
	{
		const std::string root = EnsureScratchRoot(
			TempDir() + "/scratch");
		Check(!root.empty(), "ensure root");
		Check(std::filesystem::is_directory(root + "/artifacts"),
			"ensure artifacts subdir");
		Check(EnsureScratchRoot("").empty(), "empty root rejected");
#ifdef _WIN32
		// Invisible scratch: the root carries the hidden attribute.
		const DWORD attrs = GetFileAttributesA(root.c_str());
		Check(attrs != INVALID_FILE_ATTRIBUTES
			&& (attrs & FILE_ATTRIBUTE_HIDDEN) != 0,
			"root hidden attribute");
#endif
	}

	// ---- hash ------------------------------------------------------------
	{
		const char a[] = "hello";
		const std::string h1 = HashBytes(a, 5);
		const std::string h2 = HashBytes(a, 5);
		const std::string h3 = HashBytes("hellp", 5);
		const std::string h4 = HashBytes("hello", 6);
		Check(h1 == h2 && h1.size() == 16, "hash deterministic + hex16");
		Check(h1 != h3, "hash differs on content");
		Check(h1 != h4, "hash differs on length");
	}

	// ---- registry ---------------------------------------------------------
	const std::string dir = TempDir();
	const std::string dbPath = dir + "/cache.db";
	std::string err;
	{
		CacheDb db;
		// Error paths before open.
		ArtifactRecord dummy;
		Check(!db.Put(dummy, err), "put before open fails");
		Check(!db.IsOpen(), "closed before open");
		Check(db.Open(dbPath, err), "open + schema init");
		Check(db.IsOpen(), "open state");
		Check(db.Count(err) == 0, "count empty");

		ArtifactRecord rec;
		rec.hash = HashBytes("texdata", 7);
		rec.klass = "texture";
		rec.uri = "models/x/tex.png";
		rec.pak = "artifacts/textures.pak";
		rec.offset = 128;
		rec.size = 4096;
		rec.meta = "{\"format\":\"ktx2\"}";
		Check(db.Put(rec, err), "put #1");
		Check(db.Count(err) == 1, "count one");

		ArtifactRecord got;
		Check(db.Get("texture", "models/x/tex.png", got, err), "get hit");
		Check(got.hash == rec.hash && got.pak == rec.pak
			&& got.offset == 128 && got.size == 4096
			&& got.meta == rec.meta && got.created > 0,
			"get round-trip");
		Check(!db.Get("mesh", "models/x/tex.png", got, err), "get class miss");
		Check(!db.Get("texture", "nope", got, err), "get uri miss");

		ArtifactRecord byHash;
		Check(db.GetByHash(rec.hash, byHash, err), "by-hash hit");
		Check(byHash.uri == rec.uri, "by-hash round-trip");
		Check(!db.GetByHash("0000000000000000", byHash, err), "by-hash miss");

		// Upsert: re-register the same (klass, uri) with new payload.
		ArtifactRecord rec2 = rec;
		rec2.offset = 8192;
		rec2.size = 8;
		Check(db.Put(rec2, err), "put upsert");
		Check(db.Count(err) == 1, "upsert keeps one row");
		Check(db.Get("texture", "models/x/tex.png", got, err)
			&& got.offset == 8192, "upsert latest wins");

		// Second class + listing + remove.
		ArtifactRecord mesh;
		mesh.hash = "abcdef0123456789";
		mesh.klass = "mesh";
		mesh.uri = "models/x/m#mesh.0";
		mesh.pak = "artifacts/meshes.pak";
		mesh.size = 99;
		Check(db.Put(mesh, err), "put mesh");
		const auto all = db.All(err);
		Check(all.size() == 2, "all lists both");
		Check(db.Remove("mesh", "models/x/m#mesh.0", err), "remove");
		Check(db.Count(err) == 1, "count after remove");

		// Corrupt-record guards: empty fields reject.
		ArtifactRecord bad;
		bad.klass = "texture";
		Check(!db.Put(bad, err), "record needs uri+hash");
	}

	// Reopen: schema is idempotent and rows persist.
	{
		CacheDb db;
		Check(db.Open(dbPath, err), "reopen");
		Check(db.Count(err) == 1, "rows persist across open");
		ArtifactRecord got;
		Check(db.Get("texture", "models/x/tex.png", got, err)
			&& got.offset == 8192, "persisted upsert value");
	}
	// WAL sidecars exist next to the db while at least one connection
	// is open; after Close they may remain â€” harmless either way.

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

void TestStore();

// StoreArtifactCacheAdapter: gltf_import's GltfArtifactCache over the
// real store — the adapter is what the editor/PakCooker inject, so its
// round-trip IS the acceleration contract (keyed by the importer's
// TextureConversionHash).
void TestStoreAdapter() {
	using namespace smvk::cache;
	const std::string dir = TempDir() + "/adapter";
	const std::string root = EnsureScratchRoot(dir + "/scratch");
	if (root.empty()) {
		std::cout << "FAIL adapter root\n";
		return;
	}
	CacheDb db;
	std::string err;
	if (!db.Open((std::filesystem::path(root) / "cache.db").string(),
			err)) {
		std::cout << "FAIL adapter db open: " << err << "\n";
		return;
	}
	ArtifactStore store(root, db);
	StoreArtifactCacheAdapter adapter(store);

	const std::string src = "png-bytes";
	const std::string key = TextureConversionHash(src.data(), src.size(),
		true);
	std::vector<uint8_t> payload = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
		0x30, 0xBB, 1, 2, 3 };
	Check(adapter.Put("texture", "models/x/tex.png", payload.data(),
			payload.size(), key, "{\"srgb\":1}"),
		"adapter.put.stages");
	Check(store.StagedCount() == 1, "adapter.stage.count");
	Check(store.Flush(err), "adapter.flush");
	std::vector<uint8_t> got;
	Check(adapter.Get("texture", "models/x/tex.png", got)
		&& got == payload, "adapter.get.roundtrip");
	Check(!adapter.Get("texture", "models/nope.png", got),
		"adapter.get.miss");
	// The key mixes the transfer: a colorspace change can never serve
	// the stale transfer (the VTXfer-sidecar rule, store-flavored).
	const std::string key2 = TextureConversionHash(src.data(), src.size(),
		true);
	const std::string keyLinear = TextureConversionHash(src.data(),
		src.size(), false);
	Check(key == key2 && key != keyLinear, "adapter.hash.transfer.mix");
	ArtifactRecord rec;
	Check(db.Get("texture", "models/x/tex.png", rec, err)
		&& rec.hash == key, "adapter.registry.key");

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

// AssetDatabase (Unity SourceAssetDB analog): .meta sidecars + the
// GUID<->path directory, move/rename healing through rescan, GUID-
// scoped store keys (identically-named textures across models stop
// colliding).
void TestAssetDatabase() {
	using namespace smvk::cache;
	namespace fs = std::filesystem;
	const std::string dir = TempDir() + "/assetdb";
	const std::string proj = dir + "/proj";
	const std::string scratch = dir + "/scratch";
	std::error_code ec;
	fs::create_directories(proj + "/content/models", ec);
	fs::create_directories(scratch, ec);
	const fs::path asset = fs::path(proj)
		/ "content/models/hero.gltf";
	{
		std::ofstream f(asset, std::ios::binary | std::ios::trunc);
		f << "{\"asset\":{\"version\":\"2.0\"}}";
	}
	const std::string guid = "aaaabbbbccccdddd0000111122223333";
	std::string err;
	// Meta sidecar round-trip.
	Check(WriteAssetMeta(asset.generic_string(), guid, "model", err),
		"assetdb.meta.write");
	std::string gotGuid;
	Check(ReadAssetMeta(asset.generic_string(), gotGuid, err)
		&& gotGuid == guid, "assetdb.meta.read");
	Check(ReadAssetMeta("no/such/file.gltf", gotGuid, err) == false
		&& err == "no .meta", "assetdb.meta.miss");
	Check(!IsValidAssetGuid("XYZ") && IsValidAssetGuid(guid),
		"assetdb.guid.valid");
	// Directory: adopt + lookups both ways.
	AssetDatabase adb;
	Check(adb.Open(scratch, proj, err), "assetdb.open");
	Check(adb.AdoptFile(asset.generic_string(), guid, "model", err),
		"assetdb.adopt");
	Check(adb.GuidForPath(asset.generic_string(), gotGuid)
		&& gotGuid == guid, "assetdb.guid.for.path");
	std::string gotPath;
	Check(adb.PathForGuid(guid, gotPath)
		&& fs::path(gotPath) == fs::path(asset.generic_string()),
		"assetdb.path.for.guid");
	// Move + rename (Explorer-side): stale path misses, rescan
	// re-points the row, PathForGuid heals.
	const fs::path moved = fs::path(proj)
		/ "content/models/villain.gltf";
	fs::rename(asset, moved, ec);
	fs::rename(fs::path(asset.generic_string() + ".meta"),
		fs::path(moved.generic_string() + ".meta"), ec);
	Check(!adb.PathForGuid(guid, gotPath), "assetdb.stale.path.miss");
	RescanStats stats;
	Check(adb.RescanProject(stats, err) && stats.moved == 1,
		"assetdb.rescan.moved");
	Check(adb.PathForGuid(guid, gotPath)
		&& fs::path(gotPath) == fs::path(moved.generic_string()),
		"assetdb.rescan.healed");
	// GUID-scoped store keys: same-named textures under different
	// models keep separate rows (Has gates on the hash).
	{
		CacheDb db;
		Check(db.Open((fs::path(scratch) / "cache.db").string(), err),
			"assetdb.store.open");
		ArtifactStore store(scratch, db);
		StoreArtifactCacheAdapter adapter(store);
		const std::string srcA = "AAAA";
		const std::string srcB = "BBBB";
		const std::string hashA = TextureConversionHash(
			srcA.data(), srcA.size(), true);
		const std::string hashB = TextureConversionHash(
			srcB.data(), srcB.size(), true);
		std::vector<uint8_t> out;
		// Unscoped (legacy): the shared uri thrashes (latest wins).
		adapter.SetKeyScope("");
		Check(adapter.Put("texture", "t/diffuse.png", srcA.data(),
			srcA.size(), hashA, ""), "assetdb.put.a.legacy");
		Check(adapter.Has("texture", "t/diffuse.png", hashB) == false,
			"assetdb.has.gates.hash");
		// Scoped: two models, same texture uri, distinct payloads.
		adapter.SetKeyScope(guid);
		Check(adapter.Put("texture", "t/diffuse.png", srcA.data(),
			srcA.size(), hashA, ""), "assetdb.put.a.scoped");
		adapter.SetKeyScope("ffff0000ffff0000ffff0000ffff0000");
		Check(adapter.Put("texture", "t/diffuse.png", srcB.data(),
			srcB.size(), hashB, ""), "assetdb.put.b.scoped");
		adapter.SetKeyScope(guid);
		Check(adapter.Has("texture", "t/diffuse.png", hashA)
			&& adapter.Get("texture", "t/diffuse.png", out)
			&& out.size() == 4, "assetdb.get.a.scoped");
		adapter.SetKeyScope("ffff0000ffff0000ffff0000ffff0000");
		Check(adapter.Get("texture", "t/diffuse.png", out)
			&& out.size() == 4, "assetdb.get.b.scoped");
		// Both rows live side by side (no latest-wins overwrite).
		// (Flush commits the staged puts to the registry + class pak.)
		Check(store.Flush(err), "assetdb.store.flush");
		std::string qerr;
		ArtifactRecord recA, recB;
		Check(db.Get("texture", guid + "/t/diffuse.png", recA, qerr)
			&& recA.hash == hashA, "assetdb.registry.a");
		Check(db.Get("texture",
			"ffff0000ffff0000ffff0000ffff0000/t/diffuse.png",
			recB, qerr) && recB.hash == hashB,
			"assetdb.registry.b");
	}
	fs::remove_all(dir, ec);
}

int main() {
	TestScratchAndRegistry();
	TestStore();
	TestStoreAdapter();
	TestAssetDatabase();
	std::cout << g_checks - g_failures << "/" << g_checks
		<< " passed\n";
	return g_failures == 0 ? 0 : 1;
}

void TestStore() {
	using namespace smvk::cache;
	const std::string dir = TempDir() + "/store";
	const std::string root = EnsureScratchRoot(dir + "/scratch");
	if (root.empty()) {
		std::cout << "FAIL store root\n";
		return;
	}
	CacheDb db;
	std::string err;
	if (!db.Open((std::filesystem::path(root) / "cache.db").string(),
			err)) {
		std::cout << "FAIL store db open: " << err << "\n";
		return;
	}
	ArtifactStore store(root, db);

	// Stage + read-before-flush (write-behind coherence). Texture-class
	// payloads ride the real cooked-texture envelope (BuildTexturePayload:
	// uri + sampler + ktx bytes) â€” exactly what the final paks embed, and
	// what PakBackend::TryRead parses on the VFS path below.
	const std::vector<uint8_t> ktx = { 1, 2, 3, 4, 5 };
	const std::vector<uint8_t> png = ktx;
	const auto envelope = smvk::BuildTexturePayload(
		"models/x/tex.png", 1, 1, 1, 1,
		std::span<const uint8_t>(ktx));
	const std::string srcHash = HashBytes(png.data(), png.size());
	Check(store.Put("texture", "models/x/tex.png", envelope, srcHash,
			"{\"fmt\":\"ktx2\"}", err), "store.stage");
	Check(store.StagedCount() == 1, "store.staged.count");
	{
		std::vector<uint8_t> got;
		Check(store.Get("texture", "models/x/tex.png", got, err)
			&& got == envelope, "store.staged.get");
	}
	Check(store.Has("texture", "models/x/tex.png", srcHash),
		"store.has.staged");
	Check(!store.Has("texture", "models/x/tex.png",
		HashBytes("other", 5)), "store.has.hash.miss");

	// Flush: pak grows + registry commits; reads switch to disk.
	Check(store.Flush(err), "store.flush");
	Check(store.StagedCount() == 0, "store.flush.empties");
	{
		std::vector<uint8_t> got;
		Check(store.Get("texture", "models/x/tex.png", got, err)
			&& got == envelope, "store.disk.get");
		Check(!store.Get("texture", "nope", got, err),
			"store.get.miss");
	}

	// The grown pak is a VALID pak: PakReader opens it (CRC included),
	// PakBackend serves the entry by URI through a VFS overlay.
	{
		std::vector<uint8_t> img;
		Check(store.ReadPakImage("texture", img, err),
			"store.read.pak.image");
		auto image = std::make_shared<const std::vector<uint8_t>>(
			std::move(img));
		auto r = smvk::PakReader::Open(
			std::span<const uint8_t>(*image), err);
		Check(r.has_value(), "store.pak.reader.open");
		if (r) {
			const auto* e = r->Find(smvk::PakAssetKind::Texture,
				"models/x/tex.png");
			Check(e != nullptr && e->size == envelope.size(),
				"store.pak.find");
			// Registry addressing == TOC addressing (the store's
			// direct-read contract).
			ArtifactRecord rec;
			Check(db.Get("texture", "models/x/tex.png", rec, err)
				&& rec.offset == e->offset && rec.size == e->size,
				"store.registry.addressing.matches.toc");
		}
		smvk::OverlayVfs vfs;
		std::string berr;
		auto backend = smvk::PakBackend::Open(image, "scratch", berr);
		Check(backend != nullptr, "store.backend.open");
		if (backend) {
			vfs.Mount(backend, 50, "scratch");
			auto served = vfs.TryRead("models/x/tex.png");
			// PakBackend serves the TEXTURE PAYLOAD'S ktx bytes (the
			// cooked-artifact contract, exactly like a final pak).
			Check(served && served->size() == ktx.size()
				&& std::equal(ktx.begin(), ktx.end(), served->begin()),
				"store.vfs.serves");
			Check(!vfs.TryRead("models/nope.png").has_value(),
				"store.vfs.miss");
		}
	}

	// Batch flush across classes + a second flush on the same pak
	// (append-grow: the pak keeps opening valid).
	{
		const std::vector<uint8_t> m1 = { 6, 6, 6 };
		const std::vector<uint8_t> t2(77, 0x77);
		Check(store.Put("mesh", "models/x/m#mesh.0", m1,
			HashBytes("meshsrc", 8), "{}", err), "store.stage.mesh");
		Check(store.Put("texture", "models/x/tex2.png", t2,
			HashBytes("png2", 4), "{}", err), "store.stage.tex2");
		Check(store.Flush(err), "store.flush.two.classes");
		std::vector<uint8_t> gotM, gotT;
		Check(store.Get("mesh", "models/x/m#mesh.0", gotM, err)
			&& gotM == m1, "store.mesh.get");
		Check(store.Get("texture", "models/x/tex2.png", gotT, err)
			&& gotT.size() == 77, "store.tex2.get");
		// Old entry still reads after the second growth.
		Check(store.Get("texture", "models/x/tex.png", gotT, err)
			&& gotT == envelope, "store.old.entry.survives.growth");
	}

	// Upsert through the store: latest payload wins, old bytes become
	// dead space (registry row + TOC entry both replaced).
	{
		const std::vector<uint8_t> v2 = { 9, 9, 9, 9 };
		Check(store.Put("texture", "models/x/tex.png", v2,
			HashBytes("texv2", 5), "{}", err), "store.stage.upsert");
		Check(store.Flush(err), "store.flush.upsert");
		std::vector<uint8_t> got;
		Check(store.Get("texture", "models/x/tex.png", got, err)
			&& got == v2, "store.upsert.latest.wins");
		std::string err2;
		Check(!store.Has("texture", "models/x/tex.png", srcHash),
			"store.upsert.has.old.hash.miss");
	}

	// Persistence: a fresh store over the same root reads everything.
	{
		ArtifactStore store2(root, db);
		std::vector<uint8_t> got;
		Check(store2.Get("texture", "models/x/tex.png", got, err)
			&& got.size() == 4, "store.persisted.get");
		Check(store2.Get("mesh", "models/x/m#mesh.0", got, err)
			&& got.size() == 3, "store.persisted.mesh");
	}

	// Remove + ResetClass (corruption repair path).
	{
		Check(store.Remove("mesh", "models/x/m#mesh.0", err),
			"store.remove");
		std::vector<uint8_t> got;
		Check(!store.Get("mesh", "models/x/m#mesh.0", got, err),
			"store.removed.gone");
		Check(store.ResetClass("texture", err), "store.reset.class");
		Check(!store.Get("texture", "models/x/tex.png", got, err),
			"store.reset.cleared");
		Check(!std::filesystem::exists(
				std::filesystem::path(root) / "artifacts"
				/ "texture.pak"), "store.reset.pak.gone");
		std::string cerr;
		Check(db.Count(cerr) == 0, "store.reset.registry.empty");
	}

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}
