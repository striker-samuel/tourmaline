// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Procedural rendering fixtures.  Write normal source assets and pass them
// through the existing OBJ -> .geometry -> scene-reference pipeline rather
// than inventing a second (non-cookable) editor-only mesh representation.

#include "editor_app.h"

#include <imgui.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <utility>
#include <vector>

namespace editor {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct FixtureVertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{};
};

struct FixtureMesh {
    std::vector<FixtureVertex> vertices;
    std::vector<std::array<uint32_t, 3>> triangles;

    uint32_t Vertex(glm::vec3 position, glm::vec3 normal, glm::vec2 uv) {
        const uint32_t index = uint32_t(vertices.size());
        vertices.push_back({position, normal, uv});
        return index;
    }

    // Consistently orient triangles toward the analytical outward normal.
    void Triangle(uint32_t a, uint32_t b, uint32_t c) {
        const glm::vec3 ab = vertices[b].position - vertices[a].position;
        const glm::vec3 ac = vertices[c].position - vertices[a].position;
        if (glm::dot(glm::cross(ab, ac), vertices[a].normal) < 0.0f)
            std::swap(b, c);
        triangles.push_back({a, b, c});
    }

    void Quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        Triangle(a, b, c);
        Triangle(a, c, d);
    }
};

void AppendSphere(FixtureMesh& mesh, glm::vec3 center, float radius,
    int latitude, int longitude) {
    const uint32_t first = uint32_t(mesh.vertices.size());
    for (int row = 0; row <= latitude; ++row) {
        const float v = float(row) / float(latitude);
        const float theta = kPi * v;
        for (int col = 0; col <= longitude; ++col) {
            const float u = float(col) / float(longitude);
            const float phi = 2.0f * kPi * u;
            const glm::vec3 normal(std::sin(theta) * std::cos(phi),
                std::cos(theta), std::sin(theta) * std::sin(phi));
            mesh.Vertex(center + radius * normal, normal, {u, 1.0f - v});
        }
    }
    const uint32_t stride = uint32_t(longitude + 1);
    for (int row = 0; row < latitude; ++row)
        for (int col = 0; col < longitude; ++col) {
            const uint32_t a = first + uint32_t(row) * stride + uint32_t(col);
            const uint32_t b = a + stride;
            // The missing triangle at each pole would have zero area.
            if (row > 0)
                mesh.Triangle(a, b, a + 1);
            if (row + 1 < latitude)
                mesh.Triangle(a + 1, b, b + 1);
        }
}

void AppendBoxFace(FixtureMesh& mesh, glm::vec3 center,
    glm::vec3 u, glm::vec3 v, glm::vec3 normal) {
    const uint32_t a = mesh.Vertex(center - u - v, normal, {0, 0});
    const uint32_t b = mesh.Vertex(center + u - v, normal, {1, 0});
    const uint32_t c = mesh.Vertex(center + u + v, normal, {1, 1});
    const uint32_t d = mesh.Vertex(center - u + v, normal, {0, 1});
    mesh.Quad(a, b, c, d);
}

void AppendBox(FixtureMesh& mesh, float halfX, float halfY, float halfZ) {
    AppendBoxFace(mesh, { halfX, 0, 0 }, {0, halfY, 0},
        {0, 0, halfZ}, {1, 0, 0});
    AppendBoxFace(mesh, {-halfX, 0, 0}, {0, halfY, 0},
        {0, 0, halfZ}, {-1, 0, 0});
    AppendBoxFace(mesh, {0, halfY, 0}, {halfX, 0, 0},
        {0, 0, halfZ}, {0, 1, 0});
    AppendBoxFace(mesh, {0, -halfY, 0}, {halfX, 0, 0},
        {0, 0, halfZ}, {0, -1, 0});
    AppendBoxFace(mesh, {0, 0, halfZ}, {halfX, 0, 0},
        {0, halfY, 0}, {0, 0, 1});
    AppendBoxFace(mesh, {0, 0, -halfZ}, {halfX, 0, 0},
        {0, halfY, 0}, {0, 0, -1});
}

