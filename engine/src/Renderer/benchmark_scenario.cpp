// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/WorldRenderer.h>
#include <Engine/Renderer/benchmark_scenario.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace engine::benchmark {
namespace fs = std::filesystem;
namespace {

std::string Slug(const std::string& label) {
    std::string id;
    bool dash = false;
    for (unsigned char c : label) {
        if (std::isalnum(c) && c < 128) {
            if (dash && !id.empty())
                id.push_back('-');
            dash = false;
            id.push_back(char(std::tolower(c)));
        } else {
            dash = true;
        }
    }
    if (id.size() > 64)
        id.resize(64);
    while (!id.empty() && id.back() == '-')
        id.pop_back();
    return id;
}

fs::path FindCatalog() {
    fs::path dir = fs::current_path();
    while (true) {
        const fs::path candidate = dir / "benchmarks" / "scenarios.json";
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(candidate))
            return candidate;
        const fs::path parent = dir.parent_path();
        if (parent == dir || parent.empty())
            break;
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
        message =
            "benchmarks/scenarios.json not found: launch from the source checkout/build tree.";
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
                if (entry.is_object() && entry.value("id", std::string{}) == candidate)
                    return true;
            return false;
        };
        while (occupied(id))
            id = baseId + "-" + std::to_string(suffix++);

