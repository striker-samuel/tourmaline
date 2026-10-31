// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The deployment validation gate (see DeployCheck.h). Runs BEFORE
// any graphics/window work: the manifest's paks prove themselves
// (existence + a valid image + the content-addressed NAME matching
// the actual bytes) and an engine shader source exists. A broken
// deployment fails loud + actionable; it never reaches app init.

#include "DeployCheck.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <artifact_cache/artifact_cache.h> // HashBytes (the name digests)

#include <Engine/Asset/pak.h>
#include <Engine/Utils/shader_library.h>

namespace app {

namespace {

namespace fs = std::filesystem;

// <stem>-<16 lowercase hex>.pak — the content-addressed naming the
// recipe build's publish pass writes (build_pipeline.cpp). Returns
// the digest token or "" for a plain (legacy) name.
std::string DigestInName(const std::string& fileName) {
	const std::string ext = ".pak";
	if (fileName.size() <= ext.size()
			|| fileName.compare(fileName.size() - ext.size(),
				ext.size(), ext) != 0)
		return {};
	const size_t dash = fileName.rfind('-');
	if (dash == std::string::npos
			|| fileName.size() - dash - 1 - ext.size() != 16)
		return {};
	const std::string digest =
		fileName.substr(dash + 1, fileName.size() - dash - 1
			- ext.size());
	for (char c : digest)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return {}; // not a digest shape (a plain name with dashes)
	return digest;
}

std::vector<uint8_t> ReadAllBytes(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return {};
	const std::string bytes((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

} // namespace

bool ValidatePakFile(const std::string& pakPath, std::string& err) {
	err.clear();
	std::error_code ec;
	if (!fs::is_regular_file(fs::path(pakPath), ec)) {
		err = "pak missing from the deployment: '" + pakPath
			+ "' — the manifest lists it; recook/redeploy the project"
				" or fix the manifest";
		return false;
	}
	const std::vector<uint8_t> image = ReadAllBytes(pakPath);
	if (image.empty()) {
		err = "pak unreadable or empty: '" + pakPath + "'";
		return false;
	}
	// The image proof (magic/sizes/CRC — the same gate the runtime's
	// PakBackend::Open runs; a torn or corrupt file fails HERE).
	{
		std::string openErr;
		if (!smvk::PakReader::Open(
				std::span<const uint8_t>(image), openErr)) {
			err = "pak failed its integrity check: '" + pakPath
				+ "' (" + openErr + ") — the file is corrupt or"
					" truncated; redeploy it";
			return false;
		}
	}
	// The DEPLOYMENT CONTRACT: a content-addressed name carries the
	// fnv1a64 digest of the exact bytes the publish pass wrote — the
	// file under that name must still hash to it (a stale/wrong pak
	// parked under the expected name fails even though its own image
	// is internally valid).
	const std::string name =
		fs::path(pakPath).filename().generic_string();
	const std::string digest = DigestInName(name);
	if (!digest.empty()) {
		const std::string actual =
			smvk::cache::HashBytes(image.data(), image.size());
		if (actual != digest) {
			err = "content-address mismatch: '" + name
				+ "' names digest " + digest + " but the file hashes to "
				+ actual + " — a stale or wrong pak sits under the"
					" expected name; redeploy the correct generation";
			return false;
		}
	}
	return true;
}

bool ValidateEngineShaderSource(std::string& err) {
	err.clear();
	smvk::shaders::Init();
	if (smvk::shaders::HasAnySource())
		return true;
	err = "no engine shader source: neither an engine-shaders.pak"
		" package nor a content/shaders/ dev tree exists next to the"
		" executable — the engine shader package is a MANDATORY"
		" runtime dependency (deploy it with the build's host setup)";
	return false;
}

} // namespace app
