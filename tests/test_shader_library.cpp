// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// shader_library_tests: CPU-only regression tests for compiled
// shaders as first-class pak/VFS assets (PakAssetKind::Shader) and
// the unified VFS-backed shader reader (Engine/Utils/
// shader_library.h): pak round trip (write -> PakBackend serve ->
// validated read), bytecode detection (SPIR-V vs DXBC vs garbage),
// validation failures (never a driver crash), the dev-override
// precedence (loose wins in dev), and the named diagnostics.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <Engine/Asset/pak.h>
#include <Engine/Utils/shader_library.h>
#include <Engine/Vfs/vfs.h>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_checks = 0;

// The test exe's path (main() fills it; TestCookerPack resolves
// Pak.exe beside it — never CWD, ctest/hosts launch from anywhere).
const char* g_argv0 = "";

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

// A well-formed SPIR-V payload (the magic + a size that parses; the
// library only validates the magic — the driver owns the rest).
std::vector<uint8_t> SpirvPayload(uint32_t words = 8,
	uint32_t seed = 0) {
	std::vector<uint8_t> bytes;
	const uint32_t magic = 0x07230203u;
	auto pushU32 = [&](uint32_t v) {
		for (int i = 0; i < 4; ++i)
			bytes.push_back(uint8_t(v >> (8 * i)));
	};
	pushU32(magic);          // the SPIR-V magic
	pushU32(0x00010300u);    // version
	pushU32(0xdeadbe0f + seed); // generator + bound
	pushU32(0u);             // schema
	for (uint32_t i = 0; i + 4 < words; ++i)
		pushU32(seed + i);
	return bytes;
}

// UNIQUE test paths (never present in the exe's own build tree — the
// exe-relative dev discovery below must not mask the scenarios).
const char* kSpirvPath =
	"content/shaders/testonly.default.vert.hlsl.spv";
const char* kSpirvPath2 =
	"content/shaders/testonly.sky.frag.hlsl.spv";

void WritePak(const fs::path& pakPath,
	const std::vector<std::pair<std::string,
		std::vector<uint8_t>>>& blobs) {
	smvk::PakWriter writer;
	for (const auto& [path, payload] : blobs) {
		if (!writer.AddBlob(smvk::PakAssetKind::Shader, path, payload)) {
			std::cout << "  (AddBlob failed for " << path << ")\n";
		}
	}
	const std::vector<uint8_t> image = writer.Finalize();
	std::ofstream f(pakPath, std::ios::binary | std::ios::trunc);
	f.write(reinterpret_cast<const char*>(image.data()),
		std::streamsize(image.size()));
}

void TestDetect() {
	const std::vector<uint8_t> spirv = SpirvPayload();
	Check(smvk::shaders::DetectBytecode(spirv.data(), spirv.size())
			== smvk::shaders::Bytecode::Spirv,
		"detect.spirv");
	const std::vector<uint8_t> dxbc{ 'D', 'X', 'B', 'C', 0, 0, 0, 0 };
	Check(smvk::shaders::DetectBytecode(dxbc.data(), dxbc.size())
			== smvk::shaders::Bytecode::Dxil,
		"detect.dxil");
	const std::vector<uint8_t> garbage{ 1, 2, 3, 4, 5 };
	Check(smvk::shaders::DetectBytecode(garbage.data(), garbage.size())
			== smvk::shaders::Bytecode::Unknown,
		"detect.garbage");
	Check(smvk::shaders::DetectBytecode(spirv.data(), 2)
			== smvk::shaders::Bytecode::Unknown,
		"detect.truncated");
	Check(smvk::shaders::DetectBytecode(nullptr, 0)
			== smvk::shaders::Bytecode::Unknown,
		"detect.empty");
}

