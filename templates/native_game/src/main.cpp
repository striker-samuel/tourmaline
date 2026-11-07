// SPDX-License-Identifier: MIT
// The smallest native Tourmaline game: app::RunGame + scene + GameUiRenderer.
// No SDL renderer, Vulkan calls, swapchain ownership, or custom graphics loop.
#include "Runtime.h"
#include "game.h"
#include "hud.h"
#include "project.h"
#include <Engine/App/app.h>
#include <Engine/App/input.h>
#include <Engine/Renderer/WorldRenderer.h>
#include <cstdio>

namespace {
class NativeGame final : public app::GameBehaviour {
  public:
    void onStart(app::Context& ctx) override {
        // The packaged scene is already loaded by RunGame. Only the camera
        // pose and HUD callback are game-owned.
        ctx.renderer().CameraPositioner().lookAt(
            {0.0f, 2.6f, 7.0f}, {0.0f, 0.65f, 0.0f}, {0.0f, 1.0f, 0.0f});
        // Keep the on-screen pointer available; the game owns the camera.
        ctx.host().SetRelativeMouseMode(false);
        ctx.renderer().SetExtraGameUi([this](tour::GameUiRenderer& ui, smvk::Extent2D extent) {
            m_uiSize = extent;
            starter::draw(ui, extent, m_state);
        });
        std::puts("[NativeGame] 1 OFFER, 2 UPGRADE, 3 DEFEND. Click floating squares.");
    }
    void onUpdate(app::Context& ctx, float dt) override {
        const auto& input = ctx.host().m_input;
        // The same Action is used for mouse, keyboard and CPU-only tests.
        if (input.isKeyPressed(tour::input::Key::Num1))
            m_state.apply(starter::Action::Offer);
        if (input.isKeyPressed(tour::input::Key::Num2))
            m_state.apply(starter::Action::Upgrade);
        if (input.isKeyPressed(tour::input::Key::Num3))
            m_state.apply(starter::Action::Defend);
        if (input.isMouseButtonPressed(tour::input::MouseButton::Left) && m_uiSize.width &&
            m_uiSize.height) {
            const auto buttons = starter::layout(m_uiSize);
            const glm::vec2 pos{float(input.getMouseX()), float(input.getMouseY())};
            if (buttons.offer.Contains(pos))
                m_state.apply(starter::Action::Offer);
            else if (buttons.upgrade.Contains(pos))
                m_state.apply(starter::Action::Upgrade);
            else if (buttons.defend.Contains(pos))
                m_state.apply(starter::Action::Defend);
        }
        m_state.tick(dt);
    }

  private:
    starter::State m_state{};
    smvk::Extent2D m_uiSize{};
};
} // namespace
int main(int argc, const char* argv[]) {
    NativeGame game;
    app::GameLaunchOptions options;
    // Disables FPS camera drive. The HUD stays mouse- and keyboard-focused.
    options.extraOwnsCamera = true;
    return app::RunGame(&game,
                        argc,
                        argv,
                        project::Assets.data(),
                        project::Assets.size(),
                        project::CatalogFingerprint,
                        std::move(options));
}
