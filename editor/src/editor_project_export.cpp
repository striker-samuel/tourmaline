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
    if (filename.empty() || filename.front() == '.' || filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos)
        return false;
    const fs::path p(filename);
    return p.filename() == p && p.extension() == ".tourrecipe";
}

std::string RecipeDefaultScene(const std::string& root, const std::string& currentTour) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::path(root).lexically_normal();
    if (!currentTour.empty()) {
        const fs::path tour(currentTour);
        const fs::path full = tour.is_absolute() ? tour : (base / tour);
        const fs::path rel = full.lexically_normal().lexically_relative(base);
        const std::string name = rel.generic_string();
        if (rel.extension() == ".tour" && name.rfind("content/", 0) == 0 &&
            fs::is_regular_file(full, ec))
            return name;
    }
    const fs::path scenes = base / "content" / "scenes";
    std::vector<std::string> candidates;
    for (fs::recursive_directory_iterator it(scenes, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".tour") {
            const std::string rel = it->path().lexically_relative(base).generic_string();
            candidates.push_back(rel);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.empty() ? "" : candidates.front();
}

std::string DefaultRecipeText(const std::string& projectName, const std::string& entry) {
    const std::string safeName =
        projectName.find('"') == std::string::npos ? projectName : "Project";
    const std::string name =
        safeName.find_first_of(" #\t") != std::string::npos ? "\"" + safeName + "\"" : safeName;
    const std::string scene = entry.empty() ? "content/scenes/main.tour" : entry;
    return "# Tourmaline project recipe v1\n"
           "# Edit this configuration, then Validate, Save or Build Project.\n"
           "# Paths are relative to the project root.\n"
           "recipe 1\nproject " +
           name + "\nentry " + scene +
           "\n\nbake gi off\nbake cubemaps off\nbake sdf on\n\n"
           "[pak world]\noutput game.pak\nscene " +
           scene + "\n";
}

// Update one bake switch without regenerating the entire .tourrecipe:
// other directives, package sections, blank lines and comments survive.
std::string WithBakeFlag(const std::string& source, const std::string& target, bool enabled) {
    const std::string replacement = "bake " + target + (enabled ? " on" : " off");
    size_t start = 0;
    while (start < source.size()) {
        const size_t end = source.find('\n', start);
        const size_t stop = end == std::string::npos ? source.size() : end;
        const std::string line = source.substr(start, stop - start);
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 5, "bake ") == 0) {
            const size_t kind = first + 5;
            if (line.compare(kind, target.size(), target) == 0 &&
                (kind + target.size() == line.size() || line[kind + target.size()] == ' ' ||
                 line[kind + target.size()] == '\t' || line[kind + target.size()] == '#')) {
                const size_t comment = line.find('#', kind + target.size());
                const std::string keepComment =
                    comment == std::string::npos ? "" : " " + line.substr(comment);
                return source.substr(0, start + first) + replacement + keepComment +
                       source.substr(stop);
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
        return source.substr(0, at) + replacement + "\n" + source.substr(at);
    }
    return source + (source.empty() || source.back() == '\n' ? "" : "\n") + replacement + "\n";
}


// Patch a single directive within a recipe header or one [pak] section.
// Edit only the selected line: unrecognized directives and comments remain intact.
std::string WithRecipeDirective(const std::string& source,
                                const std::string& pakId,
                                const std::string& key,
                                const std::string& value) {
    size_t sectionStart = 0;
    size_t sectionEnd = source.size();
    const size_t firstPak = source.find("[pak ");
    if (pakId.empty()) {
        if (firstPak != std::string::npos)
            sectionEnd = firstPak;
    } else {
        const std::string header = "[pak " + pakId + "]";
        const size_t at = source.find(header);
        if (at == std::string::npos)
            return source;
        const size_t lineEnd = source.find('\n', at);
        sectionStart = lineEnd == std::string::npos ? source.size() : lineEnd + 1;
        const size_t next = source.find("[pak ", sectionStart);
        if (next != std::string::npos)
            sectionEnd = next;
    }
    const std::string directive = key + " ";
    for (size_t at = sectionStart; at < sectionEnd;) {
        const size_t eol = source.find('\n', at);
        const size_t stop = eol == std::string::npos ? source.size() : eol;
        const size_t nonspace = source.find_first_not_of(" \t", at);
        if (nonspace != std::string::npos && nonspace < stop &&
            (source.compare(nonspace, directive.size(), directive) == 0 ||
             source.compare(nonspace, key.size(), key) == 0 &&
                 nonspace + key.size() == stop)) {
            const size_t comment = source.find('#', nonspace + key.size());
            const std::string suffix = comment != std::string::npos && comment < stop
                                           ? " " + source.substr(comment, stop - comment)
                                           : "";
            const size_t removeEnd = eol == std::string::npos ? stop : eol + 1;
            const std::string replacement =
                value.empty() ? "" : key + " " + value + suffix +
                                         (eol == std::string::npos ? "" : "\n");
            return source.substr(0, at) + replacement + source.substr(removeEnd);
        }
        if (eol == std::string::npos)
            break;
        at = eol + 1;
    }
    if (value.empty())
        return source;
    const size_t insertAt = sectionEnd;
    const std::string prefix = insertAt > 0 && source[insertAt - 1] != '\n' ? "\n" : "";
    return source.substr(0, insertAt) + prefix + key + " " + value + "\n" +
           source.substr(insertAt);
}

// Append repeated directives rather than replacing the first owned item.
std::string WithRecipeListItem(const std::string& source,
                               const std::string& pakId,
                               const std::string& key,
                               const std::string& value) {
    const size_t start = source.find("[pak " + pakId + "]");
    if (start == std::string::npos)
        return source;
    const size_t next = source.find("[pak ", start + 1);
    const size_t insertAt = next == std::string::npos ? source.size() : next;
    const std::string prefix = insertAt > 0 && source[insertAt - 1] != '\n' ? "\n" : "";
    return source.substr(0, insertAt) + prefix + key + " " + value + "\n" +
           source.substr(insertAt);
}

// Remove one repeated include/depends directive, without touching siblings.
std::string WithoutRecipeListItem(const std::string& source,
                                  const std::string& pakId,
                                  const std::string& key,
                                  size_t index) {
    const std::string header = "[pak " + pakId + "]";
    const size_t start = source.find(header);
    if (start == std::string::npos)
        return source;
    const size_t firstLine = source.find('\n', start);
    if (firstLine == std::string::npos)
        return source;
    const size_t end = source.find("[pak ", firstLine + 1);
    const size_t limit = end == std::string::npos ? source.size() : end;
    size_t seen = 0;
    for (size_t at = firstLine + 1; at < limit;) {
        const size_t eol = source.find('\n', at);
        const size_t stop = eol == std::string::npos ? source.size() : eol;
        const size_t first = source.find_first_not_of(" \t", at);
        if (first != std::string::npos && first < stop &&
            source.compare(first, key.size() + 1, key + " ") == 0) {
            if (seen++ == index)
                return source.substr(0, at) +
                       source.substr(eol == std::string::npos ? stop : eol + 1);
        }
        if (eol == std::string::npos)
            break;
        at = eol + 1;
    }
    return source;
}

std::string WithoutRecipePak(const std::string& source, const std::string& id) {
    const size_t start = source.find("[pak " + id + "]");
    if (start == std::string::npos)
        return source;
    const size_t next = source.find("[pak ", start + 1);
    return source.substr(0, start) +
           (next == std::string::npos ? "" : source.substr(next));
}

std::string RecipeValue(const std::string& value) {
    if (value.find_first_of(" \t#") == std::string::npos)
        return value;
    return "\"" + value + "\"";
}

} // namespace