void AppendTorus(FixtureMesh& mesh, float size, int resolution) {
    const int along = resolution * 2;
    const int around = std::max(8, resolution / 2);
    const uint32_t start = uint32_t(mesh.vertices.size());
    const float major = size * 0.36f;
    const float minor = size * 0.14f;
    for (int i = 0; i <= along; ++i) {
        const float u = 2.0f * kPi * float(i) / float(along);
        const glm::vec3 radial(std::cos(u), 0, std::sin(u));
        for (int j = 0; j <= around; ++j) {
            const float v = 2.0f * kPi * float(j) / float(around);
            const glm::vec3 normal = radial * std::cos(v)
                + glm::vec3(0, std::sin(v), 0);
            mesh.Vertex(radial * (major + minor * std::cos(v))
                + glm::vec3(0, minor * std::sin(v), 0),
                normal, {float(i) / along, float(j) / around});
        }
    }
    const uint32_t stride = uint32_t(around + 1);
    for (int i = 0; i < along; ++i)
        for (int j = 0; j < around; ++j) {
            const uint32_t a = start + uint32_t(i) * stride + uint32_t(j);
            mesh.Quad(a, a + 1, a + stride + 1, a + stride);
        }
}

void AppendWaterPlane(FixtureMesh& mesh, float size, int resolution,
    float amplitude, float frequency) {
    const uint32_t stride = uint32_t(resolution + 1);
    const float wave = frequency * 2.0f * kPi / size;
    for (int z = 0; z <= resolution; ++z)
        for (int x = 0; x <= resolution; ++x) {
            const float u = float(x) / resolution;
            const float v = float(z) / resolution;
            const float px = (u - 0.5f) * size;
            const float pz = (v - 0.5f) * size;
            const float y = amplitude * 0.5f *
                (std::sin(wave * px) + std::cos(wave * pz * 0.8f));
            const float dx = amplitude * 0.5f * wave * std::cos(wave * px);
            const float dz = -amplitude * 0.4f * wave *
                std::sin(wave * pz * 0.8f);
            const glm::vec3 normal =
                glm::normalize(glm::vec3(-dx, 1.0f, -dz));
            mesh.Vertex({px, y, pz}, normal, {u, v});
        }
    for (int z = 0; z < resolution; ++z)
        for (int x = 0; x < resolution; ++x) {
            const uint32_t a = uint32_t(z) * stride + uint32_t(x);
            mesh.Quad(a, a + 1, a + stride + 1, a + stride);
        }
}

void AppendHairBundles(FixtureMesh& mesh, float size, int resolution) {
    const int strands = std::clamp(resolution, 12, 64);
    constexpr int kSides = 8;
    constexpr int kRows = 14;
    for (int s = 0; s < strands; ++s) {
        const float angle = 2.0f * kPi * float(s) / strands;
        const glm::vec3 radial(std::cos(angle), 0.0f, std::sin(angle));
        const glm::vec3 tangent(-radial.z, 0.0f, radial.x);
        const uint32_t first = uint32_t(mesh.vertices.size());
        for (int row = 0; row <= kRows; ++row) {
            const float t = float(row) / kRows;
            const glm::vec3 center = radial * (size * (0.20f + 0.38f*t*t))
                + tangent * (size * 0.08f * std::sin(kPi * t))
                + glm::vec3(0, size * (t - 0.5f), 0);
            const float radius = size * (0.032f * (1.0f - t) + 0.002f);
            for (int side = 0; side <= kSides; ++side) {
                const float phi = 2.0f * kPi * float(side) / kSides;
                const glm::vec3 normal = radial * std::cos(phi)
                    + tangent * std::sin(phi);
                mesh.Vertex(center + radius * normal, normal,
                    {float(side) / kSides, t});
            }
        }
        const uint32_t stride = uint32_t(kSides + 1);
        for (int row = 0; row < kRows; ++row)
            for (int side = 0; side < kSides; ++side) {
                const uint32_t a = first + uint32_t(row) * stride
                    + uint32_t(side);
                mesh.Quad(a, a + 1, a + stride + 1, a + stride);
            }
    }
}

