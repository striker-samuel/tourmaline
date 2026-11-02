// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Runtime-only test fixtures for WorldRenderer's Procedurals tab.
// No editor scene, asset database, source import or .tour is involved.
// Meshes/materials live in the World's dynamic cluster and are discarded
// with the running App session.

#include <Engine/Renderer/WorldRenderer.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/gfx_filter.h>
#include <Engine/App/cluster.h>
#include <Engine/Scene/scene.h>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace engine {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct MeshRecipe {
    smvk::StaticMeshUpload upload{};

    uint32_t Vertex(const glm::vec3& p, const glm::vec3& n,
        const glm::vec2& uv) {
        smvk::ImportStaticVertex v{};
        v.pos = p;
        v.normal = glm::normalize(n);
        v.tangent = glm::vec4(
            glm::abs(v.normal.y) < 0.98f
                ? glm::normalize(glm::cross(glm::vec3(0, 1, 0), v.normal))
                : glm::vec3(1, 0, 0), 1.0f);
        v.texCoord = uv;
        v.color = glm::vec4(1.0f);
        v.chartId = UINT32_MAX; // never baked into a lightmap
        const uint32_t index = uint32_t(upload.vertices.size());
        upload.vertices.push_back(v);
        return index;
    }

    void Triangle(uint32_t a, uint32_t b, uint32_t c) {
        const glm::vec3 e0 = upload.vertices[b].pos - upload.vertices[a].pos;
        const glm::vec3 e1 = upload.vertices[c].pos - upload.vertices[a].pos;
        if (glm::dot(glm::cross(e0, e1), upload.vertices[a].normal) < 0.0f)
            std::swap(b, c);
        upload.indices.insert(upload.indices.end(), {a, b, c});
    }

    void Quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        Triangle(a, b, c);
        Triangle(a, c, d);
    }

    smvk::StaticMeshUpload Finish() {
        if (!upload.vertices.empty()) {
            upload.aabbMin = upload.aabbMax = upload.vertices.front().pos;
            for (const auto& v : upload.vertices) {
                upload.aabbMin = glm::min(upload.aabbMin, v.pos);
                upload.aabbMax = glm::max(upload.aabbMax, v.pos);
            }
            upload.surfaces.push_back(
                smvk::Surface{0u, uint32_t(upload.indices.size()), 0});
        }
        return std::move(upload);
    }
};

void Sphere(MeshRecipe& mesh, const glm::vec3& origin,
    float radius, int slices, int stacks, bool inside = false) {
    const uint32_t first = uint32_t(mesh.upload.vertices.size());
    const uint32_t stride = uint32_t(slices + 1);
    for (int y = 0; y <= stacks; ++y) {
        const float v = float(y) / stacks;
        const float elevation = kPi * v;
        for (int x = 0; x <= slices; ++x) {
            const float u = float(x) / slices;
            const float azimuth = 2.0f * kPi * u;
            const glm::vec3 n(
                std::sin(elevation) * std::cos(azimuth),
                std::cos(elevation),
                std::sin(elevation) * std::sin(azimuth));
            mesh.Vertex(origin + n * radius, inside ? -n : n, {u, v});
        }
    }
    for (int y = 0; y < stacks; ++y)
        for (int x = 0; x < slices; ++x) {
            const uint32_t a = first + uint32_t(y) * stride + uint32_t(x);
            const uint32_t b = a + stride;
            if (y > 0)
                mesh.Triangle(a, a + 1, b);
            if (y + 1 < stacks)
                mesh.Triangle(a + 1, b + 1, b);
        }
}

void Face(MeshRecipe& mesh, const glm::vec3& center,
    const glm::vec3& tangent, const glm::vec3& bitangent,
    const glm::vec3& normal) {
    const uint32_t a = mesh.Vertex(center-tangent-bitangent, normal, {0, 0});
    const uint32_t b = mesh.Vertex(center+tangent-bitangent, normal, {1, 0});
    const uint32_t c = mesh.Vertex(center+tangent+bitangent, normal, {1, 1});
    const uint32_t d = mesh.Vertex(center-tangent+bitangent, normal, {0, 1});
    mesh.Quad(a, b, c, d);
}