void TestPakKind() {
	// The kind round trip: PakWriter -> image -> PakReader, served by
	// PakBackend through the SAME VFS surface textures/sounds use.
	smvk::PakWriter writer;
	const std::vector<uint8_t> spirv = SpirvPayload();
	Check(writer.AddBlob(smvk::PakAssetKind::Shader, kSpirvPath,
			spirv),
		"pakkind.add-blob");
	const std::vector<uint8_t> image = writer.Finalize();
	std::string err;
	auto reader = smvk::PakReader::Open(image, err);
	Check(bool(reader), "pakkind.image-opens");
	if (!reader)
		return;
	Check(reader->Find(smvk::PakAssetKind::Shader, kSpirvPath)
			!= nullptr,
		"pakkind.find-by-path");
	Check(reader->BlobsOfKind(smvk::PakAssetKind::Shader).size() == 1,
		"pakkind.blobs-of-kind");
	// The backend serves it through the VFS surface.
	auto backend = smvk::PakBackend::Open(
		std::make_shared<std::vector<uint8_t>>(image), "test.pak",
		err);
	Check(bool(backend), "pakkind.backend-opens");
	if (!backend)
		return;
	Check(backend->Exists(kSpirvPath), "pakkind.backend-exists");
	const auto read = backend->TryRead(kSpirvPath);
	Check(read && *read == spirv, "pakkind.backend-reads-bytes");
	const auto st = backend->Stat(kSpirvPath);
	Check(st && st->size == spirv.size()
			&& st->source.find("test.pak") != std::string::npos,
		"pakkind.backend-stat");
	Check(!backend->Exists("content/shaders/nope.spv"),
		"pakkind.backend-miss");
	// ListDir surfaces the shader path like the other kinds.
	const std::vector<smvk::VfsPath> listed =
		backend->ListDir("content/shaders");
	Check(std::find(listed.begin(), listed.end(),
			std::string(kSpirvPath)) != listed.end(),
		"pakkind.backend-listdir");
}