// Keep this a pure deterministic recipe. The generated OBJ is an ordinary
// authorable asset: after import it receives the same material, picking,
// cooking and scene-reference behavior as a user-imported model.
FixtureMesh MakeFixture(int shape, float size, int resolution,
    float thickness, float waveAmplitude, float waveFrequency) {
    FixtureMesh mesh;
    switch (shape) {
    case 0: // UV sphere: PBR, SSS and transmission comparison
        AppendSphere(mesh, {}, size * 0.5f, resolution, resolution * 2);
        break;
    case 1: // hard-edged cube
        AppendBox(mesh, size * 0.5f, size * 0.5f, size * 0.5f);
        break;
    case 2: // curved specular highlight fixture
        AppendTorus(mesh, size, resolution);
        break;
    case 3: // water (static waves; animation is a renderer concern)
        AppendWaterPlane(mesh, size, resolution, waveAmplitude, waveFrequency);
        break;
    case 4: // thin / thick SSS comparison
        AppendBox(mesh, size * 0.5f, thickness * 0.5f, size * 0.5f);
        break;
    case 5: // tube-based strand bundle, not transparency hair cards
        AppendHairBundles(mesh, size, resolution);
        break;
    case 6: // nine identical probes at different lighting angles
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x)
                AppendSphere(mesh, {float(x) * size * 0.85f, 0,
                    float(z) * size * 0.85f}, size * 0.3f,
                    std::max(8, resolution / 2), resolution);
        break;
    }
    return mesh;
}

bool WriteFixtureObj(const std::filesystem::path& objPath,
    const std::string& name, const FixtureMesh& mesh, int shape) {
    const std::filesystem::path mtlPath =
        objPath.parent_path() / (objPath.stem().string() + ".mtl");
    std::ofstream mtl(mtlPath, std::ios::binary | std::ios::trunc);
    if (!mtl)
        return false;
    constexpr std::array<std::array<float, 3>, 7> colors{{
        {{0.72f, 0.78f, 0.84f}}, // PBR sphere
        {{0.79f, 0.67f, 0.54f}}, // cube
        {{0.84f, 0.68f, 0.32f}}, // torus
        {{0.12f, 0.42f, 0.65f}}, // water
        {{0.88f, 0.48f, 0.39f}}, // SSS
        {{0.40f, 0.21f, 0.12f}}, // hair
        {{0.72f, 0.78f, 0.84f}}  // array
    }};
    const auto& col = colors[size_t(shape)];
    mtl << "newmtl FixtureMaterial\nKd " << col[0] << ' ' << col[1]
        << ' ' << col[2] << "\nPm 0.0\nPr "
        << (shape == 3 ? "0.08" : "0.38")
        << "\nNs 100\nillum 2\n";
    mtl.close();
    if (!mtl)
        return false;

    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    if (!obj)
        return false;
    obj << "# Tourmaline Procedurals: generated editable test fixture\n"
        << "mtllib " << mtlPath.filename().generic_string() << '\n'
        << "o " << name << "\nusemtl FixtureMaterial\n"
        << std::fixed << std::setprecision(7);
    for (const auto& v : mesh.vertices)
        obj << "v " << v.position.x << ' ' << v.position.y << ' '
            << v.position.z << '\n';
    for (const auto& v : mesh.vertices)
        obj << "vt " << v.uv.x << ' ' << v.uv.y << '\n';
    for (const auto& v : mesh.vertices)
        obj << "vn " << v.normal.x << ' ' << v.normal.y << ' '
            << v.normal.z << '\n';
    for (const auto& face : mesh.triangles) {
        obj << "f";
        for (const uint32_t index : face) {
            const uint32_t n = index + 1;
            obj << ' ' << n << '/' << n << '/' << n;
        }
        obj << '\n';
    }
    obj.flush();
    return bool(obj);
}

} // namespace