void Box(MeshRecipe& mesh, float hx, float hy, float hz) {
    Face(mesh, {hx,0,0}, {0,hy,0}, {0,0,hz}, {1,0,0});
    Face(mesh, {-hx,0,0}, {0,hy,0}, {0,0,hz}, {-1,0,0});
    Face(mesh, {0,hy,0}, {hx,0,0}, {0,0,hz}, {0,1,0});
    Face(mesh, {0,-hy,0}, {hx,0,0}, {0,0,hz}, {0,-1,0});
    Face(mesh, {0,0,hz}, {hx,0,0}, {0,hy,0}, {0,0,1});
    Face(mesh, {0,0,-hz}, {hx,0,0}, {0,hy,0}, {0,0,-1});
}

void Torus(MeshRecipe& mesh, float size, int segments) {
    const int ring = std::max(8, segments / 2);
    const uint32_t first = uint32_t(mesh.upload.vertices.size());
    const uint32_t stride = uint32_t(ring + 1);
    for (int i = 0; i <= segments * 2; ++i) {
        const float u = 2.0f * kPi * float(i) / float(segments * 2);
        const glm::vec3 radial(std::cos(u), 0, std::sin(u));
        for (int j = 0; j <= ring; ++j) {
            const float v = 2.0f * kPi * float(j) / ring;
            const glm::vec3 n =
                radial * std::cos(v) + glm::vec3(0, std::sin(v), 0);
            mesh.Vertex(radial * (size * 0.35f + size * 0.15f * std::cos(v))
                + glm::vec3(0, size * 0.15f * std::sin(v), 0),
                n, {float(i) / (segments * 2), float(j) / ring});
        }
    }
    for (int i = 0; i < segments * 2; ++i)
        for (int j = 0; j < ring; ++j) {
            const uint32_t a = first + uint32_t(i) * stride + uint32_t(j);
            mesh.Quad(a, a + 1, a + stride + 1, a + stride);
        }
}

void WavePlane(MeshRecipe& mesh, float size, int resolution,
    float amplitude, float cycles) {
    const uint32_t stride = uint32_t(resolution + 1);
    const float omega = 2.0f * kPi * cycles / size;
    for (int z = 0; z <= resolution; ++z)
        for (int x = 0; x <= resolution; ++x) {
            const float u = float(x) / resolution;
            const float v = float(z) / resolution;
            const float px = (u - 0.5f) * size;
            const float pz = (v - 0.5f) * size;
            const float y = amplitude * 0.5f *
                (std::sin(omega * px) + std::cos(omega * pz * 0.8f));
            const float dx = amplitude * 0.5f * omega * std::cos(omega * px);
            const float dz = -amplitude * 0.4f * omega
                * std::sin(omega * pz * 0.8f);
            mesh.Vertex({px,y,pz},
                glm::normalize(glm::vec3(-dx,1.0f,-dz)), {u,v});
        }
    for (int z = 0; z < resolution; ++z)
        for (int x = 0; x < resolution; ++x) {
            const uint32_t a = uint32_t(z) * stride + uint32_t(x);
            mesh.Quad(a, a + 1, a + stride + 1, a + stride);
        }
}

void HairBundle(MeshRecipe& mesh, float size, int resolution) {
    const int strands = std::clamp(resolution, 12, 48);
    constexpr int sides = 8;
    constexpr int rows = 12;
    for (int strand = 0; strand < strands; ++strand) {
        const float a = 2.0f * kPi * float(strand) / strands;
        const glm::vec3 radial(std::cos(a),0,std::sin(a));
        const glm::vec3 binormal(-radial.z,0,radial.x);
        const uint32_t first = uint32_t(mesh.upload.vertices.size());
        const uint32_t stride = sides + 1;
        for (int y = 0; y <= rows; ++y) {
            const float t = float(y) / rows;
            const glm::vec3 center = radial * size * (0.2f + 0.38f*t*t)
                + binormal * size * (0.08f * std::sin(kPi*t))
                + glm::vec3(0, size*(t-0.5f), 0);
            const float r = size * (0.032f * (1.0f-t) + 0.002f);
            for (int j = 0; j <= sides; ++j) {
                const float v = 2.0f * kPi * float(j) / sides;
                const glm::vec3 n =
                    radial*std::cos(v) + binormal*std::sin(v);
                mesh.Vertex(center + r*n, n, {float(j)/sides, t});
            }
        }
        for (int y = 0; y < rows; ++y)
            for (int j = 0; j < sides; ++j) {
                const uint32_t v = first + uint32_t(y)*stride + uint32_t(j);
                mesh.Quad(v,v+1,v+stride+1,v+stride);
            }
    }
}