void TestLibrary() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_shaderlib";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);

	// A clean library state for the scenario (the process-wide lib).
	smvk::shaders::Shutdown();
	Check(!smvk::shaders::Initialized(), "lib.starts-uninitialized");

	// MISSING everywhere: the named diagnostic lists the mounts.
	{
		std::vector<uint8_t> out;
		std::string provider, err;
		Check(!smvk::shaders::TryRead(kSpirvPath, out, provider, err)
			&& err.find(kSpirvPath) != std::string::npos,
			"lib.missing-names-path");
	}

	// The engine package round trip: write a pak with two Shader
	// blobs, mount it, read through the LIBRARY (validation on).
	const fs::path pak = tmp / "engine-shaders-test.pak";
	const std::vector<uint8_t> spirv1 = SpirvPayload(8, 1);
	const std::vector<uint8_t> spirv2 = SpirvPayload(12, 2);
	WritePak(pak, { { kSpirvPath, spirv1 },
		{ kSpirvPath2, spirv2 } });
	{
		std::string err;
		Check(smvk::shaders::MountPak(pak.generic_string(), err),
			"lib.mounts-pak");
		// A CORRUPT package refuses to mount (the CRC gate).
		const fs::path bad = tmp / "corrupt.pak";
		{
			std::ofstream f(bad, std::ios::binary | std::ios::trunc);
			f << "SMP7garbage-not-a-pak";
		}
		std::string berr;
		Check(!smvk::shaders::MountPak(bad.generic_string(), berr)
			&& berr.find("corrupt.pak") != std::string::npos,
			"lib.corrupt-pak-refuses");
	}
	{
		std::vector<uint8_t> out;
		std::string provider, err;
		Check(smvk::shaders::TryRead(kSpirvPath, out, provider, err)
			&& out == spirv1
			&& provider.find("engine-shaders-test.pak")
				!= std::string::npos,
			"lib.reads-from-pak");
	}
	// The throwing form (the readFile drop-in contract).
	{
		bool threw = false;
		try {
			const std::vector<char> code =
				smvk::shaders::Read(kSpirvPath2);
			threw = code.size() != spirv2.size();
		}
		catch (const std::runtime_error&) {
			threw = true;
		}
		Check(!threw, "lib.read-throwing-form-ok");
	}

	// VALIDATION: a Shader blob with garbage bytes fails the read
	// with a named provider — never reaching a module.
	{
		const fs::path bad = tmp / "engine-shaders-bad.pak";
		WritePak(bad, { { kSpirvPath, { 1, 2, 3, 4, 5, 6, 7, 8 } } });
		std::string merr;
		Check(smvk::shaders::MountPak(bad.generic_string(), merr),
			"lib.mounts-bad-pak");
		// The DEV OVERRIDE below masks it in this scenario — prove the
		// validation directly by unmounting everything else first.
		smvk::shaders::Unmount("shaderpak:engine-shaders-bad.pak");
		smvk::shaders::Unmount("shaderpak:engine-shaders-test.pak");
		const fs::path only = tmp / "only-bad.pak";
		WritePak(only, { { kSpirvPath, { 1, 2, 3, 4, 5, 6, 7, 8 } } });
		std::string err2;
		Check(smvk::shaders::MountPak(only.generic_string(), err2),
			"lib.mounts-only-bad");
		std::vector<uint8_t> out;
		std::string provider, err;
		Check(!smvk::shaders::TryRead(kSpirvPath, out, provider, err)
			&& err.find("validation failed") != std::string::npos,
			"lib.garbage-fails-validation");
		bool threw = false;
		try {
			(void)smvk::shaders::Read(kSpirvPath);
		}
		catch (const std::runtime_error& e) {
			threw = std::string(e.what())
					.find("validation failed")
				!= std::string::npos;
		}
		Check(threw, "lib.garbage-throws-named");
		smvk::shaders::Unmount("shaderpak:only-bad.pak");
	}

	// The DEV OVERRIDE: an explicitly mounted loose tree wins over the
	// packages (recompiled shaders serve immediately in dev).
	{
		const fs::path devRoot = tmp / "devroot";
		fs::create_directories(devRoot / "content" / "shaders", ec);
		const std::vector<uint8_t> devBytes = SpirvPayload(8, 99);
		{
			std::ofstream f(devRoot / "content" / "shaders"
				/ "testonly.default.vert.hlsl.spv",
				std::ios::binary);
			f.write(reinterpret_cast<const char*>(devBytes.data()),
				std::streamsize(devBytes.size()));
		}
		std::string merr;
		Check(smvk::shaders::MountLooseDir(devRoot.generic_string(),
				"dev-override", merr),
			"lib.mounts-dev-override");
		std::vector<uint8_t> out;
		std::string provider, err;
		Check(smvk::shaders::TryRead(kSpirvPath, out, provider, err)
			&& out == devBytes,
			"lib.dev-override-wins");
		// Unmounted: the packages serve again.
		Check(smvk::shaders::Unmount("dev-override"),
			"lib.unmounts-dev-override");
		std::vector<uint8_t> out2;
		std::string provider2, err2;
		// (Re-mount the good package — the validation scenario
		// unmounted it above.)
		std::string merr2;
		(void)smvk::shaders::MountPak(pak.generic_string(), merr2);
		Check(smvk::shaders::TryRead(kSpirvPath, out2, provider2, err2)
			&& out2 == spirv1,
			"lib.pak-serves-after-override-gone");
	}

	// Shutdown resets (the test hygiene + the full-graphics-shutdown
	// contract).
	smvk::shaders::Shutdown();
	Check(!smvk::shaders::Initialized(), "lib.shutdown-resets");
	{
		std::vector<uint8_t> out;
		std::string provider, err;
		// A read after shutdown lazily re-initializes (exe-relative
		// discovery only — no test mounts) and misses cleanly.
		Check(!smvk::shaders::TryRead(kSpirvPath, out, provider, err),
			"lib.post-shutdown-misses-cleanly");
	}
}

