// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Project open/init + path accessors (see project.h). Extends the
// editor's existing project.tourproj marker model (editor_project.cpp
// keeps its own copy of the marker parse — same format, both sides
// agree; the editor is the legacy consumer, this is the pipeline one).

#include <project_pipeline/project.h>

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

bool HasModelExtension(const std::string& path) {
	auto lowerTail = [&](size_t n) {
		if (path.size() < n)
			return std::string();
		std::string t = path.substr(path.size() - n);
		for (char& c : t)
			c = char(std::tolower((unsigned char)c));
		return t;
	};
	return lowerTail(4) == ".glb" || lowerTail(5) == ".gltf";
}

// The marker rows (name=value lines; "#"-prefixed comments).
struct Marker {
	std::string name;
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
	}
	return out;
}

} // namespace

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
	p.root = abs.lexically_normal().generic_string();
	p.name = m.name.empty()
		? abs.filename().generic_string() : m.name;
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
	if (fs::is_regular_file(marker, ec)) {
		// Adopt: an existing project opens (never rewrites its name row).
		return Open(abs.generic_string(), err);
	}
	const std::string finalName =
		name.empty() ? abs.filename().generic_string() : name;
	{
		std::ofstream out(marker, std::ios::binary | std::ios::trunc);
		if (!out) {
			err = "cannot write '" + marker.generic_string() + "'";
			return p;
		}
		out << "# Tourmaline project marker\n"
			<< "name=" << finalName << "\n"
			<< "version=1\n";
		if (!out.good()) {
			err = "failed writing '" + marker.generic_string() + "'";
			return p;
		}
	}
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