smvk::StaticMeshUpload MakeFixture(int shape, float size, int resolution,
    float thickness, float amplitude, float frequency) {
    MeshRecipe mesh;
    switch (shape) {
    case 0: Sphere(mesh, {}, size*0.5f, resolution*2, resolution); break;
    case 1: Box(mesh, size*0.5f, size*0.5f, size*0.5f); break;
    case 2: Torus(mesh, size, resolution); break;
    case 3: WavePlane(mesh, size, resolution, amplitude, frequency); break;
    case 4: Box(mesh, size*0.5f, thickness*0.5f, size*0.5f); break;
    case 5: HairBundle(mesh, size, resolution); break;
    case 6:
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x)
                Sphere(mesh, {x*size*0.85f,0,z*size*0.85f},
                    size*0.3f, resolution, std::max(8,resolution/2));
        break;
    case 7: // thickness test: front and inward-facing back shell
        Sphere(mesh, {}, size*0.5f, resolution*2, resolution);
        Sphere(mesh, {}, size*0.42f, resolution*2, resolution, true);
        break;
    }
    return mesh.Finish();
}

smvk::MaterialCreateInfo FixtureMaterial(int shape) {
    smvk::MaterialCreateInfo info{};
    info.metallicFactor = (shape == 2) ? 0.85f : 0.0f;
    info.roughnessFactor = (shape == 3 || shape == 7) ? 0.08f : 0.36f;
    switch (shape) {
    case 3: info.baseColorFactor = {0.12f,0.46f,0.72f,1}; break;
    case 4: info.baseColorFactor = {0.88f,0.44f,0.34f,1}; break;
    case 5: info.baseColorFactor = {0.38f,0.22f,0.12f,1}; break;
    case 7:
        info.baseColorFactor = {0.52f,0.79f,0.95f,0.25f};
        info.alphaMode = smvk::AlphaMode::Blend;
        info.doubleSided = true;
        info.castsShadows = false;
        break;
    default: info.baseColorFactor = {0.75f,0.75f,0.77f,1}; break;
    }
    return info;
}

} // namespace

