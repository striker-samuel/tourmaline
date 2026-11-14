// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/App/Element.h>
#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Renderer/WorldRenderer.h>
#include <memory>
#include <utility>
#include <vector>
namespace app {
// App-only procedural fixture lab. All mutation goes through the World.
class RuntimeProcedurals final : public engine::IAppElement {
  public:
    RuntimeProcedurals(tour::App& host,
                       engine::WorldRenderer& renderer,
                       std::shared_ptr<tour::World*> world)
        : m_host(host), m_renderer(renderer), m_world(std::move(world)) {}
    const char* ElementName() const override {
        return "RuntimeProcedurals";
    }
    void onDetach() override;
    void onUpdate(float) override;
    void onRender(engine::ElementFrame&) override {}
    void DrawRuntimeProceduralsTab();

  private:
    void ApplyRuntimeProceduralRequest();
    void ApplyRuntimeSpotRequest();
    void RetireResources();
    tour::World* World() const {
        return m_world ? *m_world : nullptr;
    }
    tour::App& m_host;
    engine::WorldRenderer& m_renderer;
    std::shared_ptr<tour::World*> m_world;
    int m_runtimeProcShape = 0;
    int m_runtimeProcResolution = 24;
    float m_runtimeProcSize = 2.0f;
    float m_runtimeProcDistance = 5.0f;
    float m_runtimeProcThickness = 0.2f;
    float m_runtimeProcWaveHeight = 0.2f;
    float m_runtimeProcWaveFreq = 3.0f;
    int m_runtimeProcRequest = 0;
    uint32_t m_runtimeProcNode = UINT32_MAX;
    smvk::ClusterStaticMeshHandle m_runtimeProcMesh{};
    smvk::ClusterMaterialHandle m_runtimeProcMaterial{};
    // ---- runtime spotlight (spawned from the camera; a real SceneLight
    // on the dynamic cluster, folded in by the resolve like the fixture) ----
    int m_runtimeSpotRequest = 0;                  // 1 = spawn, 2 = remove
    uint32_t m_runtimeSpotNode = UINT32_MAX;       // dynamic-cluster scene node
    smvk::ClusterLightHandle m_runtimeSpotLight{}; // pool handle (destroy)
    float m_spotIntensity = 20000.0f;
    float m_spotRange = 15.0f;
    float m_spotInnerDeg = 20.0f;
    float m_spotOuterDeg = 45.0f;
    glm::vec3 m_spotColor{1.0f, 0.92f, 0.82f};
    struct Retired {
        smvk::ClusterStaticMeshHandle mesh{};
        smvk::ClusterMaterialHandle material{};
    };
    std::vector<Retired> m_retired;
};
} // namespace app
