// flintbase_tests: headless unit tests for the reserved //flintbase/
// VFS scheme: path/URI normalization, OverlayVfs scheme routing, and
// the FlintbaseBackend against a fake IFlintbaseApi (no network, no
// GPU). Run via CTest.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <Engine/Asset/asset_registry.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/Flintbase/flintbase_client.h>
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

void TestSchemeNormalize() {
	using smvk::vfs::Normalize;
	Check(Normalize("//flintbase/proj/item/task/v001/pak")
		== "//flintbase/proj/item/task/v001/pak", "fb.norm.verbatim");
	Check(Normalize("\\\\flintbase\\proj\\item")
		== "//flintbase/proj/item", "fb.norm.backslash");
	Check(Normalize("//FLINTBASE/proj") == "//flintbase/proj", "fb.norm.case");
	Check(Normalize("//flintbase//proj/./x") == "//flintbase/proj/x", "fb.norm.segments");
	Check(Normalize("//flintbase") == "//flintbase/", "fb.norm.bare.scheme.root");
	Check(Normalize("//flintbase/") == "//flintbase/", "fb.norm.scheme.root");
	Check(Normalize("//flintbase/proj/../..") == "", "fb.norm.escape");
	Check(Normalize("//flintbase/proj/../../x") == "", "fb.norm.escape2");
	Check(Normalize("//other/proj") == "", "fb.norm.unknown.authority");
	Check(Normalize("//flintbased/thing") == "", "fb.norm.authority.prefix.not.scheme");
	Check(smvk::vfs::IsFlintbasePath(Normalize("//flintbase/a/b")), "fb.is.scheme");
	Check(!smvk::vfs::IsFlintbasePath("models/Sponza/sponza.gltf"), "fb.is.not.scheme");
	Check(std::string(smvk::vfs::FlintbaseRemainder(Normalize("//flintbase/proj/item")))
		== "proj/item", "fb.remainder");
	// Regular-path behavior is untouched.
	Check(Normalize("models/Sponza/sponza.gltf") == "models/Sponza/sponza.gltf", "fb.norm.regular");
	Check(Normalize("/absolute") == "", "fb.norm.regular.absolute");
	// Join/BaseDir/FileName operate on scheme paths too.
	Check(smvk::vfs::Join("//flintbase/proj/item", "file.pak")
		== "//flintbase/proj/item/file.pak", "fb.join");
	Check(smvk::vfs::BaseDir("//flintbase/proj/item/file.pak") == "//flintbase/proj/item", "fb.basedir");
	Check(smvk::vfs::FileName("//flintbase/proj/item/file.pak") == "file.pak", "fb.filename");
}

void TestSchemeAssetUris() {
	using smvk::Arena;
	using smvk::StringViewT;
	Arena scratch(4096);

	const StringViewT uri("//flintbase/proj/item/task/v001/pak#mesh.1/prim.0");
	Check(smvk::UriAlreadyNormalized(uri), "fb.uri.already.normalized");
	const StringViewT n = smvk::NormalizeAssetUri(uri, scratch);
	Check(n == uri, "fb.uri.roundtrip");

	const StringViewT messy = smvk::NormalizeAssetUri(
		StringViewT("\\\\FLINTBASE\\proj\\./item\\task\\v001\\pak#mesh.1"), scratch);
	Check(messy == StringViewT("//flintbase/proj/item/task/v001/pak#mesh.1"), "fb.uri.rewrite");
	Check(smvk::NormalizeAssetUri(StringViewT("//flintbase/proj/../..#f"), scratch)
		== StringViewT(), "fb.uri.escape");
	Check(smvk::NormalizeAssetUri(StringViewT("//flintbase/#f"), scratch)
		== StringViewT(), "fb.uri.bare.root");
}

