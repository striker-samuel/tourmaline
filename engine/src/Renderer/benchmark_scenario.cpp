// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/benchmark_scenario.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace engine::benchmark {
namespace fs = std::filesystem;
namespace {

std::string Slug(const std::string& label) {
    std::string id;
    bool dash = false;
    for (unsigned char c : label) {
        if (std::isalnum(c) && c < 128) {
            if (dash && !id.empty()) id.push_back('-');
            dash = false;
            id.push_back(char(std::tolower(c)));
        } else {
            dash = true;
        }
    }
    if (id.size() > 64) id.resize(64);
    while (!id.empty() && id.back() == '-') id.pop_back();
    return id;
}

fs::path FindCatalog() {
    fs::path dir = fs::current_path();
    while (true) {
        const fs::path candidate = dir / "benchmarks" / "scenarios.json";
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(candidate))
            return candidate;
        const fs::path parent = dir.parent_path();
        if (parent == dir || parent.empty()) break;
        dir = parent;
    }
    return {};
}

} // namespace

bool Save(const Scenario& scenario, std::string& message) {
    const std::string baseId = Slug(scenario.label);
    if (baseId.empty() || scenario.sourceArgs.empty() || scenario.frames < 1) {
        message = "Enter a label and launch a cooked pak/project scene.";
        return false;
    }
    const fs::path catalog = FindCatalog();
    if (catalog.empty()) {
        message = "benchmarks/scenarios.json not found: launch from the source checkout/build tree.";
        return false;
    }
    try {
        nlohmann::json root;
        {
            std::ifstream input(catalog);
            if (!input || !(input >> root)) {
                message = "Could not parse scenario catalog; no changes written.";
                return false;
            }
        }
        if (!root.is_object() || root.value("schema_version", 0) != 1 ||
            !root.contains("scenarios") || !root["scenarios"].is_array()) {
            message = "Unknown benchmark catalog schema; no changes written.";
            return false;
        }

        // Append-only IDs: saving a second camera under the same label must
        // not erase an earlier stress location without an explicit git edit.
        std::string id = baseId;
        int suffix = 2;
        auto occupied = [&](const std::string& candidate) {
            for (const auto& entry : root["scenarios"])
                if (entry.is_object() && entry.value("id", "") == candidate)
                    return true;
            return false;
        };
        while (occupied(id)) id = baseId + "-" + std::to_string(suffix++);

        nlohmann::json entry = {
            {"id", id},
            {"label", scenario.label},
            {"subsystem", scenario.subsystem},
            {"notes", scenario.notes},
            {"source_args", scenario.sourceArgs},
            {"app_args", scenario.appArgs},
            {"environment", scenario.environment},
            {"camera", {
                {"position", scenario.position},
                {"forward", scenario.forward}
            }},
            {"frames", scenario.frames}
        };
        root["scenarios"].push_back(std::move(entry));

        // Write the candidate first. Keep the original if writing fails.
        const fs::path temp = fs::path(catalog.string() + ".tmp");
        {
            std::ofstream output(temp, std::ios::trunc);
            if (!output) {
                message = "Could not open temporary catalog for writing.";
                return false;
            }
            output << root.dump(2) << '\n';
            output.flush();
            if (!output) {
                message = "Could not write temporary catalog.";
                return false;
            }
        }
        // Windows won't rename across an existing file. The temporary file
        // is complete before replacing the git-tracked catalog.
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), catalog.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            message = "Could not replace scenario catalog (is it locked?).";
            return false;
        }
#else
        fs::rename(temp, catalog);
#endif
        message = "Saved '" + id + "' to " + catalog.generic_string() +
            " (commit benchmarks/scenarios.json).";
        return true;
    } catch (const std::exception& e) {
        message = std::string("Scenario export failed: ") + e.what();
        return false;
    }
}

} // namespace engine::benchmark
