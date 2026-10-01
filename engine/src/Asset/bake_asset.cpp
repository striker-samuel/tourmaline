// Bake-asset identity implementation (see Engine/Asset/bake_asset.h).

#include <Engine/Asset/bake_asset.h>

#include <Engine/Asset/tour.h> // GenerateUuid

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace smvk::bake {

namespace {

bool IsHexLower(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

} // namespace

bool IsBakeAssetGuid(std::string_view s) {
	if (s.size() != 32)
		return false;
	for (char c : s)
		if (!IsHexLower(c))
			return false;
	return true;
}

bool ReadBakeAssetSidecar(const std::string& filePath,
	BakeAssetSidecar& out, std::string& err) {
	out = BakeAssetSidecar{};
	std::error_code ec;
	const fs::path meta = fs::path(filePath + ".meta");
	if (!fs::is_regular_file(meta, ec))
		return false;
	std::ifstream in(meta, std::ios::binary);
	if (!in)
		return false;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = line.substr(0, eq);
		const std::string val = line.substr(eq + 1);
		if (key == "guid")
			out.guid = val;
		else if (key == "kind")
			out.kind = val;
		else if (key == "logical")
			out.logical = val;
	}
	if (!IsBakeAssetGuid(out.guid)) {
		err = "invalid guid '" + out.guid + "' in "
			+ meta.generic_string();
		out = BakeAssetSidecar{};
		return false;
	}
	return true;
}

bool WriteBakeAssetSidecar(const std::string& filePath,
	std::string_view guid, std::string_view kind,
	std::string_view logical, std::string& err) {
	if (!IsBakeAssetGuid(guid)) {
		err = "refusing to write invalid guid";
		return false;
	}
	std::error_code ec;
	fs::create_directories(fs::path(filePath).parent_path(), ec);	std::ofstream out(fs::path(std::string(filePath) + ".meta"),
		std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write " + filePath + ".meta";
		return false;
	}
	out << "# Tourmaline bake-asset meta (GUID identifies this bake "
		"output — keep the sidecar with it on move/rename; rebakes "
		"reuse the GUID)\n";
	out << "guid=" << guid << "\n";
	if (!kind.empty())
		out << "kind=" << kind << "\n";
	if (!logical.empty())
		out << "logical=" << logical << "\n";
	out << "importer=bake\n";
	out << "version=1\n";
	return !!out;
}

std::string FindBakeAssetGuid(const std::string& dir,
	std::string_view kind, std::string_view logical) {
	if (dir.empty() || kind.empty() || logical.empty())
		return {};
	std::error_code ec;
	for (fs::recursive_directory_iterator it(dir, ec), end;
		!ec && it != end; it.increment(ec)) {
		if (ec)
			break;
		const fs::path& p = it->path();
		std::error_code fec;
		if (!it->is_regular_file(fec) || fec)
			continue;
		if (p.extension() != ".meta")
			continue;
		// The sidecar's payload path: its own path minus the .meta
		// suffix (a plain stem() would drop a second extension, e.g.
		// "x.sglm" -> "x").
		const std::string payload =
			p.generic_string().substr(0,
				p.generic_string().size()
				- std::string(".meta").size());
		BakeAssetSidecar side;
		std::string serr;
		if (!ReadBakeAssetSidecar(payload, side, serr))
			continue;
		if (side.kind == kind && side.logical == logical)
			return side.guid;
	}
	return {};
}

std::string EnsureBakeAssetGuid(const std::string& dir,
	std::string_view kind, std::string_view logical, std::string& err) {
	// Rebake: reuse the guid whose sidecar matches (kind, logical)
	// anywhere under dir. The match is by sidecar CONTENT, not by
	// filename — renames heal as long as the file (sidecar included)
	// stayed somewhere under dir.
	if (!dir.empty() && !kind.empty() && !logical.empty()) {
		const std::string found = FindBakeAssetGuid(dir,
			std::string(kind), std::string(logical));
		if (!found.empty())
			return found;
	}
	// First bake (or nothing matched): mint fresh. The caller writes the
	// payload to its guid-derived path + refreshes the sidecar there.
	std::string guid = smvk::tour::GenerateUuid();
	if (!IsBakeAssetGuid(guid)) {
		err = "uuid generator produced an invalid guid";
		return {};
	}
	return guid;
}

std::string PayloadBakeGuid(const std::string& filePath) {
	BakeAssetSidecar side;
	std::string err;
	if (!ReadBakeAssetSidecar(filePath, side, err))
		return {};
	return side.guid;
}

} // namespace smvk::bake
