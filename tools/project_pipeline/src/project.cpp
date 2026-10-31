// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Project open/init + path accessors (see project.h). Extends the
// editor's existing project.tourproj marker model (editor_project.cpp
// keeps its own copy of the marker parse — same format, both sides
// agree; the editor is the legacy consumer, this is the pipeline one).

#include <project_pipeline/project.h>

#include <artifact_cache/assetdb.h>

#include <Engine/Asset/tour.h>

#include <cstdio>
#include <fstream>

namespace smvk::project {

namespace {

std::string Normalized(const std::string& p) {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path abs = fs::absolute(fs::path(p), ec);
	if (ec)
		abs = fs::path(p);
	return abs.lexically_normal().generic_string();
}

// FNV-1a 64 (the repo's stable hash shape — same constants the
// catalog generator folds with).
uint64_t Fnv(const char* s) {
	uint64_t h = 1469598103934665603ULL;
	while (*s) {
		h ^= uint64_t(uint8_t(*s++));
		h *= 1099511628211ULL;
	}
	return h;
}

// The marker rows (name=value lines; "#"-prefixed comments).
struct Marker {
	std::string name;
	std::string guid;
	bool parsed = false;
};

Marker ReadMarker(const std::filesystem::path& dir) {
	Marker out;
	std::ifstream in(dir / kProjectMarker, std::ios::binary);
	if (!in)
		return out;
	out.parsed = true;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		if (line.substr(0, eq) == "name")
			out.name = line.substr(eq + 1);
		else if (line.substr(0, eq) == "guid")
			out.guid = line.substr(eq + 1);
	}
	return out;
}

// Write the marker ATOMICALLY-ENOUGH for identity (write fresh text,
// preserve the existing name row on the upgrade path). `guid` must
// already be validated by the caller.
bool WriteMarker(const std::filesystem::path& markerPath,
	const std::string& name, const std::string& guid,
	std::string& err) {
	std::ofstream out(markerPath, std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write '" + markerPath.generic_string() + "'";
		return false;
	}
	out << "# Tourmaline project marker\n"
		<< "name=" << name << "\n"
		<< "guid=" << guid << "\n"
		<< "version=" << kProjectMarkerVersion << "\n";
	if (!out.good()) {
		err = "failed writing '" + markerPath.generic_string() + "'";
		return false;
	}
	return true;
}

} // namespace

uint64_t DeriveProjectId(const std::string& guid) {
	// The 0xA1000000 family the generated project.h has always carried
	// (the high marker byte), now keyed on the PERSISTENT identity —
	// a rename or a root move can never move the id.
	return 0xA1000000ULL | (Fnv(guid.c_str()) & 0xFFFF);
}

Project Project::Open(const std::string& dir, std::string& err) {
	Project p;
	if (dir.empty()) {
		err = "project directory is empty";
		return p;
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path abs = fs::absolute(fs::path(dir), ec);
	if (ec || !fs::is_directory(abs, ec)) {
		err = "not a directory: " + Normalized(dir);
		return p;
	}
	if (!fs::is_regular_file(abs / kProjectMarker, ec)) {
		err = "no " + std::string(kProjectMarker) + " marker in "
			+ abs.lexically_normal().generic_string()
			+ " (create one with Project.exe init)";
		return p;
	}
	const Marker m = ReadMarker(abs);
	// The identity contract (current-format policy — no v1 reader, no
	// name-derived fallback): a marker without a VALID guid row is
	// incompatible. The error is actionable: init adopts the project
	// and mints the identity exactly once.
	if (m.guid.empty() || !smvk::cache::IsValidAssetGuid(m.guid)) {
		err = std::string(kProjectMarker) + " in "
			+ abs.lexically_normal().generic_string()
			+ " has no valid project identity (guid= row missing or"
				" malformed) — the current project format requires"
				" one. Run 'Project.exe init <dir>' to adopt the"
				" project and mint its identity, or recreate the"
				" project";
		return p;
	}
	p.root = abs.lexically_normal().generic_string();
	p.name = m.name.empty()
		? abs.filename().generic_string() : m.name;
	p.guid = m.guid;
	return p;
}

Project Project::Init(const std::string& dir, const std::string& name,
	bool& created, std::string& err) {
	created = false;
	Project p;
	if (dir.empty()) {
		err = "project directory is empty";
		return p;
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path abs = fs::absolute(fs::path(dir), ec);
	if (ec) {
		err = "cannot resolve: " + Normalized(dir);
		return p;
	}
	// The conventional tree (idempotent — existing dirs stand).
	fs::create_directories(abs, ec);
	for (const char* sub : { "content/models", "content/scenes",
			".tourmaline", "paks" })
		fs::create_directories(abs / sub, ec);
	const fs::path marker = abs / kProjectMarker;
	const std::string finalName =
		name.empty() ? abs.filename().generic_string() : name;
	if (fs::is_regular_file(marker, ec)) {
		// Adopt: an existing project opens (never rewrites its name
		// row). The ONE identity repair lives here and here only: a
		// marker too old to carry a guid gets its identity MINTED
		// (once — the row is never re-minted afterwards) and the
		// marker rewritten with the name preserved. Open's rejection
		// points here; everything else fails loud.
		const Marker m = ReadMarker(abs);
		if (!m.guid.empty()) {
			Project adopted = Open(abs.generic_string(), err);
			if (adopted.Valid())
				adopted.name = m.name.empty()
					? adopted.name : m.name;
			return adopted;
		}
		const std::string minted = smvk::tour::GenerateUuid();
		if (!smvk::cache::IsValidAssetGuid(minted)) {
			err = "uuid minting failed for '" + marker.generic_string()
				+ "'";
			return p;
		}
		const std::string keptName = m.name.empty()
			? finalName : m.name;
		if (!WriteMarker(marker, keptName, minted, err))
			return p;
		std::printf("[project] identity MINTED for '%s' (guid %s)"
			" — the marker was pre-identity format\n",
			abs.lexically_normal().generic_string().c_str(),
			minted.c_str());
		return Open(abs.generic_string(), err);
	}
	if (!WriteMarker(marker, finalName, smvk::tour::GenerateUuid(),
			err))
		return p;
	created = true;
	return Open(abs.generic_string(), err);
}

std::string Project::ContentRoot() const {
	return root; // the project IS the content tree (tours record
	             // content-relative sources against the root)
}

std::string Project::ImportsDir() const {
	return (std::filesystem::path(root) / "content" / "models")
		.generic_string();
}

std::string Project::DerivedDataDir() const {
	return (std::filesystem::path(root) / ".tourmaline").generic_string();
}

std::string Project::RegistryPath() const {
	return (std::filesystem::path(DerivedDataDir())
		/ "project-imports.txt").generic_string();
}

std::string Project::RelativeTo(const std::string& absPath) const {
	if (!Valid() || absPath.empty())
		return {};
	namespace fs = std::filesystem;
	std::error_code ec;
	const std::string norm = fs::absolute(fs::path(absPath), ec)
		.lexically_normal().generic_string();
	if (ec)
		return {};
	std::string rootN = fs::path(root).lexically_normal().generic_string();
	if (!rootN.empty() && rootN.back() != '/')
		rootN += '/';
	if (norm.size() <= rootN.size()
			|| norm.compare(0, rootN.size(), rootN) != 0)
		return {};
	std::string rel = norm.substr(rootN.size());
	// Belt-and-suspenders: never emit an escaping relative path.
	if (rel == ".." || rel.compare(0, 3, "../") == 0
			|| rel.find("/../") != std::string::npos)
		return {};
	return rel;
}

} // namespace smvk::project
