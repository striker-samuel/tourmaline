// test_cache: artifact-cache library tests (scratch root handling +
// SQLite registry round-trips). No GPU, no editor. Run via CTest.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <artifact_cache/artifact_cache.h>

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

int main() {
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
	// is open; after Close they may remain — harmless either way.

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::cout << g_checks - g_failures << "/" << g_checks << " passed\n";
	return g_failures == 0 ? 0 : 1;
}