        nlohmann::json entry = {
            {"id", id},
            {"label", scenario.label},
            {"subsystem", scenario.subsystem},
            {"notes", scenario.notes},
            {"source_args", scenario.sourceArgs},
            {"app_args", scenario.appArgs},
            {"environment", scenario.environment},
            {"camera", {{"position", scenario.position}, {"forward", scenario.forward}}},
            {"frames", scenario.frames}};
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
        if (!MoveFileExW(temp.c_str(),
                         catalog.c_str(),
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

namespace engine {

void WorldRenderer::ExportBenchmarkScenario() {
    benchmark::Scenario s;
    s.label = m_benchmarkLabel;
    s.subsystem = m_benchmarkSubsystem;
    s.notes = m_benchmarkNotes;
    s.sourceArgs = m_benchmarkSourceArgs;
    s.appArgs = m_benchmarkAppArgs;
    const glm::vec3 pos = m_cameraPositioner.cameraPosition_;
    const glm::vec3 fwd = glm::normalize(m_cameraPositioner.getForward());
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) ||
        !std::isfinite(fwd.x) || !std::isfinite(fwd.y) || !std::isfinite(fwd.z)) {
        m_benchmarkExportMessage = "Camera contains non-finite coordinates.";
        return;
    }
    s.position = {pos.x, pos.y, pos.z};
    s.forward = {fwd.x, fwd.y, fwd.z};
    auto scalar = [](float x) {
        std::ostringstream stream;
        stream << std::setprecision(9) << x;
        return stream.str();
    };
    auto vec = [&](const glm::vec3& v) {
        return scalar(v.x) + "," + scalar(v.y) + "," + scalar(v.z);
    };
    auto pin = [&](const char* name, bool v) { s.environment[name] = v ? "1" : "0"; };
    s.environment["TOURMALINE_CAM_POS"] = vec(pos);
    s.environment["TOURMALINE_CAM_FWD"] = vec(fwd);
    s.environment["TOURMALINE_RENDER_SCALE"] = scalar(m_renderScale);
    switch (m_aaMode) {
    case AaMode::Off:
        s.environment["TOURMALINE_AA"] = "off";
        break;
    case AaMode::Msaa2x:
        s.environment["TOURMALINE_AA"] = "msaa2x";
        break;
    case AaMode::Smaa1x:
        s.environment["TOURMALINE_AA"] = "smaa1x";
        break;
    case AaMode::SmaaT2x:
        s.environment["TOURMALINE_AA"] = "smaat2x";
        break;
    }
    s.environment["TOURMALINE_AO"] = m_aoMode == AoMode::None   ? "off"
                                     : m_aoMode == AoMode::Ssao ? "ssao"
                                                                : "sao";
    switch (m_textureFiltering) {
    case TextureFiltering::Trilinear:
        s.environment["TOURMALINE_TEXTURE_FILTER"] = "trilinear";
        break;
    case TextureFiltering::Anisotropic2x:
        s.environment["TOURMALINE_TEXTURE_FILTER"] = "2x";
        break;
    case TextureFiltering::Anisotropic4x:
        s.environment["TOURMALINE_TEXTURE_FILTER"] = "4x";
        break;
    case TextureFiltering::Anisotropic8x:
        s.environment["TOURMALINE_TEXTURE_FILTER"] = "8x";
        break;
    case TextureFiltering::Anisotropic16x:
        s.environment["TOURMALINE_TEXTURE_FILTER"] = "16x";
        break;
    }
    pin("TOURMALINE_SSR", m_ssrEnabled);
    pin("TOURMALINE_SSGI", m_ssgiDiffuseEnabled);
    pin("TOURMALINE_DS", m_dsEnabled);
    pin("TOURMALINE_EVSM", m_evsmEnabled);
    pin("TOURMALINE_SDSM", m_sdsmEnabled);
    pin("TOURMALINE_TERRAIN", m_passes.terrain && m_terrainEnabled);
    pin("TOURMALINE_CUBEMAP", m_cubemapEnabled);
    pin("TOURMALINE_LIGHTMAP", m_lmEnabled);
    pin("TOURMALINE_CONTACT_SHADOWS", m_contactShadowsEnabled);
    s.environment["TOURMALINE_CONTACT_SHADOWS_STEPS"] = std::to_string(m_contactShadowSteps);
    s.environment["TOURMALINE_CONTACT_SHADOWS_DIST"] = scalar(m_contactShadowMaxDistance);
    s.environment["TOURMALINE_CONTACT_SHADOWS_THICKNESS"] = scalar(m_contactShadowThickness);
    s.environment["TOURMALINE_CONTACT_SHADOWS_RES"] =
        std::to_string(m_contactShadowResolutionDivisor);
    pin("TOURMALINE_VOLUMETRIC", m_volumetricEnabled);
    s.environment["TOURMALINE_VOLUMETRIC_STEPS"] = std::to_string(m_volumetricSteps);
    s.environment["TOURMALINE_VOLUMETRIC_INTENSITY"] = scalar(m_volumetricIntensity);
    s.environment["TOURMALINE_VOLUMETRIC_SUN"] = scalar(m_volumetricSun);
    s.environment["TOURMALINE_VOLUMETRIC_GI"] = scalar(m_volumetricGi);
    s.environment["TOURMALINE_GI_INTENSITY"] = scalar(m_giIntensity);
    s.environment["TOURMALINE_BLOOM_INTENSITY"] = scalar(m_bloomIntensity);
    s.environment["TOURMALINE_BLOOM_THRESHOLD"] = scalar(m_bloomThreshold);
    // The benchmark host deliberately fixes exposure off to keep the
    // comparison deterministic; do not export the live exposure checkbox.
    s.environment["TOURMALINE_AUTO_EXPOSURE"] = "0";

    switch (m_dlssMode) {
    case smvk::DlssMode::Off:
        s.environment["TOURMALINE_DLSS"] = "off";
        break;
    case smvk::DlssMode::UltraPerformance:
        s.environment["TOURMALINE_DLSS"] = "ultraperf";
        break;
    case smvk::DlssMode::MaxPerformance:
        s.environment["TOURMALINE_DLSS"] = "perf";
        break;
    case smvk::DlssMode::Balanced:
        s.environment["TOURMALINE_DLSS"] = "balanced";
        break;
    case smvk::DlssMode::MaxQuality:
        s.environment["TOURMALINE_DLSS"] = "quality";
        break;
    case smvk::DlssMode::UltraQuality:
        s.environment["TOURMALINE_DLSS"] = "ultraquality";
        break;
    case smvk::DlssMode::DLAA:
        s.environment["TOURMALINE_DLSS"] = "dlaa";
        break;
    }
    // Recreate the pass graph as well as its runtime enable pins.
    // The csv is one --with and one --without (the CLI parser stores only
    // the last occurrence of each). This is why toggling Bloom or DOF is
    // folded into the same final pass list instead of another --without.
    std::string enabled, disabled;
    auto pass = [&](const char* token, bool on) {
        std::string& csv = on ? enabled : disabled;
        if (!csv.empty())
            csv += ",";
        csv += token;
    };
    pass("deferred", m_passes.deferredShadows);
    pass("ao", m_passes.ssao);
    pass("smaa", m_passes.smaa);
    pass("dof", m_passes.dof && m_dofEnabled);
    pass("ssr", m_passes.ssr);
    pass("bloom", m_passes.bloom && m_bloomEnabled);
    pass("exposure", m_passes.exposure && m_autoExposureEnabled);
    pass("msaa", m_passes.msaa);
    pass("skydome", m_passes.skydome);
    pass("ui", m_passes.ui);
    pass("cull", m_passes.forwardPlusCull);
    pass("particles", m_passes.particles && m_particlesEnabled);
    pass("prepass", m_passes.forwardPlusPrepass);
    pass("filter", m_passes.gfxFilter);
    pass("terrain", m_passes.terrain && m_terrainEnabled);
    pass("ssgi", m_passes.ssgiDiffuse && m_ssgiDiffuseEnabled);
    pass("volumetric", m_passes.volumetric && m_volumetricEnabled);
    pass("contact", m_passes.contactShadows && m_contactShadowsEnabled);
    if (!enabled.empty())
        s.appArgs.push_back("--with=" + enabled);
    if (!disabled.empty())
        s.appArgs.push_back("--without=" + disabled);
    // Live-only tweak sliders have no RunConfig equivalent and are not
    // falsely advertised as replayable.
    benchmark::Save(s, m_benchmarkExportMessage);
}

} // namespace engine
