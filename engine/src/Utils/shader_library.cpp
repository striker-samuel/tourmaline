// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The engine shader library (see shader_library.h): a process-wide
// OverlayVfs over dev-override loose trees + mounted shader paks,
// with bytecode validation on every read. The old reads were
// readFile("content/shaders/...") — CWD-relative with an exe-relative
// fallback, no packages, no validation; this is the unified surface
// that keeps every existing path string working while making the
// engine-shaders.pak the production source.

#include <Engine/Utils/shader_library.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h> // readlink (/proc/self/exe)
#endif

#include <Engine/Asset/pak.h>
#include <Engine/Vfs/vfs.h>

namespace smvk::shaders {

// Forward: the lock-free mount core (defined with the other library
// entries below) — InitLocked runs under Lib().mtx and must not go
// through the locking public wrapper.
bool MountPakUnlocked(const std::string& pakFilePath, std::string& err);

namespace {

namespace fs = std::filesystem;

// SPIR-V magic: the word 0x07230203 — little-endian files (everything
// DXC/glslang emit on the LE platforms this engine targets) start
// with the bytes 03 02 23 07; the mirrored big-endian spelling is
// accepted too (the magic's whole purpose is endianness detection).
constexpr uint8_t kSpirvMagicLe[4] = { 0x03, 0x02, 0x23, 0x07 };
constexpr uint8_t kSpirvMagicBe[4] = { 0x07, 0x23, 0x02, 0x03 };
// DXBC container magic (DXIL artifacts start with the RIFF-style tag).
constexpr uint8_t kDxbcMagic[4] = { 'D', 'X', 'B', 'C' };

struct Library {
	std::mutex mtx;
	OverlayVfs vfs;
	bool inited = false;
	bool looseDevMounted = false; // the exe-relative dev tree
	bool enginePakMounted = false; // the exe-relative engine-shaders.pak
};

Library& Lib() {
	static Library lib;
	return lib;
}

// The executable's directory (deployment independence: discovery is
// exe-relative, NEVER CWD — a relocated deployment finds its
// packages, and running a host from an arbitrary working directory
// changes nothing).
std::string ExeDir() {
#ifdef _WIN32
	char exePath[1024] = {};
	if (GetModuleFileNameA(nullptr, exePath, DWORD(sizeof exePath))) {
		const std::string s(exePath);
		const size_t slash = s.find_last_of("\\/");
		return slash == std::string::npos ? std::string(".")
			: s.substr(0, slash);
	}
	return ".";
#else
	char exePath[1024] = {};
	const ssize_t n = readlink("/proc/self/exe", exePath,
		sizeof exePath - 1);
	if (n > 0) {
		exePath[n] = '\0';
		const std::string s(exePath);
		const size_t slash = s.find_last_of('/');
		return slash == std::string::npos ? std::string(".")
			: s.substr(0, slash);
	}
	return ".";
#endif
}

// The engine package's canonical file name (the build's declared
// engine-shaders.pak; see cmake/CompileShaders.cmake's packaging
// step).
constexpr const char* kEnginePakName = "engine-shaders.pak";

void InitLocked() {
	if (Lib().inited)
		return;
	Lib().inited = true;
	// 1) The exe-relative DEV override (content/shaders/ as synced by
	//    the build next to every host): wins over packages so a
	//    recompiled shader serves immediately in dev. Inert in a
	//    deployment that ships no loose tree.
	{
		std::error_code ec;
		const fs::path devTree = fs::path(ExeDir()) / "content"
			/ "shaders";
		if (fs::is_directory(devTree, ec)) {
			// LooseBackend(root) serves "<root>/<vfsPath>"; the shader
			// paths are "content/shaders/<file>", so the root is the
			// exe dir itself.
			Lib().vfs.Mount(
				std::make_shared<LooseBackend>(ExeDir()), -10,
				"engine-shaders-loose");
			Lib().looseDevMounted = true;
		}
	}
	// 2) The exe-relative engine-shaders.pak (the MANDATORY
	//    production source; missing in a dev tree that only syncs
	//    loose shaders — a loud miss only when a read finds nothing
	//    anywhere). NOTE: the UNLOCKED mount variant — InitLocked runs
	//    with Lib().mtx held (a recursive std::mutex lock is UB and
	//    MSVC fast-fails on it).
	{
		const fs::path pak = fs::path(ExeDir()) / kEnginePakName;
		std::error_code ec;
		if (fs::is_regular_file(pak, ec)) {
			std::string err;
			if (MountPakUnlocked(pak.string(), err)) {
				Lib().enginePakMounted = true;
			}
			else {
				std::fprintf(stderr,
					"[shaderlib] the exe-relative %s exists but"
					" failed to mount: %s\n", kEnginePakName,
					err.c_str());
			}
		}
	}
	std::fprintf(stderr, "[shaderlib] initialized (exe-relative"
		" discovery: %s loose dev tree, %s engine package)\n",
		Lib().looseDevMounted ? "with" : "no",
		Lib().enginePakMounted ? "with" : "no");
}

} // namespace

Bytecode DetectBytecode(const uint8_t* data, size_t size) {
	if (!data || size < 4)
		return Bytecode::Unknown;
	if (std::memcmp(data, kSpirvMagicLe, 4) == 0
			|| std::memcmp(data, kSpirvMagicBe, 4) == 0)
		return Bytecode::Spirv;
	if (std::memcmp(data, kDxbcMagic, 4) == 0)
		return Bytecode::Dxil;
	return Bytecode::Unknown;
}

void Init() {
	Library& lib = Lib();
	std::lock_guard<std::mutex> lock(lib.mtx);
	InitLocked();
}

void Shutdown() {
	Library& lib = Lib();
	std::lock_guard<std::mutex> lock(lib.mtx);
	lib.vfs.Clear();
	lib.inited = false;
	lib.looseDevMounted = false;
	lib.enginePakMounted = false;
}

bool Initialized() {
	Library& lib = Lib();
	std::lock_guard<std::mutex> lock(lib.mtx);
	return lib.inited;
}

bool HasAnySource() {
	Library& lib = Lib();
	std::lock_guard<std::mutex> lock(lib.mtx);
	InitLocked();
	return !lib.vfs.Mounts().empty();
}

bool MountPak(const std::string& pakFilePath, std::string& err) {
	Library& lib = Lib();
	std::lock_guard<std::mutex> lock(lib.mtx);
	InitLocked();
	return MountPakUnlocked(pakFilePath, err);
}

// The core mount (the caller holds Lib().mtx): read + verify the
// image (PakReader::Open validates magic, sizes and the CRC — a
// corrupt package fails HERE, loudly, not at the first pipeline
// build) + serve at production precedence. Split from MountPak so
// InitLocked (which runs under the lock) can mount the exe-relative
// engine package without a RECURSIVE std::mutex lock (UB; MSVC
// fast-fails with 0xC0000409).
bool MountPakUnlocked(const std::string& pakFilePath, std::string& err) {
	err.clear();
	std::ifstream in(pakFilePath, std::ios::binary);
	if (!in) {
		err = "shader package not found: " + pakFilePath;
		return false;
	}
	auto image = std::make_shared<std::vector<uint8_t>>();
	{
		const std::string bytes((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		image->assign(bytes.begin(), bytes.end());
	}
	if (image->empty()) {
		err = "shader package is empty: " + pakFilePath;
		return false;
	}
	std::string openErr;
	std::shared_ptr<PakBackend> backend =
		PakBackend::Open(image, pakFilePath, openErr);
	if (!backend) {
		err = "shader package failed to open: " + pakFilePath + " ("
			+ openErr + ")";
		return false;
	}
	const std::string name = "shaderpak:" + fs::path(pakFilePath)
		.filename().generic_string();
	Lib().vfs.Mount(backend, 0, name);
	return true;
}

bool MountLooseDir(const std::string& rootDir, const std::string& name,
	std::string& err) {
	err.clear();
	std::error_code ec;
	if (!fs::is_directory(fs::path(rootDir), ec)) {
		err = "loose shader tree not found: " + rootDir;
		return false;
	}
	Library& lib = Lib();
	std::lock_guard<std::mutex> lock(lib.mtx);
	InitLocked();
	lib.vfs.Mount(std::make_shared<LooseBackend>(rootDir), -20, name);
	return true;
}

bool Unmount(const std::string& name) {
	Library& lib = Lib();
	std::lock_guard<std::mutex> lock(lib.mtx);
	return lib.vfs.Unmount(name);
}

bool TryRead(const std::string& path, std::vector<uint8_t>& out,
	std::string& provider, std::string& err) {
	err.clear();
	out.clear();
	provider.clear();
	{
		Library& lib = Lib();
		std::lock_guard<std::mutex> lock(lib.mtx);
		InitLocked();
	}
	// The overlay read (dev override first, packages next). A miss
	// names every mounted provider — the actionable diagnostic.
	const std::optional<std::vector<uint8_t>> bytes =
		Lib().vfs.TryRead(path);
	if (!bytes) {
		std::ostringstream os;
		os << "shader not found: " << path << " (mounted: ";
		const std::vector<VfsMount> mounts = Lib().vfs.Mounts();
		if (mounts.empty())
			os << "nothing";
		for (size_t i = 0; i < mounts.size(); ++i) {
			if (i)
				os << ", ";
			os << mounts[i].name;
		}
		os << ")";
		err = os.str();
		return false;
	}
	// VALIDATION: the bytecode magic must be sane before any module is
	// created from it (a corrupt/truncated blob fails HERE with a
	// named path + provider, never inside the driver).
	const Bytecode kind = DetectBytecode(bytes->data(), bytes->size());
	const std::optional<VfsStat> st = Lib().vfs.Stat(path);
	provider = st ? st->source : std::string("vfs");
	if (kind != Bytecode::Spirv) {
		err = "shader bytecode validation failed: " + path + " ("
			+ (kind == Bytecode::Dxil
				? std::string("a DXBC/DXIL blob on the Vulkan"
					" backend")
				: std::string("not SPIR-V (truncated or corrupt)"))
			+ ", served by " + provider + ")";
		return false;
	}
	out = *bytes;
	return true;
}

std::vector<char> Read(const std::string& path) {
	std::vector<uint8_t> bytes;
	std::string provider;
	std::string err;
	if (!TryRead(path, bytes, provider, err))
		throw std::runtime_error(err);
	return std::vector<char>(bytes.begin(), bytes.end());
}

} // namespace smvk::shaders
