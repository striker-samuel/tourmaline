// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The watched shader hot-reload service (see shader_hot_reload.h):
// FileWatcher events over the source tree -> settled .hlsl/.hlsli
// edits -> in-process DXC recompiles (base outputs + the variant
// twins) -> validated .spv bytes landing atomically in the output
// tree the shader library serves. The last-known-good contract holds
// through every failure path: a failed compile never touches the
// previous .spv.

#include <Engine/Utils/shader_hot_reload.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <Engine/Renderer/dxc_integration.h>
#include <Engine/Utils/file_watcher.h>
#include <Engine/Utils/shader_library.h>

namespace smvk::shader_reload {

namespace {

namespace fs = std::filesystem;

// ---- the stage/entry conventions (CompileShaders.cmake's
// _detect_hlsl_stage_and_entry, the runtime mirror) ------------------
bool StageAndEntry(const std::string& fileName,
	DxcShaderStage& stage, std::wstring& entry) {
	auto endsWith = [&](const char* suffix) {
		const size_t n = std::strlen(suffix);
		return fileName.size() >= n
			&& fileName.compare(fileName.size() - n, n, suffix) == 0;
	};
	if (endsWith(".vert.hlsl")) {
		stage = DxcShaderStage::Vertex;
		entry = L"mainVS";
		return true;
	}
	if (endsWith(".frag.hlsl") || endsWith(".pix.hlsl")) {
		stage = DxcShaderStage::Fragment;
		entry = L"mainPS";
		return true;
	}
	if (endsWith(".cs.hlsl") || endsWith(".comp.hlsl")) {
		stage = DxcShaderStage::Compute;
		entry = L"mainCS";
		return true;
	}
	if (endsWith(".mesh.hlsl") || endsWith(".ms.hlsl")) {
		stage = DxcShaderStage::Mesh;
		entry = L"mainMS";
		return true;
	}
	if (endsWith(".task.hlsl") || endsWith(".as.hlsl")) {
		stage = DxcShaderStage::Task;
		entry = L"mainAS";
		return true;
	}
	return false;
}

// ---- THE VARIANT TABLE (the mirror of engine/CMakeLists.txt's
// VARIANTS list — the baked-define permutations). Drift degrades
// gracefully: a twin missing here keeps its build-compiled bytes
// until the next full build; never a WRONG variant. -------------------
const std::map<std::string, std::vector<Variant>>& VariantTable() {
	static const std::map<std::string, std::vector<Variant>> table = {
		{ "hzb_spd.cs.hlsl", {
			{ ".min", { "HZB_REDUCE_MAX=0" } },
		} },
		{ "default_lit.frag.hlsl", {
			{ ".sg5", { "SG_LM_LOBE_COUNT=5" } },
			{ ".sg6", { "SG_LM_LOBE_COUNT=6" } },
			{ ".sg9", { "SG_LM_LOBE_COUNT=9" } },
			{ ".sg12", { "SG_LM_LOBE_COUNT=12" } },
			{ ".gi0", { "GI_NONE=1" } },
			{ ".gi2", { "GI_REGULAR=1" } },
			{ ".gi3", { "GI_VOLUMES=1" } },
			{ ".std", { "MAT_STANDARD=1" } },
			{ ".gi0std", { "GI_NONE=1", "MAT_STANDARD=1" } },
			{ ".gi2std", { "GI_REGULAR=1", "MAT_STANDARD=1" } },
			{ ".gi3std", { "GI_VOLUMES=1", "MAT_STANDARD=1" } },
		} },
		{ "gfxmesh_lit.frag.hlsl", {
			{ ".sg5", { "SG_LM_LOBE_COUNT=5" } },
			{ ".sg6", { "SG_LM_LOBE_COUNT=6" } },
			{ ".sg9", { "SG_LM_LOBE_COUNT=9" } },
			{ ".sg12", { "SG_LM_LOBE_COUNT=12" } },
			{ ".gi0", { "GI_NONE=1" } },
			{ ".gi2", { "GI_REGULAR=1" } },
			{ ".gi3", { "GI_VOLUMES=1" } },
			{ ".std", { "MAT_STANDARD=1" } },
			{ ".gi0std", { "GI_NONE=1", "MAT_STANDARD=1" } },
			{ ".gi2std", { "GI_REGULAR=1", "MAT_STANDARD=1" } },
			{ ".gi3std", { "GI_VOLUMES=1", "MAT_STANDARD=1" } },
		} },
		{ "ssr_compose.frag.hlsl", {
			{ ".dbg", { "SSR_DEBUG_CONTRIB=1" } },
		} },
		{ "default_pbr_debug.frag.hlsl", {
			{ ".base", { "PBR_MODE=1" } },
			{ ".diffuse", { "PBR_MODE=2" } },
			{ ".rough", { "PBR_MODE=3" } },
			{ ".metal", { "PBR_MODE=4" } },
			{ ".f0", { "PBR_MODE=5" } },
		} },
	};
	return table;
}

struct Service {
	std::mutex mtx;
	FileWatcher watcher;
	std::string sourceDir;
	std::string outDir;
	bool started = false;
};
Service& Svc() {
	static Service s;
	return s;
}

std::string ReadText(const fs::path& p, bool& ok) {
	std::ifstream in(p, std::ios::binary);
	if (!in) {
		ok = false;
		return {};
	}
	ok = true;
	return std::string((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
}

// Atomic landing: write <name>.tmp beside the target, then rename over
// it (a reader never sees a torn file; the shader library's next Read
// picks the whole blob up).
bool WriteAtomic(const fs::path& target, const char* data, size_t size,
	std::string& err) {
	const fs::path tmp =
		target.parent_path()
			/ (target.filename().generic_string() + ".reloading");
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) {
			err = "cannot write '" + tmp.generic_string() + "'";
			return false;
		}
		f.write(data, std::streamsize(size));
		if (!f.good()) {
			err = "failed writing '" + tmp.generic_string() + "'";
			return false;
		}
	}
	std::error_code ec;
	fs::rename(tmp, target, ec);
	if (ec) {
		// Cross-device/locked fallback: copy + remove.
		fs::copy_file(tmp, target,
			fs::copy_options::overwrite_existing, ec);
		fs::remove(tmp, ec);
		if (ec) {
			err = "cannot land '" + target.generic_string() + "'";
			return false;
		}
	}
	return true;
}

// One compile landing (the base output or a variant twin): compile ->
// validate -> byte-compare (skip identical) -> atomic write. False +
// diagnostic on failure (the target is NEVER touched then).
bool CompileAndLand(const std::string& sourceText,
	const fs::path& sourcePath, DxcShaderStage stage,
	const std::wstring& entry, const std::vector<std::string>& defines,
	const fs::path& outSpv, Change& out) {
	// The -D arguments (wide form).
	std::vector<std::wstring> wideDefines;
	wideDefines.reserve(defines.size());
	for (const std::string& d : defines)
		wideDefines.emplace_back(d.begin(), d.end());
	std::string diagnostic;
	std::vector<char> spirv;
	try {
		spirv = CompileHLSLToSPIRV(sourceText, stage, entry.c_str(),
			sourcePath.parent_path().wstring(), wideDefines,
			&diagnostic);
	}
	catch (const std::exception& e) {
		out.status = Status::Failed;
		out.diagnostic = e.what();
		return false;
	}
	// VALIDATION: the SPIR-V magic must be sane before the bytes land
	// (a corrupt blob would surface at the library's read gate — this
	// catches it at the source, with the diagnostic).
	if (shaders::DetectBytecode(
			reinterpret_cast<const uint8_t*>(spirv.data()),
			spirv.size()) != shaders::Bytecode::Spirv) {
		out.status = Status::Failed;
		out.diagnostic = "the compiled output failed SPIR-V"
			" validation (truncated or wrong backend)";
		return false;
	}
	// Byte-identical no-op: never move the file (no mtime churn, no
	// downstream swap churn).
	{
		std::ifstream in(outSpv, std::ios::binary);
		if (in) {
			const std::vector<char> existing(
				(std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
			if (existing == spirv) {
				out.status = Status::Recompiled;
				out.bytes = spirv.size();
				return true; // already current — nothing to land
			}
		}
	}
	std::string wErr;
	if (!WriteAtomic(outSpv, spirv.data(), spirv.size(), wErr)) {
		out.status = Status::Failed;
		out.diagnostic = wErr;
		return false;
	}
	out.status = Status::Recompiled;
	out.bytes = spirv.size();
	return true;
}

} // namespace

const std::vector<Variant>& VariantsOf(const std::string& fileName) {
	static const std::vector<Variant> none;
	const auto& table = VariantTable();
	const auto it = table.find(fileName);
	return it == table.end() ? none : it->second;
}

bool Recompile(const std::string& hlslAbsPath,
	const std::string& spvRelName, Change& out) {
	out = Change{};
	const fs::path source = fs::path(hlslAbsPath).lexically_normal();
	out.source = source.generic_string();
	const std::string fileName =
		source.filename().generic_string();
	DxcShaderStage stage = DxcShaderStage::Vertex;
	std::wstring entry;
	if (!StageAndEntry(fileName, stage, entry)) {
		out.status = Status::Failed;
		out.diagnostic = "'" + fileName + "' does not match a shader"
			" stage convention (.vert/.frag/.cs/.mesh/.task .hlsl)";
		return false;
	}
	bool ok = false;
	const std::string text = ReadText(source, ok);
	if (!ok) {
		out.status = Status::Failed;
		out.diagnostic = "cannot read '" + source.generic_string()
			+ "'";
		return false;
	}
	std::string outDir = Svc().outDir;
	if (outDir.empty())
		outDir = source.parent_path().generic_string();
	const fs::path outSpv =
		(fs::path(outDir) / fs::path(spvRelName)).lexically_normal();
	out.spvPath = "content/shaders/" + fs::path(spvRelName)
		.filename().generic_string();
	// The BASE output.
	if (!CompileAndLand(text, source, stage, entry, {}, outSpv, out))
		return false;
	// The VARIANT twins (the baked-define permutations — the mirror
	// of the CMake VARIANTS list; a failure on a twin fails the whole
	// recompile report but the BASE + earlier twins keep their
	// landings). The twin naming is the CMake convention: the FULL
	// source name + suffix + ".spv" (default_lit.frag.hlsl.sg5.spv).
	for (const Variant& v : VariantsOf(fileName)) {
		Change twin{};
		twin.source = out.source;
		twin.spvPath = out.spvPath;
		if (!CompileAndLand(text, source, stage, entry, v.defines,
				(fs::path(outDir)
					/ (fileName + v.suffix + ".spv"))
					.lexically_normal(),
				twin)) {
			out.status = Status::Failed;
			out.diagnostic = "variant " + v.suffix + ": "
				+ twin.diagnostic;
			return false;
		}
	}
	return out.status == Status::Recompiled;
}

bool Start(const std::string& sourceDir, const std::string& outDir,
	std::string& err) {
	err.clear();
	Service& s = Svc();
	std::error_code ec;
	if (!fs::is_directory(fs::path(sourceDir), ec)) {
		err = "shader source dir not found: " + sourceDir;
		return false;
	}
	if (!fs::is_directory(fs::path(outDir), ec)) {
		err = "shader output dir not found: " + outDir;
		return false;
	}
	std::lock_guard<std::mutex> lock(s.mtx);
	if (s.started) {
		// Idempotent re-arm: a moved tree re-registers.
		s.watcher.Stop();
		s.started = false;
	}
	s.sourceDir = fs::path(sourceDir).lexically_normal()
		.generic_string();
	s.outDir = fs::path(outDir).lexically_normal().generic_string();
	FileWatcher::Policy policy = FileWatcher::Policy::ProjectDefault();
	// The SOURCE tree form: watch ONLY .hlsl/.hlsli (the policy's
	// .spv exclusion already holds; nothing else in the tree is a
	// source).
	if (!s.watcher.Start(policy, err))
		return false;
	if (!s.watcher.AddDirectory(s.sourceDir, err)) {
		s.watcher.Stop();
		return false;
	}
	s.started = true;
	std::fprintf(stderr, "[shader-reload] watching '%s' -> '%s'\n",
		s.sourceDir.c_str(), s.outDir.c_str());
	return true;
}

void Stop() {
	Service& s = Svc();
	std::lock_guard<std::mutex> lock(s.mtx);
	if (!s.started)
		return;
	s.watcher.Stop();
	s.started = false;
}

bool Running() {
	Service& s = Svc();
	std::lock_guard<std::mutex> lock(s.mtx);
	return s.started;
}

const std::string& SourceDir() {
	return Svc().sourceDir;
}

const std::string& OutDir() {
	return Svc().outDir;
}

std::vector<Change> ConsumeChanges(int stabilizeMs) {
	std::vector<Change> log;
	Service& s = Svc();
	std::lock_guard<std::mutex> lock(s.mtx);
	if (!s.started)
		return log;
	const std::vector<FileWatcher::Change> events =
		s.watcher.Poll(stabilizeMs);
	if (events.empty())
		return log;
	// The invalidation rules: a .hlsli edit recompiles EVERY .hlsl
	// (includes are coarse); a .hlsl edit recompiles that file.
	bool allSources = false;
	std::vector<std::string> dirty;
	for (const FileWatcher::Change& e : events) {
		const std::string name =
			fs::path(e.path).filename().generic_string();
		const std::string ext =
			fs::path(e.path).extension().generic_string();
		if (e.event == FileWatcher::Event::Removed)
			continue; // a deleted source keeps its last .spv (the
			          // last-known-good's degenerate form)
		if (ext == ".hlsli" || ext == ".hlsl.h")
			allSources = true;
		else if (ext == ".hlsl")
			dirty.push_back(e.path);
	}
	std::sort(dirty.begin(), dirty.end());
	dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
	if (allSources) {
		dirty.clear();
		std::error_code ec;
		for (const fs::directory_entry& e :
				fs::directory_iterator(fs::path(s.sourceDir), ec)) {
			if (!e.is_regular_file(ec))
				continue;
			if (e.path().extension().generic_string() == ".hlsl")
				dirty.push_back(e.path().lexically_normal()
					.generic_string());
		}
		std::sort(dirty.begin(), dirty.end());
	}
	for (const std::string& src : dirty) {
		const std::string fileName =
			fs::path(src).filename().generic_string();
		// The output convention: the FULL source name + ".spv"
		// (sky.frag.hlsl -> sky.frag.hlsl.spv).
		const std::string spvName = fileName + ".spv";
		Change c;
		// NOTE: recompiled under the LOCK — a heavy edit stalls this
		// frame; dev-only by design (the usage doc).
		if (Recompile(src, spvName, c))
			log.push_back(std::move(c));
		else
			log.push_back(std::move(c)); // Failed rides the log too
	}
	return log;
}

} // namespace smvk::shader_reload
