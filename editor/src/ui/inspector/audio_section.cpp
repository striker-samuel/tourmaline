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
void EditorApp::DrawAudioSection(uint32_t node) {
    const attr::ScopedID componentId("audio_source");
    // Authored audio source on this node (the emitter association):
    // the payload knobs + the SOUND SLOTS below — asset references,
    // never playing voices (the runtime audio manager owns playback;
    // clips are shared by many emitters). The emitter POSE is the
    // node's transform (gizmo) — the audio manager resolves the
    // GLOBAL transform at use, so parenting applies like meshes.
    // Undoable SetAudioAttr/AddAudioSound/RemoveAudioSound edits
    // persisted as the tabulated tour verbs (cooks replay them).
    if (!m_world || node >= m_world->scene.hierarchy.size())
        return;
    Scene& scene = m_world->scene;
    AudioSourceRow* it = FindAudioSource(scene, node);
    if (!it)
        return;
    ::SceneAudioSource& def = it->source;
    ImGui::Separator();
    ImGui::Text("Audio source: '%s'", getNodeName(scene, int(node)).c_str());
    ImGui::TextDisabled("emitter — pose = node transform (gizmo);"
                        "\nsounds are asset references, not playing voices");
    // The drag pattern (one coalesced SetAudioAttr per drag).
    auto beginAudioDrag = [&](const char* attr) {
        if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetAudioAttr ||
                                 m_pending.cmd.node != node || m_pending.cmd.audioAttr != attr))
            FinalizePending();
        if (!m_pending.active) {
            m_pending.active = true;
            m_pending.cmd = EditCmd{};
            m_pending.cmd.verb = EditVerb::SetAudioAttr;
            m_pending.cmd.node = node;
            m_pending.cmd.audioAttr = attr;
            ReadNodeAudioSourceAttr(node,
                                    attr,
                                    m_pending.cmd.beforeVec4,
                                    m_pending.cmd.beforeNum,
                                    m_pending.cmd.beforeFlag,
                                    m_pending.cmd.beforeText);
        }
    };
    auto endAudioDrag = [&](const char* attr) {
        if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
            m_pending.cmd.verb == EditVerb::SetAudioAttr && m_pending.cmd.node == node &&
            m_pending.cmd.audioAttr == attr)
            FinishDrag();
    };
    float vol = def.volume;
    if (attr::Float("Volume", &vol, 0.01f, 0.0f, 16.0f, "%.2f", "The emitter's master gain")) {
        beginAudioDrag("volume");
        WriteNodeAudioSourceAttr(node, "volume", glm::vec4(0), double(vol), false, {});
        m_pending.cmd.number = double(vol);
    }
    endAudioDrag("volume");
    float mind = def.minDistance;
    if (attr::Float("Min distance (m)",
                    &mind,
                    0.1f,
                    0.0f,
                    1000.0f,
                    "%.1f",
                    "Full-volume radius (the attenuation starts"
                    " past this)")) {
        beginAudioDrag("minDistance");
        WriteNodeAudioSourceAttr(node, "minDistance", glm::vec4(0), double(mind), false, {});
        m_pending.cmd.number = double(mind);
    }
    endAudioDrag("minDistance");
    float maxd = def.maxDistance;
    if (attr::Float("Max distance (m)",
                    &maxd,
                    0.5f,
                    0.01f,
                    10000.0f,
                    "%.1f",
                    "Silent radius (the attenuation ends here)")) {
        beginAudioDrag("maxDistance");
        WriteNodeAudioSourceAttr(node, "maxDistance", glm::vec4(0), double(maxd), false, {});
        m_pending.cmd.number = double(maxd);
    }
    endAudioDrag("maxDistance");
    // Immediate checkboxes (chips — one Do per click, exact inverse).
    {
        bool spatial = def.spatial;
        if (attr::Check("Spatial (3D)",
                        &spatial,
                        "Distance attenuation + stereo pan from the"
                        " listener (off = a 2D full-mix source)")) {
            EditCmd c{};
            c.verb = EditVerb::SetAudioAttr;
            c.node = node;
            c.audioAttr = "spatial";
            ReadNodeAudioSourceAttr(
                node, "spatial", c.beforeVec4, c.beforeNum, c.beforeFlag, c.beforeText);
            c.afterFlag = spatial;
            c.number = c.beforeNum;
            WriteNodeAudioSourceAttr(node, "spatial", glm::vec4(0), 0.0, spatial, {});
            Do(c);
        }
        bool pol = def.playOnLoad;
        if (attr::Check("Play on load",
                        &pol,
                        "Start the first slot's clip when the scene loads"
                        " (the runtime autoplay)")) {
            EditCmd c{};
            c.verb = EditVerb::SetAudioAttr;
            c.node = node;
            c.audioAttr = "playOnLoad";
            ReadNodeAudioSourceAttr(
                node, "playOnLoad", c.beforeVec4, c.beforeNum, c.beforeFlag, c.beforeText);
            c.afterFlag = pol;
            c.number = c.beforeNum;
            WriteNodeAudioSourceAttr(node, "playOnLoad", glm::vec4(0), 0.0, pol, {});
            Do(c);
        }
    }
    // ---- the sound slots (asset references, ordered) ----
    ImGui::TextDisabled("sounds");
    for (size_t si = 0; si < def.sounds.size(); ++si) {
        ImGui::PushID(int(si));
        const AudioSoundRef& slot = def.sounds[si];
        ImGui::Text("%s", slot.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            // Remove (the removed slot rides the command's
            // before-state — the exec captures it).
            EditCmd c{};
            c.verb = EditVerb::RemoveAudioSound;
            c.node = node;
            c.afterText = slot.name;
            Do(c);
            ImGui::PopID();
            break; // the row vector just mutated
        }
        // The asset path (an addAudioSound UPSERT on commit — the
        // same stable slot name, the new path).
        char path[256] = {};
        std::snprintf(path, sizeof(path), "%s", slot.assetPath.c_str());
        if (ImGui::InputText("path", path, sizeof(path), ImGuiInputTextFlags_EnterReturnsTrue)) {
            EditCmd c{};
            c.verb = EditVerb::AddAudioSound;
            c.node = node;
            c.afterText = slot.name;
            c.audioPath = path;
            c.number = double(slot.gain);
            c.afterFlag = slot.loop;
            Do(c);
        }
        float gain = slot.gain;
        if (attr::Float("gain", &gain, 0.01f, 0.0f, 16.0f, "%.2f", "Per-slot gain")) {
            EditCmd c{};
            c.verb = EditVerb::AddAudioSound;
            c.node = node;
            c.afterText = slot.name;
            c.audioPath = slot.assetPath;
            c.number = double(gain);
            c.afterFlag = slot.loop;
            Do(c);
            ImGui::PopID();
            break; // upsert re-ordering safety
        }
        bool loop = slot.loop;
        if (attr::Check("loop", &loop, "The slot's loop default")) {
            EditCmd c{};
            c.verb = EditVerb::AddAudioSound;
            c.node = node;
            c.afterText = slot.name;
            c.audioPath = slot.assetPath;
            c.number = double(slot.gain);
            c.afterFlag = loop;
            Do(c);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("+ Add slot")) {
        // A unique slot name over this source's live slots.
        std::string name = "Sound";
        for (int n = 2;; ++n) {
            bool taken = false;
            for (const AudioSoundRef& s : def.sounds)
                if (s.name == name) {
                    taken = true;
                    break;
                }
            if (!taken)
                break;
            name = "Sound" + std::to_string(n);
        }
        EditCmd c{};
        c.verb = EditVerb::AddAudioSound;
        c.node = node;
        c.afterText = name;
        c.audioPath = "content/audio/new.wav";
        c.number = 1.0;
        c.afterFlag = false;
        Do(c);
    }
}

} // namespace editor