class FakeApi final : public smvk::IFlintbaseApi {
public:
	bool Resolve(const std::string& remainder, smvk::FlintbaseResolved& out,
		smvk::FlintbaseError& error) override {
		++calls;
		smvk::FlintbaseResolved r;
		r.status = "finished";
		r.rootFolder = "(fake)";
		if (remainder == "proj/item/task/v001/pak") {
			r.resolvedPath = "//flintbase/proj/item/task/v001/pak";
			r.versionNumber = 1;
			r.files.push_back({"scene.pak", 29, kPayloadSha, "fb_test_payload_a.bin"});
			r.files.push_back({"textures/foo.ktx2", 8, kPayloadSha, "fb_test_payload_b.bin"});
			out = std::move(r);
			return true;
		}
		if (remainder == "proj/item/task/v001/pak/scene.pak") {
			r.resolvedPath = "//flintbase/proj/item/task/v001/pak/scene.pak";
			r.versionNumber = 1;
			r.files.push_back({"scene.pak", 29, kPayloadSha, "fb_test_payload_a.bin"});
			r.fileIndex = 0;
			out = std::move(r);
			return true;
		}
		if (remainder == "proj/item/task/latest/pak") {
			// Alternating result proves the backend pins 'latest' per process.
			r.versionNumber = (calls == 1) ? 1 : 2;
			r.resolvedPath = "//flintbase/proj/item/task/v00"
				+ std::to_string(r.versionNumber) + "/pak";
			r.files.push_back({"scene.pak", 29, kPayloadSha, "fb_test_payload_a.bin"});
			out = std::move(r);
			return true;
		}
		if (remainder == "proj/item/task/v002/badhash") {
			r.versionNumber = 2;
			r.resolvedPath = "//flintbase/proj/item/task/v002/badhash";
			r.files.push_back({"bad.bin", 29, std::string(64, '0'), "fb_test_payload_a.bin"});
			out = std::move(r);
			return true;
		}
		error = {404, "fake: no such product '" + remainder + "'"};
		return false;
	}

	int calls = 0;
	static constexpr const char* kPayloadSha =
		"60b0b808546ee0cc39e16a452d499936939fcf6086cf1f653e44e2391d1f82dc";
};

void WriteTestFile(const char* name, const char* content) {
	std::ofstream f(name, std::ios::binary);
	f << content;
}

void TestBackendAgainstFake() {
	auto fake = std::make_shared<FakeApi>();
	smvk::FlintbaseBackend backend(fake);

	const std::string pakPath = "//flintbase/proj/item/task/v001/pak";
	Check(backend.Exists(pakPath), "fb.backend.exists");
	Check(!backend.Exists("//flintbase/proj/item/task/v001/nonsense"), "fb.backend.exists.not");

	// Read with manifest hash verification (file content matches kPayloadSha).
	auto bytes = backend.TryRead("//flintbase/proj/item/task/v001/pak/scene.pak");
	Check(bytes && bytes->size() == 29 && !std::strcmp((const char*)bytes->data(),
		"flintbase vfs test payload v1"), "fb.backend.tryread.verified");
	// Multi-file product without a file segment does not read as a file.
	Check(!backend.TryRead(pakPath), "fb.backend.tryread.multifile.rejected");
	// Hash mismatch fails the read.
	Check(!backend.TryRead("//flintbase/proj/item/task/v002/badhash"), "fb.backend.tryread.hashmismatch");

	// Stat from the manifest (file-addressed path).
	auto st = backend.Stat(pakPath + "/scene.pak");
	Check(st && st->backend == "flintbase" && st->source == "fb_test_payload_a.bin"
		&& st->size == 29, "fb.backend.stat");

	// ListDir: product root lists files and directory markers.
	const auto entries = backend.ListDir(pakPath);
	Check(entries.size() == 2
		&& entries[0] == pakPath + "/scene.pak"
		&& entries[1] == pakPath + "/textures", "fb.backend.listdir.root");
	// ListDir inside a product subdirectory.
	const auto sub = backend.ListDir(pakPath + "/textures");
	Check(sub.size() == 1 && sub[0] == pakPath + "/textures/foo.ktx2", "fb.backend.listdir.subdir");

	// 'latest' is pinned per process: one API call regardless of reads.
	const int callsBefore = fake->calls;
	Check(backend.TryRead("//flintbase/proj/item/task/latest/pak").has_value(), "fb.backend.latest.read1");
	Check(backend.TryRead("//flintbase/proj/item/task/latest/pak").has_value(), "fb.backend.latest.read2");
	Check(fake->calls == callsBefore + 1, "fb.backend.latest.pinned.cache");

	// Unknown version resolves once and is cached as a negative too.
	Check(!backend.TryRead("//flintbase/proj/item/task/v099/pak"), "fb.backend.miss");
}

