// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Dedicated material-lab host. Only the authored toon_cel node opts into
// ProjectLit; all other materials keep Tourmaline's production StandardLit.
#include "Host.h"

#include <Engine/App/Element.h>
#include <Engine/App/RunConfig.h>
#include <Engine/App/world.h>
#include <Engine/Utils/shader_library.h>

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace {
class ToonMaterialSelector final : public engine::IAppElement {
  public:
    explicit ToonMaterialSelector(std::shared_ptr<tour::World*> world)
        : m_world(std::move(world)) {}

    const char* ElementName() const override {
        return "MaterialLabToon";
    }

    bool onAttach() override {
        tour::World* world = m_world ? *m_world : nullptr;
        if (!world) {
            std::fprintf(stderr, "[MaterialLab] no loaded world for toon fixture\n");
            return false;
        }

        std::unordered_set<uint32_t> materials;
        world->forEachRenderItem(smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_Opaque,
                                 [&](const tour::World::RenderItem& item) {
                                     if (item.material == tour::World::kInvalidIndex)
                                         return;
                                     const std::string name = tour::scene::getNodeName(
                                         world->scene, static_cast<int>(item.node));
                                     if (name == "toon_cel")
                                         materials.insert(item.material);
                                 });

        if (materials.empty()) {
            std::fprintf(stderr, "[MaterialLab] toon_cel node not found in fixture\n");
            return false; // Never capture a mislabeled StandardLit baseline.
        }
        for (uint32_t material : materials) {
            if (!world->SetMaterialShaderFamily(material, smvk::MaterialShaderFamily::ProjectLit)) {
                std::fprintf(stderr, "[MaterialLab] cannot select toon material %u\n", material);
                return false;
            }
            std::printf("[MaterialLab] material %u: ProjectLit toon\n", material);
        }
        return true;
    }

    void onRender(engine::ElementFrame&) override {} // Shader-only effect.

  private:
    std::shared_ptr<tour::World*> m_world;
};
} // namespace

int main(int argc, const char* argv[]) {
    const engine::RunConfig run = engine::RunConfig::Parse();
    run.Log();
    engine::RunConfig::WarnUnknown();
    tour::shaders::Init();

    app::PakAppConfig cfg;
    cfg.run = run;
    cfg.extraElements.push_back(
        [](tour::App&,
           engine::WorldRenderer&,
           std::shared_ptr<tour::World*> world,
           const std::shared_ptr<app::ProjectRuntime>&) -> std::shared_ptr<engine::IAppElement> {
            return std::make_shared<ToonMaterialSelector>(std::move(world));
        });

    int exitCode = 0;
    if (!app::ParsePakAppArgs(argc, argv, cfg.cli, exitCode))
        return exitCode;
    return app::RunPakApp(std::move(cfg));
}
