// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "editor_asset_dcc.h"
#include "editor_asset_ops.h"

#include <gltf_import/asset_export.h>
#include <gltf_import/source_formats.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdlib>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
extern char** environ;
#endif

namespace editor {
namespace {
namespace fs = std::filesystem;

std::string LowerExt(const fs::path& p) {
	std::string ext = p.extension().generic_string();
	for (char& c : ext)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return ext;
}

bool Regular(const fs::path& p) {
	std::error_code ec;
	return fs::is_regular_file(p, ec) && !ec;
}

std::string Absolute(const fs::path& p) {
	std::error_code ec;
	auto abs = fs::absolute(p, ec);
	return (ec ? p : abs).lexically_normal().generic_string();
}

fs::path Sidecar(const std::string& published) {
	return fs::path(published + ".dccsource");
}

bool ValidDocument(const fs::path& p) {
	// A hand-edited .dccsource must not launch scripts or executables
	// merely because the OS has a file association. Extend this
	// explicit set for additional DCC applications.
	const std::string e = LowerExt(p);
	return e == ".ma" || e == ".mb" || e == ".blend"
		|| e == ".obj" || e == ".gltf" || e == ".glb"
		|| e == ".fbx" || e == ".usd" || e == ".usda"
		|| e == ".usdc" || e == ".hip" || e == ".hiplc"
		|| e == ".hipnc" || e == ".max" || e == ".c4d"
		|| e == ".dae" || e == ".abc";
}
} // namespace

bool IsDccPublishable(const std::string& published) {
	const fs::path p(published);
	if (smvk::assets::IsMeshSourceFile(p))
		return true;
	const std::string e = LowerExt(p);
	return e == ".png" || e == ".jpg" || e == ".jpeg"
		|| e == ".tga" || e == ".bmp" || e == ".ktx2"
		|| e == ".exr" || e == ".hdr";
}

bool ResolveDccPublishedAsset(const std::string& selected,
	std::string& published, std::string& err) {
	published.clear();
	err.clear();
	if (selected.empty() || !Regular(selected)) {
		err = "selected asset is not a readable file";
		return false;
	}
	fs::path p = fs::path(Absolute(selected));
	const std::string ext = LowerExt(p);
	if (ext == ".geometry" || ext == ".mesh"
			|| ext == ".material" || ext == ".anim") {
		const std::string owner = FindOwningMeshSource(p.generic_string());
		if (owner.empty()) {
			err = "derived asset has no source manifest: " + selected;
			return false;
		}
		p = fs::path(owner);
	}
	else if (ext == ".texture") {
		smvk::assets::TextureAsset tex;
		std::string textureError;
		if (smvk::assets::LoadTextureAsset(p, tex, textureError)
				&& !tex.source.empty()) {
			p = (p.parent_path() / tex.source).lexically_normal();
		}
		else {
			err = "texture source unavailable: " + textureError;
			return false;
		}
	}
	if (!Regular(p)) {
		err = "published source is missing: " + p.generic_string();
		return false;
	}
	published = Absolute(p);
	return true;
}

bool ResolveDccAsset(const std::string& selected,
	DccAsset& asset, std::string& err) {
	asset = {};
	// First publication: a native Maya/Blender scene in the project
	// can create <stem>.gltf without needing a placeholder file.
	// The runner publishes that glTF, then an authoring link is saved.
	const std::string nativeExt = LowerExt(fs::path(selected));
	if ((nativeExt == ".ma" || nativeExt == ".mb"
			|| nativeExt == ".blend") && Regular(fs::path(selected))) {
		asset.document = Absolute(selected);
		fs::path gltf = fs::path(asset.document);
		gltf.replace_extension(".gltf");
		asset.published = gltf.generic_string();
		asset.linked = true;
		asset.fromNative = true;
		err.clear();
		return true;
	}
	if (!ResolveDccPublishedAsset(selected, asset.published, err))
		return false;
	asset.document = asset.published;
	const fs::path sidecar = Sidecar(asset.published);
	std::error_code ec;
	if (!fs::exists(sidecar, ec))
		return true;
	if (ec || !Regular(sidecar) || fs::file_size(sidecar, ec) > 16 * 1024 || ec) {
		err = "invalid DCC source link: " + sidecar.generic_string();
		return false;
	}
	std::ifstream in(sidecar, std::ios::binary);
	std::string header, line;
	if (!std::getline(in, header) || header != "version=1"
			|| !std::getline(in, line) || line.rfind("source=", 0) != 0
			|| line.size() <= 7) {
		err = "malformed DCC source link: " + sidecar.generic_string();
		return false;
	}
	if (!line.empty() && line.back() == '\r')
		line.pop_back();
	fs::path target(line.substr(7));
	if (target.is_relative())
		target = sidecar.parent_path() / target;
	if (!Regular(target) || !ValidDocument(target)) {
		err = "linked DCC document missing or not openable: "
			+ target.generic_string();
		return false;
	}
	asset.document = Absolute(target);
	asset.linked = true;
	return true;
}

bool LinkDccSource(const std::string& selected,
	const std::string& documentPath, std::string& err) {
	std::string published;
	if (!ResolveDccPublishedAsset(selected, published, err))
		return false;
	if (!IsDccPublishable(published)) {
		err = "only published mesh/image sources can carry a DCC link";
		return false;
	}
	const fs::path document = fs::path(Absolute(documentPath));
	if (!Regular(document) || !ValidDocument(document)) {
		err = "DCC document must be an existing non-executable file";
		return false;
	}
	fs::path saved = document;
	std::error_code ec;
	const fs::path relative = fs::relative(document,
		fs::path(published).parent_path(), ec);
	if (!ec && !relative.empty())
		saved = relative;
	// Relative paths preserve portability when the source and output
	// move together. This file is editor metadata, not an importer input.
	const fs::path sidecar = Sidecar(published);
	const std::string name = saved.generic_string();
	if (name.find('\n') != std::string::npos
			|| name.find('\r') != std::string::npos) {
		err = "DCC document path cannot contain a newline";
		return false;
	}
	std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write DCC link: " + sidecar.generic_string();
		return false;
	}
	out << "version=1\nsource=" << name << "\n";
	out.close();
	if (!out) {
		err = "failed writing DCC link: " + sidecar.generic_string();
		return false;
	}
	err.clear();
	return true;
}

