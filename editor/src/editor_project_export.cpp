// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The PROJECT export surface. The authored .tourrecipe (not the old
// one-pak save dialog) is the source of truth for package ownership,
// dependencies, entry scene and bake configuration.
#include "editor_app.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <project_pipeline/build_plan.h>
#include <project_pipeline/project.h>
#include <project_pipeline/project_recipe.h>
#include <project_pipeline/recipe_resolver.h>

namespace editor {
namespace {

bool RecipeNameValid(const std::string& filename) {
    namespace fs = std::filesystem;
    if (filename.empty() || filename.front() == '.'
        || filename.find('/') != std::string::npos
        || filename.find('\\') != std::string::npos)
        return false;
    const fs::path p(filename);
    return p.filename() == p && p.extension() == ".tourrecipe";
}

std::string RecipeDefaultScene(const std::string& root,
    const std::string& currentTour) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::path(root).lexically_normal();
    if (!currentTour.empty()) {
        const fs::path tour(currentTour);
        const fs::path full = tour.is_absolute() ? tour
            : (base / tour);
        const fs::path rel = full.lexically_normal().lexically_relative(base);
        const std::string name = rel.generic_string();
        if (rel.extension() == ".tour"
            && name.rfind("content/", 0) == 0
            && fs::is_regular_file(full, ec))
            return name;
    }
    const fs::path scenes = base / "content" / "scenes";
    std::vector<std::string> candidates;
    for (fs::recursive_directory_iterator it(scenes, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".tour") {
            const std::string rel = it->path()
                .lexically_relative(base).generic_string();
            candidates.push_back(rel);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.empty() ? "" : candidates.front();
}

std::string DefaultRecipeText(const std::string& projectName,
    const std::string& entry) {
    const std::string safeName = projectName.find('"') == std::string::npos
        ? projectName : "Project";
    const std::string name = safeName.find_first_of(" #\t") != std::string::npos
        ? "\"" + safeName + "\"" : safeName;
    const std::string scene = entry.empty()
        ? "content/scenes/main.tour" : entry;
    return "# Tourmaline project recipe v1\n"
        "# Edit this configuration, then Validate, Save or Build Project.\n"
        "# Paths are relative to the project root.\n"
        "recipe 1\nproject " + name + "\nentry " + scene
        + "\n\nbake gi off\nbake cubemaps off\nbake sdf off\n\n"
        "[pak world]\noutput game.pak\nscene " + scene + "\n";
}

// Update one bake switch without regenerating the entire .tourrecipe:
// other directives, package sections, blank lines and comments survive.
std::string WithBakeFlag(const std::string& source,
    const std::string& target, bool enabled) {
    const std::string replacement = "bake " + target
        + (enabled ? " on" : " off");
    size_t start = 0;
    while (start < source.size()) {
        const size_t end = source.find('\n', start);
        const size_t stop = end == std::string::npos ? source.size() : end;
        const std::string line = source.substr(start, stop - start);
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos
            && line.compare(first, 5, "bake ") == 0) {
            const size_t kind = first + 5;
            if (line.compare(kind, target.size(), target) == 0
                && (kind + target.size() == line.size()
                    || line[kind + target.size()] == ' '
                    || line[kind + target.size()] == '\t'
                    || line[kind + target.size()] == '#')) {
                const size_t comment = line.find('#', kind + target.size());
                const std::string keepComment = comment == std::string::npos
                    ? "" : " " + line.substr(comment);
                return source.substr(0, start + first) + replacement
                    + keepComment + source.substr(stop);
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    // No previous switch: create one in the header before the first pak.
    const size_t pak = source.find("[pak ");
    if (pak != std::string::npos) {
        const size_t line = source.rfind('\n', pak);
        const size_t at = line == std::string::npos ? 0 : line + 1;
        return source.substr(0, at) + replacement + "\n"
            + source.substr(at);
    }
    return source + (source.empty() || source.back() == '\n' ? "" : "\n")
        + replacement + "\n";
}

} // namespace

bool EditorApp::LoadProjectExportRecipe(const std::string& filename) {
    if (!RecipeNameValid(filename) || m_projectRoot.empty()) {
        m_projectExportStatus = "Choose a .tourrecipe filename in the project root.";
        return false;
    }
    const std::filesystem::path path =
        std::filesystem::path(m_projectRoot) / filename;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        m_projectExportStatus = "Cannot read " + path.generic_string();
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    if (text.size() >= m_projectExportRecipeText.size()) {
        m_projectExportStatus = "Recipe exceeds the editor's 64 KiB text limit.";
        return false;
    }
    m_projectExportRecipeText.fill('\0');
    std::memcpy(m_projectExportRecipeText.data(), text.data(), text.size());
    std::snprintf(m_projectExportRecipeNameBuf,
        sizeof(m_projectExportRecipeNameBuf), "%s", filename.c_str());
    m_projectExportRecipeName = filename;
    m_projectExportDirty = false;
    m_projectExportStatus = "Loaded " + filename;
    return true;
}

void EditorApp::OpenProjectExportWindow() {
    if (m_projectRoot.empty() || m_cookRunning || m_bakeRunning
        || m_dccPublishRunning) {
        Log("Export Project: open a project and finish active tasks first.");
        return;
    }
    // Opening the menu twice must never silently discard unsaved edits.
    if (m_showProjectExportWindow)
        return;
    m_projectExportRecipes.clear();
    std::error_code ec;
    namespace fs = std::filesystem;
    for (fs::directory_iterator it(m_projectRoot, ec), end;
        !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)
            && it->path().extension() == ".tourrecipe")
            m_projectExportRecipes.push_back(
                it->path().filename().generic_string());
    }
    std::sort(m_projectExportRecipes.begin(), m_projectExportRecipes.end());
    m_projectExportStatus.clear();
    m_projectExportDirty = false;
    m_projectExportRecipeText.fill('\0');
    m_showProjectExportWindow = true;
    if (!m_projectExportRecipes.empty()) {
        const auto preferred = std::find(m_projectExportRecipes.begin(),
            m_projectExportRecipes.end(), "game.tourrecipe");
        LoadProjectExportRecipe(preferred != m_projectExportRecipes.end()
            ? *preferred : m_projectExportRecipes.front());
        return;
    }
    const std::string entry = RecipeDefaultScene(m_projectRoot, m_tourPath);
    const std::string initial = DefaultRecipeText(m_projectName, entry);
    std::memcpy(m_projectExportRecipeText.data(), initial.data(),
        initial.size());
    std::snprintf(m_projectExportRecipeNameBuf,
        sizeof(m_projectExportRecipeNameBuf), "%s", "game.tourrecipe");
    m_projectExportRecipeName = m_projectExportRecipeNameBuf;
    m_projectExportStatus = entry.empty()
        ? "No .tour scenes found. Set 'entry' and the pak's 'scene' before saving."
        : "New recipe draft. Save to create game.tourrecipe.";
}

bool EditorApp::SaveProjectExportRecipe(std::string& outPath) {
    outPath.clear();
    const std::string name(m_projectExportRecipeNameBuf);
    if (m_projectRoot.empty() || !RecipeNameValid(name)) {
        m_projectExportStatus =
            "Recipe filename must end in .tourrecipe and contain no directories.";
        return false;
    }
    smvk::project::ProjectRecipe parsed;
    smvk::project::RecipeDiagnostic diagnostic;
    const std::string source(m_projectExportRecipeText.data());
    if (!smvk::project::ParseProjectRecipeText(source, parsed, diagnostic)) {
        m_projectExportStatus = diagnostic.Format(name);
        return false;
    }
    namespace fs = std::filesystem;
    const fs::path target = fs::path(m_projectRoot) / name;
    const fs::path temp = fs::path(target.generic_string() + ".editor.tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(source.data(), std::streamsize(source.size()));
        out.flush();
        if (!out) {
            m_projectExportStatus = "Cannot write " + temp.generic_string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        // Windows cannot rename over an existing destination.
        ec.clear();
        fs::copy_file(temp, target, fs::copy_options::overwrite_existing, ec);
        std::error_code cleanup;
        fs::remove(temp, cleanup);
        if (ec) {
            m_projectExportStatus = "Cannot save recipe: " + ec.message();
            return false;
        }
    }
    outPath = target.generic_string();
    m_projectExportRecipeName = name;
    m_projectExportDirty = false;
    if (std::find(m_projectExportRecipes.begin(),
        m_projectExportRecipes.end(), name) == m_projectExportRecipes.end()) {
        m_projectExportRecipes.push_back(name);
        std::sort(m_projectExportRecipes.begin(), m_projectExportRecipes.end());
    }
    m_projectExportStatus = "Saved " + outPath;
    return true;
}

void EditorApp::DrawProjectExportWindow() {
    if (!m_showProjectExportWindow)
        return;
    ImGui::SetNextWindowSize(ImVec2(790, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Export Project (.tourrecipe)",
            &m_showProjectExportWindow)) {
        ImGui::End();
        return;
    }
    ImGui::Text("Project: %s", m_projectName.c_str());
    ImGui::TextDisabled("Build output: %s",
        (std::filesystem::path(m_projectRoot) / "build").generic_string().c_str());
    ImGui::TextWrapped("Recipe v1 controls the entry scene, pak outputs, "
        "owned include globs, dependencies, and GI/cubemap/SDF bake settings. "
        "Edits are saved as a project-root .tourrecipe; the build uses the "
        "same recipe resolver and stage pipeline as Project.exe.");
    if (!m_projectExportRecipes.empty()
        && ImGui::BeginCombo("Existing recipes", m_projectExportRecipeNameBuf)) {
        for (const std::string& name : m_projectExportRecipes) {
            if (ImGui::Selectable(name.c_str(),
                    name == m_projectExportRecipeNameBuf)) {
                if (m_projectExportDirty)
                    m_projectExportStatus =
                        "Save your changes before switching recipes.";
                else
                    LoadProjectExportRecipe(name);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::InputText("Recipe filename", m_projectExportRecipeNameBuf,
            sizeof(m_projectExportRecipeNameBuf)))
        m_projectExportDirty = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload recipe")) {
        if (!LoadProjectExportRecipe(m_projectExportRecipeNameBuf))
            Log("Export Project: " + m_projectExportStatus);
    }
    // Keep the complete text as the source of truth; the quick bake
    // controls patch only their directives and preserve other lines.
    smvk::project::ProjectRecipe preview;
    smvk::project::RecipeDiagnostic previewError;
    if (smvk::project::ParseProjectRecipeText(
            m_projectExportRecipeText.data(), preview, previewError)) {
        ImGui::TextDisabled("Entry: %s | %zu package(s)",
            preview.entry.c_str(), preview.paks.size());
        auto bakeCheckbox = [&](const char* label,
                const char* key, bool on) {
            if (ImGui::Checkbox(label, &on)) {
                const std::string next = WithBakeFlag(
                    m_projectExportRecipeText.data(), key, on);
                if (next.size() < m_projectExportRecipeText.size()) {
                    m_projectExportRecipeText.fill('\0');
                    std::memcpy(m_projectExportRecipeText.data(),
                        next.data(), next.size());
                    m_projectExportDirty = true;
                }
            }
        };
        bakeCheckbox("Bake GI", "gi", preview.bakeGi);
        ImGui::SameLine();
        bakeCheckbox("Bake cubemaps", "cubemaps", preview.bakeCubemaps);
        ImGui::SameLine();
        bakeCheckbox("Bake SDF", "sdf", preview.bakeSdf);
    }
    ImGui::TextDisabled("recipe 1 | project | entry | bake gi/cubemaps/sdf | "
        "[pak id]: output, scene, include, depends, dependencies transitive");
    if (ImGui::InputTextMultiline("##tourrecipe",
            m_projectExportRecipeText.data(),
            m_projectExportRecipeText.size(),
            ImVec2(-1.0f, -130.0f), ImGuiInputTextFlags_AllowTabInput))
        m_projectExportDirty = true;
    const bool busy = m_cookRunning || m_bakeRunning || m_dccPublishRunning;
    if (m_tourDirty && m_world)
        ImGui::TextWrapped("Current scene has unsaved changes. Save it "
            "before Build Project (recipe builds use the saved .tour files).");
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Validate")) {
        smvk::project::ProjectRecipe recipe;
        smvk::project::RecipeDiagnostic diag;
        const std::string text(m_projectExportRecipeText.data());
        if (!smvk::project::ParseProjectRecipeText(text, recipe, diag)) {
            m_projectExportStatus = diag.Format(m_projectExportRecipeNameBuf);
        } else {
            std::string err;
            auto project = smvk::project::Project::Open(m_projectRoot, err);
            if (project.Valid()) {
                smvk::project::BuildPlan plan;
                if (smvk::project::ResolveRecipe(project, recipe, plan, err))
                    m_projectExportStatus = "Recipe valid: "
                        + std::to_string(plan.paks.size())
                        + " pak(s); entry " + plan.entryScene;
            }
            if (!err.empty())
                m_projectExportStatus = "Recipe validation: " + err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save recipe")) {
        std::string path;
        if (SaveProjectExportRecipe(path))
            Log("Export Project: recipe saved " + path);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_tourDirty && m_world);
    if (ImGui::Button("Build Project")) {
        std::string path;
        if (SaveProjectExportRecipe(path))
            StartProjectRecipeBuild(path,
                std::string(m_projectExportRecipeText.data()));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close"))
        m_showProjectExportWindow = false;
    if (!m_projectExportStatus.empty())
        ImGui::TextWrapped("%s", m_projectExportStatus.c_str());
    ImGui::End();
}

} // namespace editor