// Queued by ImGui during the last frame. Rebuild only at the next frame
// boundary, before the rendergraph records any view of the new world.
void WorldRenderer::ApplyRuntimeProceduralRequest() {
    if (m_runtimeProcRequest == 0 || !m_worldOwned || !m_world
        || !m_context || !m_rendergraph) {
        m_runtimeProcRequest = 0;
        return;
    }
    const int request = std::exchange(m_runtimeProcRequest, 0);
    smvk::Cluster& dyn = m_world->EnsureDynamicCluster(m_context);

    if (request == 1) {
        const smvk::StaticMeshUpload upload = MakeFixture(
            std::clamp(m_runtimeProcShape,0,7),
            std::clamp(m_runtimeProcSize,0.2f,20.0f),
            std::clamp(m_runtimeProcResolution,8,64),
            std::clamp(m_runtimeProcThickness,0.02f,5.0f),
            std::clamp(m_runtimeProcWaveHeight,0.0f,3.0f),
            std::clamp(m_runtimeProcWaveFreq,0.25f,10.0f));
        if (upload.vertices.empty() || upload.indices.empty())
            return;
        const auto newMesh = dyn.UploadStaticMesh(upload);
        if (!newMesh.valid()) {
            std::fprintf(stderr,"[Procedurals] mesh upload failed\n");
            return;
        }
        const auto newMaterial =
            dyn.CreateMaterial(FixtureMaterial(m_runtimeProcShape));
        if (!newMaterial.valid()) {
            dyn.DestroyMesh(newMesh);
            std::fprintf(stderr,"[Procedurals] material creation failed\n");
            return;
        }
        // Retire previous fixture only after the new one has been built.
        if (m_runtimeProcNode != UINT32_MAX) {
            smvk::RetireNodeSlot(dyn.scene, m_runtimeProcNode);
        }
        const int node = smvk::addNode(dyn.scene, -1, 0);
        if (node < 0) {
            dyn.DestroyMesh(newMesh);
            dyn.DestroyMaterial(newMaterial);
            return;
        }
        m_runtimeProcNode = uint32_t(node);
        const glm::vec3 center = m_cameraPositioner.cameraPosition_
            + m_cameraPositioner.getForward() * m_runtimeProcDistance;
        dyn.scene.localTransform[size_t(node)] =
            glm::translate(glm::mat4(1.0f), center);
        dyn.scene.globalTransform[size_t(node)] =
            dyn.scene.localTransform[size_t(node)];
        smvk::SetMesh(dyn.scene, uint32_t(node), newMesh.index());
        smvk::SetMaterial(dyn.scene, uint32_t(node), newMaterial.index());
        m_runtimeProcMesh = newMesh;
        m_runtimeProcMaterial = newMaterial;
    } else if (request == 2) {
        if (m_runtimeProcNode == UINT32_MAX)
            return;
        smvk::RetireNodeSlot(dyn.scene, m_runtimeProcNode);
        m_runtimeProcNode = UINT32_MAX;
    }
    // Scene + render-item tables must be derived from the cluster
    // content, never patched directly (same rule as loaded pak scenes).
    m_world->ResolveAndFixup_clusters();
    if (m_gfxFilterAvailable) {
        smvk::gfx::ResetStaticRegistrations(m_gfxRenderer.State());
        m_gfxRenderer.RegisterWorldMeshes(*m_world);
        m_gfxRenderer.UploadStaticData(*m_context, *m_rendergraph);
    }
    m_context->FlushUploads();
    // Old mesh regions retire behind the GPU in-flight safety margin.
    // World resolved first so no new frame references these handles.
    if (request == 2 || m_runtimeProcNode != UINT32_MAX) {
        if (m_runtimeProcOldMesh.valid())
            dyn.DestroyMesh(m_runtimeProcOldMesh);
        if (m_runtimeProcOldMaterial.valid())
            dyn.DestroyMaterial(m_runtimeProcOldMaterial);
        m_runtimeProcOldMesh = {};
        m_runtimeProcOldMaterial = {};
    }
    if (request == 2) {
        m_runtimeProcMesh = {};
        m_runtimeProcMaterial = {};
    }
}