void TestCookerPack() {
	// The ENGINE SHADER PACKAGE flow (Pak.exe --shaders-list): the
	// DECLARED list packs EXACTLY its files (a garbage .spv NOT in the
	// list never ships), every listed blob validates, the package
	// opens through the library, and a list naming a corrupt blob
	// FAILS the pack.
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_shaderpak";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	const fs::path spvA = tmp / "testonly.a.vert.hlsl.spv";
	const fs::path spvB = tmp / "testonly.b.frag.hlsl.spv";
	const fs::path spvStale = tmp / "testonly.stale.frag.hlsl.spv";
	{
		const std::vector<uint8_t> a = SpirvPayload(8, 11);
		const std::vector<uint8_t> b = SpirvPayload(16, 12);
		std::ofstream fa(spvA, std::ios::binary);
		fa.write(reinterpret_cast<const char*>(a.data()),
			std::streamsize(a.size()));
		std::ofstream fb(spvB, std::ios::binary);
		fb.write(reinterpret_cast<const char*>(b.data()),
			std::streamsize(b.size()));
		// NOT in the declared list (the retired-variant stand-in) and
		// CORRUPT on top — the directory-walk form would ship it.
		std::ofstream fs2(spvStale, std::ios::binary);
		fs2 << "garbage";
	}
	const fs::path listFile = tmp / "shaders.list";
	{
		std::ofstream f(listFile, std::ios::binary | std::ios::trunc);
		f << "# the declared list\n"
			<< spvA.generic_string() << "\n"
			<< spvB.generic_string() << "\n";
	}
	const fs::path pak = tmp / "engine-shaders-test.pak";
	// Pak.exe sits beside this test exe (the shared build root) —
	// resolve it EXE-relative (never CWD: ctest/hosts launch from
	// anywhere), with NATIVE separators. NO embedded quotes: the CRT's
	// system() wraps the string in a leading-quote cmd /c form whose
	// quote-stripping rule mangles multi-quoted commands ("filename
	// syntax is incorrect"); every path here is space-free (the test
	// temp dir + the repo), so bare tokens parse cleanly.
	auto runPak = [&](const std::string& extra) -> int {
		const std::string exe =
			(fs::path(g_argv0).parent_path() / "Pak.exe").string();
		return std::system((exe + " " + extra).c_str());
	};
	{
		const std::string cmd = "--shaders-list "
			+ listFile.string() + " --out-pak "
			+ pak.string() + " --compress on";
		Check(runPak(cmd) == 0, "cook.declared-list-packs");
	}
	// The package serves through the library; the STALE blob never
	// shipped (the declared-list discipline).
	smvk::shaders::Shutdown();
	{
		std::string merr;
		Check(smvk::shaders::MountPak(pak.generic_string(), merr),
			"cook.mounts-package");
		std::vector<uint8_t> out;
		std::string provider, err;
		Check(smvk::shaders::TryRead(
				"content/shaders/testonly.a.vert.hlsl.spv", out,
				provider, err)
			&& out.size() == 32,
			"cook.reads-declared-a");
		Check(smvk::shaders::TryRead(
				"content/shaders/testonly.b.frag.hlsl.spv", out,
				provider, err),
			"cook.reads-declared-b");
		Check(!smvk::shaders::TryRead(
				"content/shaders/testonly.stale.frag.hlsl.spv", out,
				provider, err),
			"cook.stale-never-shipped");
		smvk::shaders::Shutdown();
	}
	// A declared list naming a CORRUPT blob fails the pack (never
	// ships, never reaches a driver).
	{
		const fs::path badList = tmp / "shaders-bad.list";
		{
			std::ofstream f(badList, std::ios::binary | std::ios::trunc);
			f << spvStale.generic_string() << "\n";
		}
		const fs::path badPak = tmp / "engine-shaders-bad.pak";
		const std::string cmd = "--shaders-list "
			+ badList.string() + " --out-pak "
			+ badPak.string() + " --compress on";
		Check(runPak(cmd) != 0, "cook.corrupt-declared-fails");
		Check(!fs::exists(badPak, ec), "cook.corrupt-wrote-nothing");
	}
}

} // namespace

int main(int argc, char** argv) {
	if (argc > 0 && argv[0])
		g_argv0 = argv[0];
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	std::cout << std::unitbuf;
	TestDetect();
	TestPakKind();
	TestLibrary();
	TestCookerPack();
	std::cout << "shader_library_tests: "
		<< (g_checks - g_failures) << "/" << g_checks
		<< " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