bool EditorApp::LoadProjectExportRecipe(const std::string& filename) {
    if (!RecipeNameValid(filename) || m_projectRoot.empty()) {
        m_projectExportStatus = "Choose a .tourrecipe filename in the project root.";
        return false;
    }
    const std::filesystem::path path = std::filesystem::path(m_projectRoot) / filename;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        m_projectExportStatus = "Cannot read " + path.generic_string();
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= m_projectExportRecipeText.size()) {
        m_projectExportStatus = "Recipe exceeds the editor's 64 KiB text limit.";
        return false;
    }
    m_projectExportRecipeText.fill('\0');
    std::memcpy(m_projectExportRecipeText.data(), text.data(), text.size());
    std::snprintf(
        m_projectExportRecipeNameBuf, sizeof(m_projectExportRecipeNameBuf), "%s", filename.c_str());
    m_projectExportRecipeName = filename;
    m_projectExportDirty = false;
    m_projectExportStatus = "Loaded " + filename;
    return true;
}

void EditorApp::OpenProjectExportWindow() {
    if (m_projectRoot.empty() || m_cookRunning || m_bakeRunning || m_dccPublishRunning) {
        Log("Export Project: open a project and finish active tasks first.");
        return;
    }
    // Opening the menu twice must never silently discard unsaved edits.
    if (m_showProjectExportWindow)
        return;
    m_projectExportRecipes.clear();
    std::error_code ec;
    namespace fs = std::filesystem;
    for (fs::directory_iterator it(m_projectRoot, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".tourrecipe")
            m_projectExportRecipes.push_back(it->path().filename().generic_string());
    }
    std::sort(m_projectExportRecipes.begin(), m_projectExportRecipes.end());
    m_projectExportStatus.clear();
    m_projectExportDirty = false;
    m_projectExportRecipeText.fill('\0');
    m_showProjectExportWindow = true;
    if (!m_projectExportRecipes.empty()) {
        const auto preferred = std::find(
            m_projectExportRecipes.begin(), m_projectExportRecipes.end(), "game.tourrecipe");
        LoadProjectExportRecipe(preferred != m_projectExportRecipes.end()
                                    ? *preferred
                                    : m_projectExportRecipes.front());
        return;
    }
    const std::string entry = RecipeDefaultScene(m_projectRoot, m_tourPath);
    const std::string initial = DefaultRecipeText(m_projectName, entry);
    std::memcpy(m_projectExportRecipeText.data(), initial.data(), initial.size());
    std::snprintf(m_projectExportRecipeNameBuf,
                  sizeof(m_projectExportRecipeNameBuf),
                  "%s",
                  "game.tourrecipe");
    m_projectExportRecipeName = m_projectExportRecipeNameBuf;
    m_projectExportStatus =
        entry.empty() ? "No .tour scenes found. Set 'entry' and the pak's 'scene' before saving."
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
    if (std::find(m_projectExportRecipes.begin(), m_projectExportRecipes.end(), name) ==
        m_projectExportRecipes.end()) {
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
    if (!ImGui::Begin("Export Project (.tourrecipe)", &m_showProjectExportWindow)) {
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
    if (!m_projectExportRecipes.empty() &&
        ImGui::BeginCombo("Existing recipes", m_projectExportRecipeNameBuf)) {
        for (const std::string& name : m_projectExportRecipes) {
            if (ImGui::Selectable(name.c_str(), name == m_projectExportRecipeNameBuf)) {
                if (m_projectExportDirty)
                    m_projectExportStatus = "Save your changes before switching recipes.";
                else
                    LoadProjectExportRecipe(name);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::InputText(
            "Recipe filename", m_projectExportRecipeNameBuf, sizeof(m_projectExportRecipeNameBuf)))
        m_projectExportDirty = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload recipe")) {
        if (!LoadProjectExportRecipe(m_projectExportRecipeNameBuf))
            Log("Export Project: " + m_projectExportStatus);
    }
    // Both views edit the SAME canonical recipe buffer. A malformed raw draft
    // stays editable in Text mode; no form state may silently rewrite it.
    auto apply = [&](const std::string& next) {
        if (next == m_projectExportRecipeText.data())
            return;
        if (next.size() >= m_projectExportRecipeText.size()) {
            m_projectExportStatus = "Recipe exceeds the editor's 64 KiB text limit.";
            return;
        }
        m_projectExportRecipeText.fill('\0');
        std::memcpy(m_projectExportRecipeText.data(), next.data(), next.size());
        m_projectExportDirty = true;
    };
    const auto directiveInput = [&](const char* label,
                                    const char* id,
                                    const std::string& pak,
                                    const std::string& key,
                                    const std::string& current,
                                    bool allowEmpty = false) {
        char buf[1024]{};
        std::snprintf(buf, sizeof(buf), "%s", current.c_str());
        ImGui::PushID(id);
        if (ImGui::InputText(label, buf, sizeof(buf))) {
            const std::string value(buf);
            if (allowEmpty || !value.empty())
                apply(WithRecipeDirective(m_projectExportRecipeText.data(), pak, key,
                                          value.empty() ? "" : RecipeValue(value)));
        }
        ImGui::PopID();
    };
    smvk::project::ProjectRecipe preview;
    smvk::project::RecipeDiagnostic previewError;
    const bool parsed = smvk::project::ParseProjectRecipeText(
        m_projectExportRecipeText.data(), preview, previewError);

    if (ImGui::BeginTabBar("##recipeViews")) {
        if (ImGui::BeginTabItem("Configure")) {
            m_projectExportVisualMode = true;
            if (!parsed) {
                ImGui::TextWrapped("The recipe has a syntax error: %s",
                                   previewError.Format(m_projectExportRecipeNameBuf).c_str());
                ImGui::TextWrapped("Switch to Text to fix it. Your draft has not been changed.");
            } else {
                ImGui::TextDisabled("Edit project settings and package ownership below.");
                directiveInput("Project name", "project", "", "project", preview.projectName);
                directiveInput("Entry scene", "entry", "", "entry", preview.entry);
                ImGui::SeparatorText("Default baking");
                auto bake = [&](const char* label, const char* key, bool on) {
                    if (ImGui::Checkbox(label, &on))
                        apply(WithRecipeDirective(m_projectExportRecipeText.data(), "",
                                                  std::string("bake ") + key,
                                                  on ? "on" : "off"));
                };
                bake("GI", "gi", preview.bakeGi);
                ImGui::SameLine();
                bake("Cubemaps", "cubemaps", preview.bakeCubemaps);
                ImGui::SameLine();
                bake("SDF", "sdf", preview.bakeSdf);

                ImGui::SeparatorText("Packages");
                if (m_projectExportSelectedPak >= static_cast<int>(preview.paks.size()))
                    m_projectExportSelectedPak = std::max(0, int(preview.paks.size()) - 1);
                ImGui::BeginChild("##pakList", ImVec2(175, 255), ImGuiChildFlags_Borders);
                for (size_t i = 0; i < preview.paks.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(preview.paks[i].id.c_str(),
                                          int(i) == m_projectExportSelectedPak))
                        m_projectExportSelectedPak = static_cast<int>(i);
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginGroup();
                if (!preview.paks.empty()) {
                    const auto& pak = preview.paks[size_t(m_projectExportSelectedPak)];
                    ImGui::Text("Package: %s", pak.id.c_str());
                    directiveInput("Output (.pak)", "output", pak.id, "output", pak.output);
                    directiveInput("Owned scene", "scene", pak.id, "scene", pak.scene, true);
                    bool transitive = pak.dependenciesTransitive;
                    if (ImGui::Checkbox("Transitive dependencies", &transitive))
                        apply(WithRecipeDirective(m_projectExportRecipeText.data(), pak.id,
                                                  "dependencies",
                                                  transitive ? "transitive" : "none"));
                    ImGui::TextDisabled("Per-package bake overrides");
                    auto pakBake = [&](const char* label, const char* key,
                                       const std::optional<bool>& overrideValue) {
                        int selection = !overrideValue.has_value() ? 0 :
                                        (*overrideValue ? 1 : 2);
                        ImGui::PushID(key);
                        if (ImGui::Combo(label, &selection, "Inherit\0On\0Off\0")) {
                            apply(WithRecipeDirective(
                                m_projectExportRecipeText.data(), pak.id,
                                std::string("bake ") + key,
                                selection == 0 ? "" : selection == 1 ? "on" : "off"));
                        }
                        ImGui::PopID();
                    };
                    pakBake("GI override", "gi", pak.bakeGi);
                    pakBake("Cubemap override", "cubemaps", pak.bakeCubemaps);
                    ImGui::TextDisabled("Includes (project-relative globs)");
                    for (size_t i = 0; i < pak.includes.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TextUnformatted(pak.includes[i].c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove##include"))
                            apply(WithoutRecipeListItem(m_projectExportRecipeText.data(),
                                                        pak.id, "include", i));
                        ImGui::PopID();
                    }
                    ImGui::InputTextWithHint("##newInclude", "content/models/**",
                                             m_projectExportNewIncludeBuf,
                                             sizeof(m_projectExportNewIncludeBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Add include") && m_projectExportNewIncludeBuf[0]) {
                        apply(WithRecipeListItem(
                            m_projectExportRecipeText.data(), pak.id, "include",
                            RecipeValue(m_projectExportNewIncludeBuf)));
                        m_projectExportNewIncludeBuf[0] = '\0';
                    }
                    ImGui::TextDisabled("Depends on (pak IDs)");
                    for (size_t i = 0; i < pak.depends.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TextUnformatted(pak.depends[i].c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove##depends"))
                            apply(WithoutRecipeListItem(m_projectExportRecipeText.data(),
                                                        pak.id, "depends", i));
                        ImGui::PopID();
                    }
                    ImGui::InputTextWithHint("##newDepends", "shared",
                                             m_projectExportNewDependsBuf,
                                             sizeof(m_projectExportNewDependsBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Add dependency") && m_projectExportNewDependsBuf[0]) {
                        apply(WithRecipeListItem(
                            m_projectExportRecipeText.data(), pak.id, "depends",
                            RecipeValue(m_projectExportNewDependsBuf)));
                        m_projectExportNewDependsBuf[0] = '\0';
                    }
                    if (ImGui::Button("Remove package")) {
                        // Removing a package never implicitly rewires dependants.
                        apply(WithoutRecipePak(m_projectExportRecipeText.data(), pak.id));
                    }
                }
                ImGui::EndGroup();
                ImGui::InputTextWithHint("##newPak", "new-package-id",
                                         m_projectExportNewPakBuf,
                                         sizeof(m_projectExportNewPakBuf));
                ImGui::SameLine();
                if (ImGui::Button("Add package") && m_projectExportNewPakBuf[0]) {
                    const std::string id(m_projectExportNewPakBuf);
                    const bool safe = id.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
                                      std::string::npos;
                    if (!safe || preview.FindPak(id))
                        m_projectExportStatus = "Choose a unique package ID (letters, digits, - or _).";
                    else {
                        std::string next(m_projectExportRecipeText.data());
                        if (!next.empty() && next.back() != '\n')
                            next += '\n';
                        next += "\n[pak " + id + "]\noutput " + id + ".pak\n";
                        apply(next);
                        m_projectExportSelectedPak = static_cast<int>(preview.paks.size());
                        m_projectExportNewPakBuf[0] = '\0';
                    }
                }
                ImGui::TextDisabled("Advanced material channel/encoding directives: Text tab.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Text")) {
            m_projectExportVisualMode = false;
            ImGui::TextDisabled("Full .tourrecipe source. Comments and advanced directives are preserved.");
            if (ImGui::InputTextMultiline("##tourrecipe",
                                          m_projectExportRecipeText.data(),
                                          m_projectExportRecipeText.size(),
                                          ImVec2(-1.0f, 310.0f),
                                          ImGuiInputTextFlags_AllowTabInput))
                m_projectExportDirty = true;
            if (!parsed)
                ImGui::TextWrapped("%s",
                                   previewError.Format(m_projectExportRecipeNameBuf).c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
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
                    m_projectExportStatus = "Recipe valid: " + std::to_string(plan.paks.size()) +
                                            " pak(s); entry " + plan.entryScene;
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
            StartProjectRecipeBuild(path, std::string(m_projectExportRecipeText.data()));
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
