// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// tourinit: deliberately thin CLI over tourmaline::tooling::InitializeProject.
// All project identity, import and recipe rules live in the linked C++ library.

#include <tourmaline/tooling.h>

#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string QuoteJson(std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x20) {
            out += "\\u00";
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0xF]);
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out + "\"";
}

void Usage() {
    std::cout <<
        "Usage: tourinit --name <display-name> [--path <directory>] [options]\n"
        "       tourinit --path <directory> [options]\n\n"
        "Creates project.tourproj via the canonical Project::Init API, a\n"
        "starter .tourrecipe, default build settings and a README. The\n"
        "project root is ./<name> when --path is omitted.\n\n"
        "Options:\n"
        "  --name <name>          Display name (spaces allowed)\n"
        "  --path <directory>     Target project root (absolute or relative)\n"
        "  --recipe <stem>        Starter recipe basename (default game)\n"
        "  --pak <stem>           Entry package basename (default world)\n"
        "  --entry <path>         Project-relative content/scenes/*.tour entry\n"
        "  --from-gltf <file>     Import an existing .gltf or .glb source\n"
        "  --from-scene <file>    Seed an authored .tour (requires --from-gltf)\n"
        "  --bake-gi             Enable GI bake in starter recipe\n"
        "  --bake-cubemaps       Enable cubemap bake in starter recipe\n"
        "  --adopt               Allow existing directory/project, preserve files\n"
        "  --no-gitignore        Do not write a .gitignore\n"
        "  --dry-run             Validate/preview without changing disk\n"
        "  --json                Structured one-line report to stdout\n"
        "  --help                Print this help\n\n"
        "Exit 0 = initialized or planned; 1 = failed; 2 = bad arguments.\n"
        "A starter recipe is syntactically valid but not cookable until\n"
        "the referenced authored .tour scene and source asset exist.\n";
}

} // namespace

int main(int argc, char** argv) {
    using namespace tourmaline::tooling;
    ProjectInitOptions options;
    bool json = false;
    if (argc == 1) {
        Usage();
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            Usage();
            return 0;
        }
        auto take = [&]() -> std::string {
            if (++i >= argc) {
                std::cerr << "tourinit: missing value after " << arg << "\n";
                return {};
            }
            return argv[i];
        };
        if (arg == "--name") options.name = take();
        else if (arg == "--path") options.root = take();
        else if (arg == "--recipe") options.recipeName = take();
        else if (arg == "--pak") options.pakName = take();
        else if (arg == "--entry") options.entryScene = take();
        else if (arg == "--from-gltf") options.fromGltf = take();
        else if (arg == "--from-scene") options.fromScene = take();
        else if (arg == "--bake-gi") options.bakeGi = true;
        else if (arg == "--bake-cubemaps") options.bakeCubemaps = true;
        else if (arg == "--adopt") options.adoptExisting = true;
        else if (arg == "--no-gitignore") options.writeGitignore = false;
        else if (arg == "--dry-run") options.dryRun = true;
        else if (arg == "--json") json = true;
        else {
            std::cerr << "tourinit: unknown option " << arg << "\n";
            return 2;
        }
        if (i == argc || (arg == "--name" && options.name.empty()) ||
            (arg == "--path" && options.root.empty())) {
            std::cerr << "tourinit: missing required value for " << arg << "\n";
            return 2;
        }
    }

    ProjectInitResult result;
    std::string error;
    if (!InitializeProject(options, result, error)) {
        std::cerr << "tourinit: " << error << "\n";
        return 1;
    }
    if (json) {
        std::cout << "{\"schema_version\":1,\"root\":"
                  << QuoteJson(result.root) << ",\"name\":" << QuoteJson(result.name)
                  << ",\"guid\":" << QuoteJson(result.guid)
                  << ",\"recipe\":" << QuoteJson(result.recipe)
                  << ",\"entry_scene\":" << QuoteJson(result.entryScene)
                  << ",\"created\":" << (result.created ? "true" : "false")
                  << ",\"imported\":" << (result.imported ? "true" : "false")
                  << ",\"seeded_scene\":" << (result.seededScene ? "true" : "false")
                  << ",\"recipe_written\":" << (result.recipeWritten ? "true" : "false")
                  << ",\"ready_to_build\":" << (result.readyToBuild ? "true" : "false")
                  << ",\"dry_run\":" << (result.dryRun ? "true" : "false")
                  << "}\n";
    } else {
        std::cout << "tourinit: " << (result.dryRun ? "would create " :
                       result.created ? "created " : "adopted ")
                  << result.root << "\n"
                  << "  name: " << result.name << "\n"
                  << "  recipe: " << result.recipe << "\n"
                  << "  entry: " << result.entryScene << "\n"
                  << "  build-ready: " << (result.readyToBuild ? "entry scene exists" :
                      "needs an authored scene/source") << "\n";
    }
    return 0;
}
