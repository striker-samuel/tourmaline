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
	// Opening a document through its OS association must never become
	// a shell/script/executable launch from a hand-edited sidecar.
	const std::string e = LowerExt(p);
	return e != ".exe" && e != ".com" && e != ".bat"
		&& e != ".cmd" && e != ".ps1" && e != ".sh"
		&& e != ".msi" && e != ".app" && e != ".desktop";
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
	std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write DCC link: " + sidecar.generic_string();
		return false;
	}
	const std::string name = saved.generic_string();
	if (name.find('\n') != std::string::npos
			|| name.find('\r') != std::string::npos) {
		err = "DCC document path cannot contain a newline";
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

} // namespace editor