void WorldRenderer::DrawRuntimeProceduralsTab() {
    ImGui::TextWrapped("Runtime-only rendering fixtures. No .tour, "
        "project assets or editor entities are created.");
    ImGui::SeparatorText("Geometry");
    static constexpr const char* names =
        "Sphere (PBR)\0Cube (hard edges)\0Torus (specular)\0"
        "Wave surface (water)\0Thin slab (SSS)\0"
        "Strand bundle (hair)\0Nine-sphere grid\0Glass shell\0";
    ImGui::Combo("Fixture", &m_runtimeProcShape, names);
    ImGui::SliderFloat("Size", &m_runtimeProcSize, 0.2f, 20.0f, "%.2f m");
    ImGui::SliderInt("Resolution", &m_runtimeProcResolution, 8, 64);
    ImGui::SliderFloat("Spawn distance", &m_runtimeProcDistance, 2.0f, 30.0f, "%.1f m");
    if (m_runtimeProcShape == 4)
        ImGui::SliderFloat("Slab thickness", &m_runtimeProcThickness,
            0.02f, 5.0f, "%.2f m");
    if (m_runtimeProcShape == 3) {
        ImGui::SliderFloat("Wave height", &m_runtimeProcWaveHeight,
            0.0f, 3.0f, "%.2f m");
        ImGui::SliderFloat("Wave cycles", &m_runtimeProcWaveFreq,
            0.25f, 10.0f);
        ImGui::TextDisabled("Static geometry (no wave simulation).");
    }
    if (m_runtimeProcShape == 5)
        ImGui::TextDisabled("Solid strands, not alpha-tested hair cards.");
    const bool canSpawn = m_worldOwned && m_world;
    ImGui::BeginDisabled(!canSpawn);
    if (ImGui::Button("Spawn / replace fixture")) {
        m_runtimeProcOldMesh = m_runtimeProcMesh;
        m_runtimeProcOldMaterial = m_runtimeProcMaterial;
        m_runtimeProcRequest = 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove fixture") && m_runtimeProcNode != UINT32_MAX) {
        m_runtimeProcOldMesh = m_runtimeProcMesh;
        m_runtimeProcOldMaterial = m_runtimeProcMaterial;
        m_runtimeProcRequest = 2;
    }
    ImGui::EndDisabled();
    if (!canSpawn)
        ImGui::TextDisabled("Only available in the standalone App, not borrowed editor worlds.");

    ImGui::SeparatorText("Fixture material");
    if (canSpawn && m_runtimeProcMaterial.valid()) {
        smvk::Cluster& dyn = m_world->EnsureDynamicCluster(m_context);
        if (smvk::Material* mat = dyn.materials.get(m_runtimeProcMaterial)) {
            ImGui::ColorEdit3("Base color", &mat->values.diffuse.x);
            ImGui::SliderFloat("Metallic", &mat->values.params.y, 0, 1);
            ImGui::SliderFloat("Roughness", &mat->values.params.z, 0.02f, 1);
            ImGui::ColorEdit3("Emissive", &mat->values.emissiveFactor.x);
            if (mat->alphaMode == smvk::AlphaMode::Blend)
                ImGui::SliderFloat("Opacity", &mat->values.diffuse.w, 0.01f, 1);
            ImGui::TextDisabled("Material values update live on the runtime GPU table.");
        }
    } else {
        ImGui::TextDisabled("Spawn a fixture to edit its material.");
    }

    ImGui::SeparatorText("Rendering diagnostic");
    if (ImGui::Button("Lit"))
        m_debugView = ViewMode::Lit;
    ImGui::SameLine();
    if (ImGui::Button("SSS debug"))
        m_debugView = ViewMode::SssPreview;
    ImGui::SameLine();
    if (ImGui::Button("Hair debug"))
        m_debugView = ViewMode::HairPreview;
    if (ImGui::Button("Water debug"))
        m_debugView = ViewMode::WaterPreview;
    ImGui::TextDisabled("SSS/hair/water modes are existing renderer diagnostics, not physical shaders.");

    ImGui::SeparatorText("Light Crowd");
    ImGui::Checkbox("Enable light crowd", &m_lightDemo.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled(m_lightCrowdCount > 0 ? "(spawned)" : "(off)");
    ImGui::SliderInt("Count", &m_lightDemo.count, 0, 96);
    ImGui::SliderFloat3("Center", &m_lightDemo.center.x, -30.0f, 30.0f);
    ImGui::SliderFloat("Spiral radius (m)", &m_lightDemo.radius, 0.5f, 30.0f);
    ImGui::SliderFloat("Height (m)", &m_lightDemo.height, 0.0f, 12.0f);
    ImGui::SliderFloat("Bob (m)", &m_lightDemo.bob, 0.0f, 4.0f);
    ImGui::SliderFloat("Cutoff range (m)", &m_lightDemo.range, 0.5f, 20.0f);
    ImGui::SliderFloat("Intensity (cd)", &m_lightDemo.intensity, 0.0f, 200000.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sunlit scenes may need 20000+ cd to read clearly.");
    ImGui::SliderFloat("Orbit speed (rad/s)", &m_lightDemo.speed, 0.0f, 2.0f);
    ImGui::SliderFloat("Base hue", &m_lightDemo.baseHue, 0.0f, 1.0f);
    ImGui::SliderFloat("Hue spread", &m_lightDemo.hueSpread, 0.0f, 1.0f);
    ImGui::Checkbox("Cast shadows (crowd)", &m_lightDemo.castShadows);
    if (m_lightDemo.castShadows) {
        const int cap = std::min(int(smvk::kMaxPointShadowLights),
            std::max(m_lightDemo.count, 1));
        int shadows = std::min(m_lightDemo.shadowCount, cap);
        if (ImGui::SliderInt("Shadow casters", &shadows, 0, cap))
            m_lightDemo.shadowCount = shadows;
    }
}

} // namespace engine