void TestOverlayRouting() {
	auto fake = std::make_shared<FakeApi>();
	smvk::OverlayVfs vfs;
	auto mem = std::make_shared<smvk::MemBackend>("mem");
	mem->Write("models/thing.pak", {'o', 'k'});
	vfs.Mount(mem, 0, "mem");
	vfs.Mount(std::make_shared<smvk::FlintbaseBackend>(fake), 50, "flintbase");

	Check(vfs.TryRead("models/thing.pak") && (*vfs.TryRead("models/thing.pak"))[0] == 'o',
		"fb.routing.regular.unaffected");
	Check(vfs.TryRead("//flintbase/proj/item/task/v001/pak/scene.pak").has_value(),
		"fb.routing.scheme.served");
	Check(!vfs.Exists("//flintbase/nope/nope/nope/v001/nope"),
		"fb.routing.scheme.miss.is.miss");
	Check(vfs.FindProviders("//flintbase/proj/item/task/v001/pak")
		== std::vector<std::string>{"flintbase"}, "fb.routing.providers");

	// A regular-only overlay never serves scheme paths.
	smvk::OverlayVfs regularOnly;
	regularOnly.Mount(mem, 0, "mem");
	Check(!regularOnly.Exists("//flintbase/proj/item/task/v001/pak"),
		"fb.routing.regular.only.never.serves.scheme");

	// A scheme-only overlay never serves regular paths.
	smvk::OverlayVfs schemeOnly;
	schemeOnly.Mount(std::make_shared<smvk::FlintbaseBackend>(fake), 0, "flintbase");
	Check(!schemeOnly.Exists("models/thing.pak"), "fb.routing.scheme.only.never.serves.regular");
}

void TestHostPathHelper() {
	// Non-scheme paths pass through unchanged.
	Check(smvk::FlintbaseHostPath("content/models/thing.gltf")
		== "content/models/thing.gltf", "fb.hostpath.passthrough");
	// Scheme paths resolve through the default client; the API is not
	// reachable in the test environment, so resolution fails empty.
	const std::string got = smvk::FlintbaseHostPath(
		"//flintbase/proj/item/task/v001/pak/scene.pak");
	Check(got.empty(), "fb.hostpath.no.api.empty");
}

// Live round-trip against a real Flintbase API. Skipped under CTest;
// opt in with FLINTBASE_LIVE_TEST=1 (and optionally FLINTBASE_LIVE_PATH
// + FLINTBASE_API_URL) with the API and Postgres running.
void TestLiveApi() {
	const char* enabled = std::getenv("FLINTBASE_LIVE_TEST");
	if (!enabled || !*enabled)
		return;
	const char* pathEnv = std::getenv("FLINTBASE_LIVE_PATH");
	const std::string path = pathEnv && *pathEnv
		? pathEnv : "//flintbase/e2e/chr_e2etest/model/low/latest/pak2";
	std::cout << "  live: resolving '" << path << "'\n";

	smvk::FlintbaseBackend backend; // default FlintbaseClient (FLINTBASE_API_URL)
	auto bytes = backend.TryRead(path);
	Check(bytes.has_value(), "fb.live.read");
	if (bytes) {
		std::cout << "  live: read " << bytes->size() << " bytes\n";
		auto st = backend.Stat(path);
		Check(st && st->size == bytes->size(), "fb.live.stat.matches");
	}
	smvk::FlintbaseError error;
	const smvk::FlintbaseResolved* r = backend.ResolveCached(path, error);
	Check(r && r->status == "finished" && !r->files.empty(), "fb.live.resolve.cached");
	if (r) {
		std::cout << "  live: " << r->resolvedPath
			<< " manifest " << r->contentsHash.substr(0, 12) << "...\n";
	}
}

} // namespace

int main() {
	WriteTestFile("fb_test_payload_a.bin", "flintbase vfs test payload v1");
	WriteTestFile("fb_test_payload_b.bin", "ktx2data");

	TestSchemeNormalize();
	TestSchemeAssetUris();
	TestBackendAgainstFake();
	TestOverlayRouting();
	TestHostPathHelper();
	TestLiveApi();

	std::remove("fb_test_payload_a.bin");
	std::remove("fb_test_payload_b.bin");

	std::cout << (g_failures ? "\nFAILURES: " : "\nALL OK: ")
		<< (g_failures ? g_failures : g_checks) << "\n";
	return g_failures ? 1 : 0;
}
