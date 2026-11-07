// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "editor_asset_browser_nav.h" // collection folders belong at root
#include "editor_asset_creation.h"
#include "editor_asset_dcc.h"     // DCC source link + export build stage
#include "editor_asset_inspect.h" // generic file/directory inspector
#include "editor_attr_ui.h"       // the attribute-widget stubs (attr::Bool/...)
#include "editor_brush_presets.h"
#include "modal_bindings.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/shape_asset.h>
#include <Engine/Profile/profile.h>

#include <Engine/Asset/spriteset.h>
#include <SDL3/SDL_filesystem.h> // SDL_GetBasePath (staged export scripts)
#include <SDL3/SDL_misc.h>       // SDL_OpenURL: launch the local asset history UI
#include <imgui.h>
#include <imgui_impl_vulkan.h> // ImGui_ImplVulkan_AddTexture (TextureThumb)
#include <imgui_internal.h>    // DockBuilder: initial workspace graph + user reset
#include <stb_image.h>         // stbi_load (AssetSidecarThumb; impl in editor_branding)

#include <ktx.h>

#include <cmath>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shellapi.h> // ShellExecuteA (reveal-in-explorer)
#include <windows.h>
#endif

namespace editor {
void EditorApp::DrawPhysicsBodySection(uint32_t node) {
    const attr::ScopedID componentId("physics_body");
    if (!m_world || node >= m_world->scene.hierarchy.size())
        return;
    auto* row = tour::scene::FindPhysicsBody(m_world->scene, node);
    if (!row)
        return;
    if (attr::ComponentHeader("Physics Body / Collider")) {
        EditCmd cmd{};
        cmd.verb = EditVerb::RemovePhysicsBodyVerb;
        cmd.node = node;
        Do(cmd);
        return;
    }
    ImGui::TextDisabled("Authored definition (simulation preview not yet available).");
    auto commit = [&](const char* key, double value, bool flag = false) {
        EditCmd cmd{};
        cmd.verb = EditVerb::SetPhysicsBodyAttr;
        cmd.node = node;
        cmd.physicsAttr = key;
        ReadNodePhysicsBodyAttr(node, key, cmd.beforeNum, cmd.beforeFlag);
        cmd.number = value;
        cmd.afterFlag = flag;
        Do(cmd);
    };
    int motion = int(row->body.motion);
    if (ImGui::Combo("Motion", &motion, "Static\0Dynamic\0Kinematic\0"))
        commit("motion", motion);
    int shape = int(row->body.shape);
    if (ImGui::Combo("Collider shape", &shape, "Box\0Sphere\0Capsule\0"))
        commit("shape", shape);
    auto scalar = [&](const char* label, const char* key, float initial, float lo, float hi) {
        float value = initial;
        if (ImGui::DragFloat(label, &value, 0.01f, lo, hi, "%.3f", ImGuiSliderFlags_AlwaysClamp))
            commit(key, std::clamp(value, lo, hi));
    };
    if (motion == 1)
        scalar("Mass (kg)", "mass", row->body.mass, 0.001f, 1000000.f);
    scalar("Friction", "friction", row->body.friction, 0.f, 1.f);
    scalar("Restitution", "restitution", row->body.restitution, 0.f, 1.f);
    if (shape == 0) {
        scalar("Half extent X (m)", "halfX", row->body.halfExtents.x, 0.001f, 10000.f);
        scalar("Half extent Y (m)", "halfY", row->body.halfExtents.y, 0.001f, 10000.f);
        scalar("Half extent Z (m)", "halfZ", row->body.halfExtents.z, 0.001f, 10000.f);
    } else {
        scalar("Radius (m)", "radius", row->body.radius, 0.001f, 10000.f);
        if (shape == 2)
            scalar("Cylinder height (m)", "height", row->body.height, 0.001f, 10000.f);
    }
    bool sensor = row->body.sensor;
    if (ImGui::Checkbox("Sensor (no contact response)", &sensor))
        commit("sensor", 0, sensor);
}

} // namespace editor
