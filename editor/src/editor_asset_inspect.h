// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Asset-browser inspector routing and the bounded, ImGui-free file preview.
// Typed assets keep their own inspectors; every other selected file can
// still show metadata and, when safe, a read-only UTF-8 text preview.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace editor {

enum class BrowserInspectorKind {
	Material, Geometry, Texture, GiCook, LmCook, Cubemap, Givolume,
	TextureSource, SpriteSet, Generic
};

inline BrowserInspectorKind ClassifyBrowserInspector(
	const std::filesystem::path& path) {
	std::string ext = path.extension().string();
	for (char& c : ext)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	if (ext == ".spriteset") return BrowserInspectorKind::SpriteSet;
	if (ext == ".material") return BrowserInspectorKind::Material;
	if (ext == ".geometry" || ext == ".mesh")
		return BrowserInspectorKind::Geometry; // legacy .mesh files
	if (ext == ".texture") return BrowserInspectorKind::Texture;
	if (ext == ".gicook") return BrowserInspectorKind::GiCook;
	if (ext == ".lmcook") return BrowserInspectorKind::LmCook;
	if (ext == ".cubemap") return BrowserInspectorKind::Cubemap;
	if (ext == ".givolume") return BrowserInspectorKind::Givolume;
	if (ext == ".png" || ext == ".jpg" || ext == ".jpeg"
		|| ext == ".tga" || ext == ".bmp" || ext == ".ktx2")
		return BrowserInspectorKind::TextureSource;
	return BrowserInspectorKind::Generic;
}

struct BrowserFilePreview {
	bool isDirectory = false;
	bool readOk = false;
	bool isText = false;
	bool truncated = false;
	std::uintmax_t fileSize = 0;
	std::string text;
	std::string error;
};

// Returns the end of a valid UTF-8 text prefix. Invalid bytes and
// binary controls reject the preview; only an incomplete code point at
// the limit may be trimmed. ImGui expects valid UTF-8 and no NUL bytes.
inline size_t BrowserTextPrefix(std::string_view bytes, bool truncated) {
	for (size_t i = 0; i < bytes.size();) {
		const auto c = static_cast<unsigned char>(bytes[i]);
		if (c < 0x80) {
			if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
				return std::string::npos;
			if (c == 0x7f)
				return std::string::npos;
			++i;
			continue;
		}
		size_t width = 0;
		if (c >= 0xc2 && c <= 0xdf) width = 2;
		else if (c >= 0xe0 && c <= 0xef) width = 3;
		else if (c >= 0xf0 && c <= 0xf4) width = 4;
		else return std::string::npos;
		if (width > bytes.size() - i)
			return truncated ? i : std::string::npos;
		for (size_t j = 1; j < width; ++j) {
			const auto b = static_cast<unsigned char>(bytes[i + j]);
			if ((b & 0xc0) != 0x80)
				return std::string::npos;
		}
		const auto next = static_cast<unsigned char>(bytes[i + 1]);
		if ((c == 0xe0 && next < 0xa0)
			|| (c == 0xed && next >= 0xa0)
			|| (c == 0xf0 && next < 0x90)
			|| (c == 0xf4 && next >= 0x90))
			return std::string::npos; // overlong, surrogate, out of range
		i += width;
	}
	return bytes.size();
}

inline BrowserFilePreview ReadBrowserFilePreview(
	const std::filesystem::path& path, size_t maxBytes = 64 * 1024) {
	namespace fs = std::filesystem;
	BrowserFilePreview result;
	std::error_code ec;
	if (fs::is_directory(path, ec) && !ec) {
		result.isDirectory = true;
		result.readOk = true;
		return result;
	}
	if (ec || !fs::is_regular_file(path, ec) || ec) {
		result.error = "File is missing or is not a regular file.";
		return result;
	}
	result.fileSize = fs::file_size(path, ec);
	if (ec) {
		result.error = "Unable to read file size.";
		return result;
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		result.error = "Unable to open file.";
		return result;
	}
	const size_t count = static_cast<size_t>(std::min<std::uintmax_t>(
		result.fileSize, maxBytes));
	std::string bytes(count, '\0');
	if (count) {
		in.read(bytes.data(), static_cast<std::streamsize>(count));
		bytes.resize(static_cast<size_t>(in.gcount()));
		if (bytes.empty()) {
			result.error = "Unable to read file contents.";
			return result;
		}
	}
	result.readOk = true;
	result.truncated = result.fileSize > bytes.size();
	const size_t prefix = BrowserTextPrefix(bytes, result.truncated);
	if (prefix == std::string::npos)
		return result; // binary / invalid encoding: metadata only
	result.isText = true;
	// A UTF-8 BOM is not part of the displayed text.
	const size_t start = prefix >= 3
		&& bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
	result.text = bytes.substr(start, prefix - start);
	return result;
}

} // namespace editor
