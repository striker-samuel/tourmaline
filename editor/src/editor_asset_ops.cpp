// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Asset-browser delete planning/execution (see editor_asset_ops.h).

#include "editor_asset_ops.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include <gltf_import/asset_export.h> // manifest-derived asset detection

namespace editor {

namespace fs = std::filesystem;

namespace {

// Normalized absolute generic path (no existence requirement).
std::string Normalized(const std::string& p) {
	std::error_code ec;
	fs::path abs = fs::absolute(fs::path(p), ec);
	if (ec)
		abs = fs::path(p);
	return abs.lexically_normal().generic_string();
}

bool HasSuffixLower(const std::string& name, const char* suffix) {
	const size_t n = std::string(suffix).size();
	if (name.size() < n)
		return false;
	std::string tail = name.substr(name.size() - n);
	for (char& c : tail)
		c = char(std::tolower((unsigned char)c));
	return tail == suffix;
}

// True when `abs` is strictly inside `root` (both normalized generic;
// root itself is NOT inside).
bool StrictlyInside(const std::string& abs, const std::string& root) {
	std::string r = root;
	if (!r.empty() && r.back() != '/')
		r += '/';
	return abs.size() > r.size() && abs.compare(0, r.size(), r) == 0;
}

std::string RelOf(const std::string& abs, const std::string& root) {
	if (StrictlyInside(abs, root))
		return abs.substr(root.size() + (root.back() == '/' ? 0 : 1));
	return abs;
}

// The sibling <stem>.gltf whose manifest lists `fileAbs` as a derived
// asset ("" when the file is standalone). Derived files carry no
// per-file sidecar — identity is manifest-driven.
std::string OwningGltf(const std::string& fileAbs) {
	const fs::path file(fileAbs);
	const fs::path dir = file.parent_path();
	const std::string name = file.filename().generic_string();
	std::error_code ec;
	for (fs::directory_iterator it(dir, ec), end;
		!ec && it != end; it.increment(ec)) {
		if (ec)
			break;
		std::error_code fec;
		if (!it->is_regular_file(fec) || fec)
			continue;
		const std::string n = it->path().filename().string();
		if (n.size() <= 10
			|| n.compare(n.size() - 10, 10, ".gltf.meta") != 0)
			continue;
		const std::string gltf = it->path().generic_string().substr(
			0, it->path().generic_string().size() - 5); // strip ".meta"
		smvk::assets::AssetManifest man;
		std::string merr;
		if (!smvk::assets::LoadAssetManifest(gltf, man, merr))
			continue;
		for (const auto& e : man.entries) {
			if (e.file == name)
				return gltf;
		}
	}
	return {};
}

} // namespace

bool PlanAssetDelete(const std::string& absPath,
	const std::string& projectRoot, AssetDeletePlan& out,
	std::string& err) {
	out = AssetDeletePlan{};
	const std::string root = Normalized(projectRoot);
	const std::string abs = Normalized(absPath);
	if (root.empty() || abs.empty()) {
		err = "no project open";
		return false;
	}
	std::error_code ec;
	const bool isDir = fs::is_directory(fs::path(abs), ec) && !ec;
	const bool isFile = !isDir && fs::is_regular_file(fs::path(abs), ec)
		&& !ec;
	if (!isDir && !isFile) {
		err = "not found: " + RelOf(abs, root);
		return false;
	}
	// Protected structure (never content, never user-deletable here).
	if (abs == root) {
		err = "refusing to delete the project root";
		return false;
	}
	const std::string rel = RelOf(abs, root);
	if (rel == "project.tourproj") {
		err = "refusing to delete the project marker";
		return false;
	}
	if (rel == "artifacts" || rel.compare(0, 10, "artifacts/") == 0
		|| rel == ".tourmaline" || rel.compare(0, 12, ".tourmaline/") == 0
		|| rel == "scratch" || rel.compare(0, 8, "scratch/") == 0) {
		err = "refusing to delete the owned artifact cache";
		return false;
	}
	if (!StrictlyInside(abs, root)) {
		err = "outside the project: " + absPath;
		return false;
	}
	out.target = abs;
	out.isDir = isDir;
	if (isDir) {
		// Recursive count for the confirm prompt (best effort — the
		// delete itself reports failures).
		size_t n = 0;
		for (fs::recursive_directory_iterator it(abs, ec), end;
			!ec && it != end; it.increment(ec)) {
			if (ec)
				break;
			++n;
		}
		out.entryCount = n;
		out.notes.push_back("folder with " + std::to_string(n)
			+ " entr" + (n == 1 ? "y" : "ies") + " (recursive)");
		return true;
	}
	out.entryCount = 1;
	// Derived assets refuse standalone deletion (the manifest would
	// desync — delete the source .gltf, which takes the set with it).
	// The REGENERABLE BAKE CLASSES are carved out: .gicook/.lmcook are
	// bake CONSEQUENCES (the next bake regenerates them — stale-bake
	// litter cleanup is a legitimate delete; the manifest-desync
	// argument only holds for .geometry/.material/.anim).
	const std::string nameForBake = fs::path(abs).filename()
		.generic_string();
	const bool isBakeAsset = HasSuffixLower(nameForBake, ".gicook")
		|| HasSuffixLower(nameForBake, ".lmcook");
	if (!isBakeAsset) {
		if (const std::string owner = OwningGltf(abs); !owner.empty()) {
			err = "derived asset of '" + RelOf(Normalized(owner), root)
				+ "' — delete the source instead";
			return false;
		}
	}
	// The identity sidecar rides along (no orphan GUIDs; the assetdb row
	// stays as a missing retarget for reference diagnostics).
	if (fs::is_regular_file(fs::path(abs + ".meta"), ec) && !ec)
		out.also.push_back(abs + ".meta");
	// (the .gicook/.lmcook bake classes passed the derived check above)
	const std::string name = fs::path(abs).filename().generic_string();
	if (HasSuffixLower(name, ".gltf") || HasSuffixLower(name, ".glb")) {
		// A manifested source takes its derived set with it.
		smvk::assets::AssetManifest man;
		std::string merr;
		if (smvk::assets::LoadAssetManifest(abs, man, merr)) {
			size_t derived = 0;
			for (const auto& e : man.entries) {
				const std::string d =
					(fs::path(abs).parent_path() / e.file)
						.lexically_normal().generic_string();
				std::error_code dec;
				if (fs::is_regular_file(fs::path(d), dec) && !dec) {
					out.also.push_back(d);
					++derived;
				}
			}
			if (derived > 0) {
				out.notes.push_back("also deletes "
					+ std::to_string(derived) + " derived asset(s)");
			}
		}
	}
	if (HasSuffixLower(name, ".gicook") || HasSuffixLower(name, ".lmcook")
		|| HasSuffixLower(name, ".cubemap")
		|| HasSuffixLower(name, ".givolume"))
		out.notes.push_back("the baked blob payload stays (shared "
			"bake output — delete it from GI bakes if really wanted)");
	return true;
}

bool ExecuteAssetDelete(const AssetDeletePlan& plan, std::string& err) {
	if (plan.target.empty()) {
		err = "nothing planned";
		return false;
	}
	// Derived/sidecar files first (a mid-list failure still leaves the
	// target for retry — the plan recomputes cleanly).
	for (const std::string& p : plan.also) {
		std::error_code ec;
		fs::remove_all(fs::path(p), ec);
		if (ec) {
			err = "cannot delete '" + p + "': " + ec.message();
			return false;
		}
	}
	{
		std::error_code ec;
		fs::remove_all(fs::path(plan.target), ec);
		if (ec) {
			err = "cannot delete '" + plan.target + "': "
				+ ec.message();
			return false;
		}
	}
	return true;
}

} // namespace editor