void EditorApp::GenerateProceduralMesh() {
    if (!m_world || m_gltfPath.empty() || m_projectRoot.empty()) {
        Log("Procedurals: open a project scene before creating a fixture.");
        return;
    }
    if (m_cookRunning) {
        Log("Procedurals: finish the current cook before importing a fixture.");
        return;
    }
    namespace fs = std::filesystem;
    const int shape = std::clamp(m_proceduralShape, 0, 6);
    static constexpr const char* names[] = {
        "PBR_Sphere", "PBR_Cube", "Specular_Torus", "Water_Surface",
        "SSS_Slab", "Hair_Strands", "Material_Grid"
    };
    const fs::path folder = fs::path(m_projectRoot) / "content"
        / "models" / "procedurals";
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        Log("Procedurals: cannot create models directory: " + ec.message());
        return;
    }
    fs::path objPath;
    std::string name;
    for (int i = 1; i <= 9999; ++i) {
        name = std::string(names[shape]) + "_" + std::to_string(i);
        objPath = folder / (name + ".obj");
        if (!fs::exists(objPath))
            break;
    }
    if (fs::exists(objPath)) {
        Log("Procedurals: no available fixture filename.");
        return;
    }
    const FixtureMesh mesh = MakeFixture(shape,
        std::clamp(m_proceduralSize, 0.2f, 20.0f),
        std::clamp(m_proceduralResolution, 8, 64),
        std::clamp(m_proceduralThickness, 0.02f, 5.0f),
        std::clamp(m_proceduralWaveAmplitude, 0.0f, 3.0f),
        std::clamp(m_proceduralWaveFrequency, 0.25f, 10.0f));
    if (mesh.vertices.empty() || mesh.triangles.empty()
        || !WriteFixtureObj(objPath, name, mesh, shape)) {
        Log("Procedurals: could not write the fixture OBJ/MTL.");
        return;
    }
    // This is the editor's canonical import route; an imported OBJ has
    // independent .material artifacts and stable asset GUIDs.
    const std::string geometry = ImportMeshArtifactFor(objPath.generic_string());
    if (geometry.empty()) {
        Log("Procedurals: fixture saved, but asset import failed: "
            + objPath.generic_string());
        return;
    }
    if (AddSceneReference(geometry, name, true)) {
        Log("Procedurals: placed '" + name
            + "'. Select its mesh or material in the editor to edit PBR.");
    } else {
        Log("Procedurals: asset imported but placement failed: " + geometry);
    }
}

void EditorApp::GenerateLightCrowd() {
    if (!m_world)
        return;
    const uint32_t parent = DefaultCreateParent();
    const glm::vec3 center = ActiveCam().target;
    const int count = std::clamp(m_lightCrowdCount, 1, 32);
    const int batch = ++m_lightCrowdSerial;
    for (int i = 0; i < count; ++i) {
        const float t = float(i) / count;
        const float angle = 2.0f * kPi * t;
        EditCmd c{};
        c.verb = EditVerb::CreateLight;
        c.node = UINT32_MAX;
        c.node2 = parent;
        c.afterText = "LightCrowd_" + std::to_string(batch) + "_"
            + std::to_string(i + 1);
        c.afterVec = center + glm::vec3(std::cos(angle) * m_lightCrowdRadius,
            m_lightCrowdHeight, std::sin(angle) * m_lightCrowdRadius);
        c.lightType = 1u; // dynamic point light
        c.lightColor = m_lightCrowdColored
            ? glm::vec3(0.5f + 0.5f * std::cos(angle),
                0.5f + 0.5f * std::cos(angle + 2.094f),
                0.5f + 0.5f * std::cos(angle + 4.189f))
            : glm::vec3(1.0f);
        c.lightIntensity = m_lightCrowdIntensity;
        c.lightRange = m_lightCrowdRange;
        c.lightShadow = m_lightCrowdShadows;
        c.lightStatic = false;
        Do(c); // Uses the regular undoable, tour-persisted create verb.
    }
}

