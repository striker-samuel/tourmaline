// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Project.exe: the project-pipeline CLI (a thin wrapper over the shared
// project_pipeline library — the editor calls the same implementation;
// there is no separate CLI logic of any kind).
//
//   Project.exe init   --project <dir> [--name <n>] [--from-gltf <glTF>]
//   Project.exe import --project <dir> --source <glTF|GLB>
//                      [--no-lods] [--no-meshlets] [--lod-ratios 0.75,0.5,0.3]
//                      [--lod-error 0.01] [--force]
//   Project.exe status --project <dir>        (the staleness report)
//
// Exit codes: 0 success, 1 pipeline failure, 2 usage.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <project_pipeline/import_pipeline.h>
#include <project_pipeline/project.h>

namespace {

int Usage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "Project.exe") << " <command> [flags]\n"
		<< "Commands:\n"
		<< "  init   --project <dir> [--name <name>] [--from-gltf <glTF|GLB>]\n"
		<< "         create (or adopt) a project; --from-gltf imports the\n"
		<< "         source into it after creation\n"
		<< "  import --project <dir> --source <glTF|GLB>\n"
		<< "         [--no-lods] [--no-meshlets] [--lod-ratios 0.75,0.5,0.3,0.15]\n"
		<< "         [--lod-error 0.01] [--shader-lods] [--force]\n"
		<< "         copy the source into the project + derive the native\n"
		<< "         artifact family (idempotent: unchanged = no-op)\n"
		<< "  status --project <dir>\n"
		<< "         report every imported source: current, or why not\n";
	return 2;
}

bool ParseRatios(const std::string& csv, std::vector<float>& out) {
	out.clear();
	size_t pos = 0;
	while (pos <= csv.size()) {
		size_t end = csv.find(',', pos);
		if (end == std::string::npos)
			end = csv.size();
		const std::string tok = csv.substr(pos, end - pos);
		char* endp = nullptr;
		const float v = std::strtof(tok.c_str(), &endp);
		if (tok.empty() || (endp && *endp != '\0') || !(v > 0.0f && v < 1.0f))
			return false;
		out.push_back(v);
		pos = end + 1;
	}
	return !out.empty();
}

} // namespace

