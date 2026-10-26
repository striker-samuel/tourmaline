// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The project import REGISTRY (<root>/.tourmaline/project-imports.txt):
// one record per imported source family — the project's own bookkeeping
// of WHAT was imported, under WHICH settings, from WHERE. The manifest
// (<stem>.gltf.meta) remains the artifact-family identity; this is the
// PROJECT-level record (staleness reports, reimport settings, the
// original-location metadata).
//
// Format (key=value; "#"-comments; "\t" prefixes the per-record rows):
//   import src=<project-relative glTF path>
//     guid=<manifest guid>
//     original=<original external path (reimport metadata ONLY — the
//               project relocates freely, this path is never resolved)>
//     fingerprint=<import fingerprint at publish time>
//     geometry=<blobs> lods=<levels> meshlets=<sets>

#include <cstdio>
#include <fstream>
#include <sstream>

#include <project_pipeline/import_pipeline.h>

#include "registry_detail.h"

namespace smvk::project {

namespace {

struct RegistryRow {
	ImportStatus st;
	uint32_t geometry = 0;
	uint32_t lods = 0;
	uint32_t meshlets = 0;
};

std::vector<RegistryRow> ParseRegistry(const std::string& path) {
	std::vector<RegistryRow> rows;
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return rows;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		if (line.compare(0, 11, "import src=") == 0) {
			RegistryRow row;
			row.st.source = line.substr(11);
			row.st.known = true;
			rows.push_back(std::move(row));
			continue;
		}
		if (!rows.empty() && (line[0] == '\t' || line[0] == ' ')) {
			const std::string body = line.substr(1);
			const size_t eq = body.find('=');
			if (eq == std::string::npos)
				continue;
			const std::string key = body.substr(0, eq);
			const std::string val = body.substr(eq + 1);
			RegistryRow& r = rows.back();
			if (key == "guid") r.st.guid = val;
			else if (key == "original") r.st.original = val;
			else if (key == "fingerprint") r.st.fingerprint = val;
			else if (key == "geometry") r.geometry = uint32_t(std::atoi(val.c_str()));
			else if (key == "lods") r.lods = uint32_t(std::atoi(val.c_str()));
			else if (key == "meshlets") r.meshlets = uint32_t(std::atoi(val.c_str()));
		}
	}
	return rows;
}

bool WriteRegistry(const std::string& path,
		const std::vector<RegistryRow>& rows) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out << "# Tourmaline project import registry (one record per imported"
		" source family; the project-relative src is the key)\n";
	for (const RegistryRow& r : rows) {
		out << "import src=" << r.st.source << "\n"
			<< "\tguid=" << r.st.guid << "\n"
			<< "\toriginal=" << r.st.original << "\n"
			<< "\tfingerprint=" << r.st.fingerprint << "\n"
			<< "\tgeometry=" << r.geometry << " lods=" << r.lods
			<< " meshlets=" << r.meshlets << "\n";
	}
	return out.good();
}

} // namespace

std::vector<ImportStatus> ProjectImportStatuses(const Project& project,
	std::string* err) {
	std::vector<ImportStatus> out;
	if (!project.Valid()) {
		if (err)
			*err = "invalid project";
		return out;
	}
	std::vector<RegistryRow> rows = ParseRegistry(project.RegistryPath());
	const std::string fingerprintWanted =
		smvk::assets::ImportFingerprintFor(GeometryImportSettings{});
	for (RegistryRow& r : rows) {
		// The decision: the project copy exists, the family is current
		// under the DEFAULT settings, and the registry fingerprint
		// matches this toolchain. (A settings-mismatched row reports
		// not-current with the reason — the caller decides.)
		namespace fs = std::filesystem;
		const fs::path projectRoot(project.root);
		const fs::path gltf = projectRoot / fs::path(r.st.source);
		std::error_code ec;
		if (!fs::is_regular_file(gltf, ec)) {
			r.st.current = false;
			r.st.why = "the project source copy is missing";
			out.push_back(r.st);
			continue;
		}
		const bool stale = smvk::assets::MeshArtifactStale(
			gltf, GeometryImportSettings{});
		if (stale) {
			r.st.current = false;
			r.st.why = "source content or import settings changed since"
				" the artifact was derived";
		}
		else if (!r.st.fingerprint.empty()
				&& r.st.fingerprint != fingerprintWanted) {
			r.st.current = false;
			r.st.why = "the registry fingerprint predates the current"
				" import settings/revision";
		}
		else {
			r.st.why = "current";
		}
		out.push_back(r.st);
	}
	return out;
}

// ---- the registry write side (the import pipeline's tail) ----------------
//
// Internal (registry_detail.h): only ImportProjectAsset updates records.
// The row set is rewritten whole (parse + mutate + write) — the registry
// is small and the file is the project's own derived data.
namespace registry {

bool UpdateRow(const Project& project, const ImportResult& result,
	const std::string& projectRelativeSource,
	const std::string& originalSource,
	const std::string& importFingerprint) {
	std::vector<RegistryRow> rows = ParseRegistry(project.RegistryPath());
	RegistryRow row;
	row.st.source = projectRelativeSource;
	row.st.guid = result.guid;
	row.st.original = originalSource;
	row.st.fingerprint = importFingerprint;
	row.geometry = result.geometryBlobs;
	row.lods = result.lodLevels;
	row.meshlets = result.meshletSets;
	// Replace-or-append keyed on the source path.
	bool replaced = false;
	for (RegistryRow& r : rows) {
		if (r.st.source == row.st.source) {
			r = row;
			replaced = true;
			break;
		}
	}
	if (!replaced)
		rows.push_back(std::move(row));
	return WriteRegistry(project.RegistryPath(), rows);
}

} // namespace registry
} // namespace smvk::project