void EditorApp::DrawProceduralsPanel() {
    ImGui::SeparatorText("Procedural geometry");
    ImGui::TextWrapped("Create real mesh assets for PBR, water, SSS, hair "
        "and lighting tests. Each fixture is saved in the project and "
        "placed through the normal importer.");
    ImGui::Combo("Fixture", &m_proceduralShape,
        "UV sphere (PBR / glass)\0Cube (hard surfaces)\0"
        "Torus (specular)\0Wave plane (water)\0"
        "Thin slab (SSS)\0Strand bundle (hair)\0"
        "Nine-sphere grid\0");
    ImGui::SliderFloat("Size", &m_proceduralSize, 0.2f, 20.0f, "%.2f m");
    ImGui::SliderInt("Resolution", &m_proceduralResolution, 8, 64);
    if (m_proceduralShape == 3) {
        ImGui::SliderFloat("Wave amplitude", &m_proceduralWaveAmplitude,
            0.0f, 3.0f, "%.2f m");
        ImGui::SliderFloat("Wave frequency", &m_proceduralWaveFrequency,
            0.25f, 10.0f, "%.2f");
        ImGui::TextDisabled("Static geometry; wave animation is not enabled.");
    }
    if (m_proceduralShape == 4)
        ImGui::SliderFloat("Slab thickness", &m_proceduralThickness,
            0.02f, 5.0f, "%.2f m");
    if (m_proceduralShape == 5)
        ImGui::TextDisabled("Solid tapered strands; no alpha cards.");
    const bool canCreate = m_world && !m_gltfPath.empty()
        && !m_projectRoot.empty() && !m_cookRunning;
    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button("Create mesh fixture"))
        GenerateProceduralMesh();
    ImGui::EndDisabled();
    if (!canCreate)
        ImGui::TextDisabled("Requires an open project scene and no active cook.");

    ImGui::SeparatorText("Light Crowd");
    ImGui::TextWrapped("Spawn a ring of dynamic point lights around the "
        "viewport target. Each light is individually undoable.");
    ImGui::SliderInt("Light count", &m_lightCrowdCount, 1, 32);
    ImGui::SliderFloat("Ring radius", &m_lightCrowdRadius, 0.5f, 30.0f, "%.1f m");
    ImGui::SliderFloat("Height", &m_lightCrowdHeight, -10.0f, 20.0f, "%.1f m");
    ImGui::SliderFloat("Intensity", &m_lightCrowdIntensity,
        10.0f, 5000.0f, "%.0f");
    ImGui::SliderFloat("Range", &m_lightCrowdRange, 1.0f, 100.0f, "%.1f m");
    ImGui::Checkbox("Colored lights", &m_lightCrowdColored);
    ImGui::Checkbox("Cast shadows", &m_lightCrowdShadows);
    ImGui::BeginDisabled(!m_world);
    if (ImGui::Button("Spawn light crowd"))
        GenerateLightCrowd();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Material editing");
    ImGui::TextWrapped("Select a generated mesh and edit its imported "
        "material below. This reuses the editor's existing material "
        "controls and renderer, not a separate preview shader.");
    if (ImGui::CollapsingHeader("Edit scene material",
            ImGuiTreeNodeFlags_DefaultOpen))
        DrawMaterialEditor();
    ImGui::TextDisabled("SSS, hair and water-specific shader parameters "
        "require their renderer material-model integration.");
}

} // namespace editor