int main(int argc, const char* argv[]) {
	if (argc < 2)
		return Usage(argv[0]);
	const std::string cmd = argv[1];

	// Shared flag scan.
	std::string projectDir, source, name, ratiosCsv;
	bool fromGltf = false, force = false;
	bool noLods = false, noMeshlets = false, shaderLods = false;
	bool ratiosSet = false, errorSet = false;
	float lodError = 0.01f;
	for (int i = 2; i < argc; ++i) {
		const char* a = argv[i];
		if ((std::strcmp(a, "--project") == 0) && i + 1 < argc)
			projectDir = argv[++i];
		else if (std::strncmp(a, "--project=", 10) == 0)
			projectDir = a + 10;
		else if ((std::strcmp(a, "--source") == 0
				|| std::strcmp(a, "--from-gltf") == 0) && i + 1 < argc) {
			source = argv[++i];
			fromGltf = std::strcmp(a, "--from-gltf") == 0;
		}
		else if (std::strncmp(a, "--from-gltf=", 12) == 0) {
			source = a + 12;
			fromGltf = true;
		}
		else if ((std::strcmp(a, "--name") == 0) && i + 1 < argc)
			name = argv[++i];
		else if (std::strncmp(a, "--name=", 7) == 0)
			name = a + 7;
		else if ((std::strcmp(a, "--lod-ratios") == 0) && i + 1 < argc) {
			ratiosCsv = argv[++i];
			ratiosSet = true;
		}
		else if (std::strncmp(a, "--lod-ratios=", 13) == 0) {
			ratiosCsv = a + 13;
			ratiosSet = true;
		}
		else if ((std::strcmp(a, "--lod-error") == 0) && i + 1 < argc) {
			lodError = std::strtof(argv[++i], nullptr);
			errorSet = true;
		}
		else if (std::strncmp(a, "--lod-error=", 12) == 0) {
			lodError = std::strtof(a + 12, nullptr);
			errorSet = true;
		}
		else if (std::strcmp(a, "--no-lods") == 0)
			noLods = true;
		else if (std::strcmp(a, "--no-meshlets") == 0)
			noMeshlets = true;
		else if (std::strcmp(a, "--shader-lods") == 0)
			shaderLods = true;
		else if (std::strcmp(a, "--force") == 0)
			force = true;
		else {
			std::cerr << "Unknown flag: " << a << "\n";
			return Usage(argv[0]);
		}
	}
	if (projectDir.empty()) {
		std::cerr << "--project is required.\n";
		return Usage(argv[0]);
	}

	if (cmd == "init") {
		bool created = false;
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Init(projectDir, name, created, err);
		if (!p.Valid()) {
			std::cerr << "init failed: " << err << "\n";
			return 1;
		}
		std::cout << "Project " << (created ? "created" : "adopted")
			<< ": " << p.root << " (name '" << p.name << "')\n";
		if (!fromGltf)
			return 0;
		// --from-gltf = the normal import right after creation (the same
		// path as a separate 'import' command — no special casing).
	}

	if (cmd == "import" || (cmd == "init" && fromGltf)) {
		if (source.empty()) {
			std::cerr << (cmd == "import"
				? "--source is required.\n"
				: "--from-gltf is required.\n");
			return Usage(argv[0]);
		}
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Open(projectDir, err);
		if (!p.Valid()) {
			std::cerr << err << "\n";
			return 1;
		}
		smvk::project::ImportRequest req;
		req.project = p;
		req.source = std::filesystem::path(source);
		req.settings.lods = !noLods;
		req.settings.meshlets = !noMeshlets;
		req.settings.shaderLods = shaderLods;
		if (ratiosSet && !ParseRatios(ratiosCsv, req.settings.lodRatios)) {
			std::cerr << "bad --lod-ratios '" << ratiosCsv
				<< "' (want comma-separated fractions in (0,1), e.g."
				" 0.75,0.5,0.3)\n";
			return 2;
		}
		if (errorSet)
			req.settings.lodTargetError = lodError;
		req.force = force;
		smvk::project::ImportResult result;
		if (!smvk::project::ImportProjectAsset(req, result, err)) {
			std::cerr << "import failed: " << err << "\n";
			return 1;
		}
		if (!result.derived) {
			std::cout << "Import current (no work): "
				<< result.geometry.generic_string() << "\n";
			return 0;
		}
		std::cout << "Imported: " << result.projectSource.generic_string()
			<< "\n  geometry=" << result.geometry.generic_string()
			<< "\n  guid=" << result.guid
			<< "\n  blobs=" << result.geometryBlobs
			<< " lods=" << result.lodLevels
			<< " meshletSets=" << result.meshletSets << "\n";
		return 0;
	}

	if (cmd == "status") {
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Open(projectDir, err);
		if (!p.Valid()) {
			std::cerr << err << "\n";
			return 1;
		}
		const std::vector<smvk::project::ImportStatus> rows =
			smvk::project::ProjectImportStatuses(p, &err);
		if (!err.empty())
			std::cerr << "registry: " << err << "\n";
		if (rows.empty()) {
			std::cout << "No imported sources (registry: "
				<< p.RegistryPath() << ")\n";
			return 0;
		}
		int stale = 0;
		for (const smvk::project::ImportStatus& r : rows) {
			std::cout << r.source << ": "
				<< (r.current ? "current" : "RE-DERIVE")
				<< " — " << r.why << "\n";
			if (!r.current)
				++stale;
		}
		std::cout << (rows.size() - stale) << "/" << rows.size()
			<< " current\n";
		return 0;
	}

	return Usage(argv[0]);
}
