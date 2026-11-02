// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

namespace engine::benchmark {
struct Scenario {
    std::string label;
    std::string subsystem;
    std::string notes;
    std::vector<std::string> sourceArgs;
    std::vector<std::string> appArgs;
    std::map<std::string, std::string> environment;
    std::array<float, 3> position{};
    std::array<float, 3> forward{};
    int frames = 120;
};

// Appends to a source-controlled catalog, never overwriting an existing ID.
// Reports errors if outside a source checkout or the catalog is malformed.
bool Save(const Scenario& scenario, std::string& message);
} // namespace engine::benchmark
