// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <tourmaline/tooling.h>

#include <project_pipeline/import_pipeline.h>
#include <project_pipeline/project.h>
#include <project_pipeline/project_recipe.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace tourmaline::tooling {
namespace {
namespace fs = std::filesystem;

bool SafeStem(const std::string& stem) {
    if (stem.empty() || stem.size() > 80)
        return false;
    for (char c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            return false;
    return true;
}

bool SafeRelativeScene(const std::string& path) {
    const fs::path value(path);
    if (value.empty() || value.is_absolute() || value.has_root_name() ||
        value.extension() != ".tour")
        return false;
    for (const auto& segment : value) {
        if (segment == ".." || segment == "." || segment.empty())
            return false;
    }
    const std::string normalized = value.generic_string();
    return normalized.rfind("content/scenes/", 0) == 0;
}

bool SaveNewFile(const fs::path& path, const std::string& body, std::string& error) {
    std::error_code ec;
    if (fs::exists(path, ec))
        return true; // user-authored file takes precedence
    if (ec) {
        error = "cannot inspect " + path.generic_string() + ": " + ec.message();
        return false;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::out);
    if (!stream) {
        error = "cannot create " + path.generic_string();
        return false;
    }
    stream << body;
    if (!stream.good()) {
        error = "cannot finish writing " + path.generic_string();
        return false;
    }
    return true;
}

std::string DisplayName(const fs::path& root, const std::string& requested) {
    const fs::path base = root.filename().empty() ? root.parent_path().filename() : root.filename();
    return requested.empty() ? base.generic_string() : requested;
}

std::string Settings() {
    return "# Tourmaline project build settings (legacy single-scene stage flags)\n"
           "cubemap=on\nlightmap=off\ngiBackend=embree\n"
           "giMaxProbes=131072\n";
}

std::string Ignore() {
    return "# Generated caches, cooked packages and imported derivatives\n"
           ".tourmaline/\npaks/\nbuild/\n*.meta\n";
}

std::string Readme(const std::string& name, const std::string& recipe, const std::string& entry) {
    return "# " + name +
           "\n\n"
           "Tourmaline project scaffold. The project.tourproj GUID is its permanent\n"
           "identity; do not regenerate it when moving or renaming this directory.\n\n"
           "The recipe is a *starter* until the entry scene's glTF/geometry reference\n"
           "is authored (a placeholder .tour is NOT fabricated by tourinit).\n\n"
           "Next steps (from the Tourmaline build directory):\n\n"
           "1. Import a model with Project import --project <this-dir> --source <model.glb>.\n"
           "2. Author or seed " +
           entry +
           " using the editor or Project init --from-scene.\n"
           "3. Project validate --project <this-dir> --recipe " +
           recipe +
           "\n"
           "4. Project build --project <this-dir> --recipe " +
           recipe +
           "\n\n"
           "See the repository docs/agent_cli_tools.md manual for inspection,\n"
           "capture and release comparison commands.\n";
}

std::string Recipe(const std::string& name,
                   const std::string& entry,
                   const std::string& pak,
                   bool bakeGi,
                   bool bakeCubemaps) {
    return "# Tourmaline project recipe v1. Requires a real imported scene.\n"
           "recipe 1\nproject \"" +
           name +
           "\"\n"
           "entry " +
           entry +
           "\n"
           "bake gi " +
           (bakeGi ? "on" : "off") +
           "\n"
           "bake cubemaps " +
           (bakeCubemaps ? "on" : "off") +
           "\n\n"
           "[pak world]\noutput " +
           pak +
           ".pak\n"
           "scene " +
           entry + "\n";
}

} // namespace

bool InitializeProject(const ProjectInitOptions& options,
                       ProjectInitResult& out,
                       std::string& error) {
    error.clear();
    ProjectInitResult result;
    if (!SafeStem(options.recipeName) || !SafeStem(options.pakName)) {
        error = "recipeName and pakName must be short ASCII letters/numbers/_/-";
        return false;
    }
    const std::string requestedName = options.name;
    if (requestedName.find_first_of("\r\n\"") != std::string::npos || requestedName.size() > 128) {
        error = "project name cannot contain quotes or newlines; maximum 128 bytes";
        return false;
    }
    fs::path root = options.root;
    if (root.empty() && !requestedName.empty())
        root = fs::current_path() / requestedName;
    if (root.empty()) {
        error = "provide --name and/or --path";
        return false;
    }
    std::error_code ec;
    root = fs::absolute(root, ec).lexically_normal();
    if (ec) {
        error = "cannot resolve project root: " + ec.message();
        return false;
    }
    if (root == root.root_path()) {
        error = "refusing to initialize a filesystem root";
        return false;
    }
    const std::string name = DisplayName(root, requestedName);
    if (name.empty() || name.find_first_of("\r\n\"") != std::string::npos) {
        error = "invalid project name";
        return false;
    }
    if (!SafeRelativeScene(options.entryScene)) {
        error = "entryScene must be a project-relative content/scenes/*.tour path";
        return false;
    }
    if (!options.fromScene.empty() && options.fromGltf.empty()) {
        error = "--from-scene requires --from-gltf so the root reference can be repointed";
        return false;
    }
    if (!options.fromScene.empty() && options.entryScene != "content/scenes/main.tour" &&
        options.entryScene != "content/scenes/" + options.fromScene.filename().generic_string()) {
        error = "entryScene conflicts with the path created by --from-scene";
        return false;
    }
    if (!options.fromGltf.empty() &&
        ((options.fromGltf.extension() != ".gltf" && options.fromGltf.extension() != ".glb") ||
         !fs::is_regular_file(options.fromGltf))) {
        error = "--from-gltf must name an existing .gltf or .glb file";
        return false;
    }
    if (!options.fromScene.empty() &&
        (options.fromScene.extension() != ".tour" || !fs::is_regular_file(options.fromScene))) {
        error = "--from-scene must name an existing .tour file";
        return false;
    }

    const bool alreadyExists = fs::exists(root, ec);
    if (ec) {
        error = "cannot inspect project destination: " + ec.message();
        return false;
    }
    if (alreadyExists && !fs::is_directory(root, ec)) {
        error = "destination exists but is not a directory";
        return false;
    }
    if (alreadyExists && !options.adoptExisting) {
        error = "destination already exists; use --adopt to retain existing content";
        return false;
    }

    result.root = root.generic_string();
    result.name = name;
    result.recipe = (root / (options.recipeName + ".tourrecipe")).generic_string();
    result.entryScene = options.entryScene;
    result.dryRun = options.dryRun;
    result.created = !alreadyExists;
    if (!options.fromScene.empty())
        result.entryScene = "content/scenes/" + options.fromScene.filename().generic_string();
    result.readyToBuild = fs::is_regular_file(root / result.entryScene);
    if (options.dryRun) {
        out = std::move(result);
        return true;
    }

    bool created = false;
    smvk::project::Project project =
        smvk::project::Project::Init(result.root, name, created, error);
    if (!project.Valid())
        return false;
    result.created = created;
    result.name = project.name;
    result.guid = project.guid;

    if (!options.fromGltf.empty()) {
        smvk::project::ImportRequest req{};
        req.project = project;
        req.source = options.fromGltf;
        smvk::project::ImportResult imported;
        if (!smvk::project::ImportProjectAsset(req, imported, error)) {
            error = "import failed: " + error;
            return false;
        }
        result.imported = true;
        if (!options.fromScene.empty()) {
            const std::string seeded =
                smvk::project::SeedProjectScene(project,
                                                options.fromScene.string(),
                                                options.fromGltf.stem().generic_string(),
                                                error);
            if (seeded.empty()) {
                error = "scene seed failed: " + error;
                return false;
            }
            result.entryScene = seeded;
            result.seededScene = true;
        }
    }

    const std::string text = Recipe(
        result.name, result.entryScene, options.pakName, options.bakeGi, options.bakeCubemaps);
    smvk::project::ProjectRecipe parsed;
    smvk::project::RecipeDiagnostic diagnostic;
    if (!smvk::project::ParseProjectRecipeText(text, parsed, diagnostic)) {
        error = "generated recipe is invalid: " + diagnostic.message;
        return false;
    }
    const fs::path recipePath(result.recipe);
    if (!fs::exists(recipePath)) {
        if (!SaveNewFile(recipePath, text, error))
            return false;
        result.recipeWritten = true;
    }
    if (!SaveNewFile(root / ".tourmaline" / "build-settings.txt", Settings(), error))
        return false;
    if (options.writeGitignore && !SaveNewFile(root / ".gitignore", Ignore(), error))
        return false;
    if (!SaveNewFile(root / "README.md",
                     Readme(result.name, options.recipeName + ".tourrecipe", result.entryScene),
                     error))
        return false;
    result.readyToBuild = fs::is_regular_file(root / result.entryScene);
    out = std::move(result);
    return true;
}

} // namespace tourmaline::tooling