bool UnlinkDccSource(const std::string& selected, std::string& err) {
	std::string published;
	if (!ResolveDccPublishedAsset(selected, published, err))
		return false;
	std::error_code ec;
	(void)fs::remove(Sidecar(published), ec);
	if (ec) {
		err = ec.message();
		return false;
	}
	err.clear();
	return true;
}


namespace {
#ifdef _WIN32
std::wstring Wide(const std::string& utf8) {
	const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		utf8.c_str(), -1, nullptr, 0);
	if (n < 1)
		return {};
	std::wstring w(size_t(n), L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			utf8.c_str(), -1, w.data(), n))
		return {};
	w.pop_back();
	return w;
}

std::wstring QuoteWindowsArg(const std::wstring& arg) {
	std::wstring out = L"\"";
	size_t slashes = 0;
	for (wchar_t c : arg) {
		if (c == L'\\') { ++slashes; continue; }
		if (c == L'"') {
			out.append(slashes * 2 + 1, L'\\');
			out += L'"';
		}
		else {
			out.append(slashes, L'\\');
			out += c;
		}
		slashes = 0;
	}
	out.append(slashes * 2, L'\\');
	out += L'"';
	return out;
}
#endif

bool RunProcess(const std::vector<std::string>& argv, std::string& err) {
#ifdef _WIN32
	std::wstring cmd;
	for (const auto& arg : argv) {
		const auto wide = Wide(arg);
		if (wide.empty()) { err = "invalid UTF-8 command argument"; return false; }
		if (!cmd.empty()) cmd += L" ";
		cmd += QuoteWindowsArg(wide);
	}
	std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
	buffer.push_back(L'\0');
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		err = "cannot start DCC runner (Win32 error "
			+ std::to_string(GetLastError()) + ")";
		return false;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exitCode = 1;
	(void)GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (exitCode != 0) {
		err = "DCC build process exited with code " + std::to_string(exitCode);
		return false;
	}
#else
	std::vector<char*> raw;
	raw.reserve(argv.size() + 1);
	for (const auto& arg : argv)
		raw.push_back(const_cast<char*>(arg.c_str()));
	raw.push_back(nullptr);
	pid_t pid = 0;
	const int rc = posix_spawnp(&pid, raw.front(),
		nullptr, nullptr, raw.data(), environ);
	if (rc) {
		err = std::string("cannot spawn DCC runner: ") + std::strerror(rc);
		return false;
	}
	int status = 0;
	pid_t finished;
	do {
		finished = waitpid(pid, &status, 0);
	} while (finished == -1 && errno == EINTR);
	if (finished != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		err = "DCC build process failed (exit code "
			+ std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")";
		return false;
	}
#endif
	err.clear();
	return true;
}
} // namespace

bool RunDccPublish(const DccAsset& asset, const std::string& publishScript,
	std::string& err) {
	if (!asset.linked) {
		err = "asset has no DCC authoring source link";
		return false;
	}
	if (LowerExt(fs::path(asset.published)) != ".gltf") {
		err = "the basic DCC build runner only publishes .gltf outputs";
		return false;
	}
	std::string runner;
	const std::string ext = LowerExt(fs::path(asset.document));
	if (ext == ".ma" || ext == ".mb")
		runner = "maya";
	else if (ext == ".blend")
		runner = "blender";
	else {
		err = "no glTF build runner for " + ext
			+ " (only Maya and Blender are configured)";
		return false;
	}
	if (!Regular(fs::path(asset.document))
			|| !Regular(fs::path(publishScript))) {
		err = "DCC authoring document or staged export runner is missing";
		return false;
	}
	// Native-file first publish must not silently overwrite an
	// unrelated or manually imported glTF that happens to share a stem.
	if (asset.fromNative && Regular(fs::path(asset.published))) {
		DccAsset existing;
		std::string linkError;
		if (!ResolveDccAsset(asset.published, existing, linkError)
				|| !existing.linked
				|| existing.document != asset.document) {
			err = "published glTF already exists and is not linked to "
				"this native scene; select the glTF and explicitly "
				"link its DCC source before publishing";
			return false;
		}
	}
	const char* configured = std::getenv("TOURMALINE_PYTHON");
	const std::string python = (configured && *configured)
		? configured : (
#ifdef _WIN32
			"python"
#else
			"python3"
#endif
		);
	return RunProcess({ python, publishScript,
		"--runner", runner, "--source", asset.document,
		"--output", asset.published }, err);
}

} // namespace editor
