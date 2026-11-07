// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Playable Asteroids HUD over Tourmaline's Sponza viewer. The renderer
// composites the engine's GameUiRenderer after ImGui, before UiComposite.
#include "AsteroidsGame.h"
#include "PixelArt.h"
#include "Runtime.h"

#include <Engine/App/input.h>
#include <Engine/Audio/audio.h>
#include <Engine/Renderer/GameUiRenderer.h>
#include <Engine/Renderer/WorldRenderer.h>
#include <SDL3/SDL_filesystem.h>
#include <filesystem>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <utility>

namespace {
constexpr float Pi = 3.14159265358979323846f;
using Color = glm::vec4;
constexpr Color Cyan{0.48f, 0.92f, 1.0f, 1.0f};
constexpr Color Amber{1.0f, 0.7f, 0.38f, 1.0f};
constexpr Color White{0.96f, 0.98f, 1.0f, 1.0f};

class AsteroidsDemo final : public app::GameBehaviour {
  public:
    void onStart(app::Context& ctx) override {
        // The canonical sponza_10 atrium camera from the tracked
        // benchmarks/scenarios.json; prevents a sky-only backdrop.
        // Host makes a named PCG32 stream from TOURMALINE_RNG_SEED.
        m_game.SetRandomStream(ctx.random().MakeStream("asteroids"));
        ctx.renderer().CameraPositioner().lookAt(
            {8.82f, 5.38f, -0.60f}, {7.832f, 5.362f, -0.448f}, {0.0f, 1.0f, 0.0f});
        const char* exeDirectory = SDL_GetBasePath();
        const auto source =
            std::filesystem::path(exeDirectory ? exeDirectory : ".") / "content/asteroids";
        if (auto* resourceManager = ctx.host().GetResourceManager()) {
            if (!m_art.Load(*resourceManager, source))
                std::fprintf(stderr, "[Asteroids] using vector fallback; font unavailable\n");
        }
        auto& audio = ctx.host().m_audio;
        if (!audio.IsInitialized())
            m_audioOwned = audio.Init();
        if (audio.IsInitialized()) {
            const std::array<const char*, 5> sounds{
                "music.wav", "laser.wav", "hit.wav", "wave.wav", "gameover.wav"};
            for (size_t i = 0; i < sounds.size(); ++i) {
                m_clips[i] = audio.LoadWav((source / "audio" / sounds[i]).string());
                if (!m_clips[i].valid())
                    std::fprintf(
                        stderr, "[Asteroids] audio %s: %s\n", sounds[i], audio.LastError().c_str());
            }
        }
        // Native GameUI host hook is now supplied by master (#85).
        // Keep sample-specific shader/sampling selection on GameUiRenderer
        // itself instead of extending the public WorldRenderer API.
        ctx.renderer().SetExtraGameUi([this](tour::GameUiRenderer& ui, smvk::Extent2D extent) {
            if (!m_uiConfigured) {
                ui.SetPixelArtSampling(true);
                ui.SetFragmentShaderPath("content/shaders/asteroids_gameui.frag.hlsl.spv");
                m_uiConfigured = true;
            }
            Draw(ui, extent);
        });
        std::puts("[Asteroids] Enter start/restart, arrows turn/thrust, Space fire");
    }

    void onStop(app::Context& ctx) override {
        // The renderer is still live at GameElement::onDetach. Release GPU
        // textures here, not in the game's destructor after host shutdown.
        ctx.renderer().SetExtraGameUi({});
        auto& audio = ctx.host().m_audio;
        if (m_musicVoice.valid())
            audio.Stop(m_musicVoice);
        for (auto clip : m_clips)
            if (clip.valid())
                audio.UnloadClip(clip);
        m_clips = {};
        if (m_audioOwned)
            audio.Shutdown();
        m_musicVoice = {};
        m_art.Unload();
    }

    void onUpdate(app::Context& ctx, float dt) override {
        const tour::input::Input& keys = ctx.host().m_input;
        asteroids::Input in{};
        in.left = keys.isKeyHeld(tour::input::Key::Left);
        in.right = keys.isKeyHeld(tour::input::Key::Right);
        in.thrust = keys.isKeyHeld(tour::input::Key::Up);
        in.fire = keys.isKeyHeld(tour::input::Key::Space);
        m_thrust = in.thrust;
        if (keys.isKeyPressed(tour::input::Key::Num5)) {
            m_pixelIndex = (m_pixelIndex + 1) % 4;
            std::printf("[Asteroids] pixel grid: %u\n", PixelGrid());
        }
        if (keys.isKeyPressed(tour::input::Key::Num6))
            m_scanlines = !m_scanlines;
        const auto wasState = m_game.state();
        const int oldScore = m_game.score(), oldLives = m_game.lives();
        const int oldWave = m_game.wave();
        const size_t oldBullets = m_game.bullets().size();
        m_startQueued |= keys.isKeyPressed(tour::input::Key::Enter);
        // Deterministic 120 Hz logic, bounded catch-up after window stalls.
        m_accumulator = std::min(m_accumulator + std::max(dt, 0.0f), 0.1f);
        bool first = true;
        while (m_accumulator >= 1.0f / 120.0f) {
            in.start = first && m_startQueued;
            m_game.Step(in, 1.0f / 120.0f);
            m_accumulator -= 1.0f / 120.0f;
            first = false;
        }
        if (!first) {
            m_startQueued = false;
            auto& audio = ctx.host().m_audio;
            if (audio.IsInitialized()) {
                const auto play = [&](size_t index, float volume) {
                    if (m_clips[index].valid())
                        audio.Play(m_clips[index], volume);
                };
                if (wasState != asteroids::Game::State::Playing &&
                    m_game.state() == asteroids::Game::State::Playing) {
                    if (m_musicVoice.valid())
                        audio.Stop(m_musicVoice);
                    if (m_clips[0].valid())
                        m_musicVoice = audio.Play(m_clips[0], .24f, true);
                }
                if (m_game.bullets().size() > oldBullets)
                    play(1, .22f);
                if (m_game.score() > oldScore || m_game.lives() < oldLives)
                    play(2, .27f);
                if (m_game.wave() > oldWave)
                    play(3, .27f);
                if (wasState == asteroids::Game::State::Playing &&
                    m_game.state() == asteroids::Game::State::GameOver) {
                    if (m_musicVoice.valid())
                        audio.Stop(m_musicVoice);
                    m_musicVoice = {};
                    play(4, .4f);
                }
            }
        }
    }

  private:
    asteroids::Game m_game;
    asteroids::PixelArt m_art;
    float m_accumulator = 0.0f;
    bool m_thrust = false;
    bool m_startQueued = false;
    std::array<tour::audio::ClipHandle, 5> m_clips{};
    tour::audio::VoiceHandle m_musicVoice{};
    bool m_audioOwned = false;
    int m_pixelIndex = 0;
    bool m_scanlines = true;
    bool m_uiConfigured = false;
    [[nodiscard]] uint32_t PixelGrid() const {
        return std::array<uint32_t, 4>{0, 2, 4, 8}[m_pixelIndex];
    }

    struct Canvas {
        glm::vec2 origin{};
        float scale = 1.0f;
        tour::UiRect2D bounds{};
        glm::vec2 At(asteroids::Vec2 p) const {
            return origin + glm::vec2(p.x, p.y) * scale;
        }
        void Dot(tour::GameUiRenderer& ui, asteroids::Vec2 p, float radius, Color color) const {
            const glm::vec2 center = At(p);
            const glm::vec2 half{radius, radius};
            ui.DrawRect({.bounds = {center - half, half * 2.0f}, .tint = color, .clip = bounds});
        }
    };

    static void Rect(tour::GameUiRenderer& ui,
                     glm::vec2 at,
                     glm::vec2 size,
                     Color color,
                     std::optional<tour::UiRect2D> clip = std::nullopt,
                     uint32_t effect = 0) {
        ui.DrawRect({.bounds = {at, size}, .tint = color, .clip = clip, .effect = effect});
    }

    static void Segment(tour::GameUiRenderer& ui,
                        const Canvas& c,
                        asteroids::Vec2 from,
                        asteroids::Vec2 to,
                        Color color) {
        const glm::vec2 a = c.At(from), b = c.At(to);
        const float distance = glm::length(b - a);
        const int count = std::max(1, int(std::ceil(distance / 2.0f)));
        for (int i = 0; i <= count; ++i) {
            const float t = float(i) / float(count);
            const glm::vec2 p = a + (b - a) * t;
            Rect(ui, p - glm::vec2(1.05f), glm::vec2(2.1f), color, c.bounds);
        }
    }
    static void DrawRock(tour::GameUiRenderer& ui,
                         const Canvas& c,
                         const asteroids::Asteroid& rock) {
        const float radius = rock.tier == 3 ? 5.0f : rock.tier == 2 ? 3.1f : 1.7f;
        std::array<asteroids::Vec2, 11> points{};
        for (int i = 0; i < 11; ++i) {
            const uint32_t hash = rock.shape ^ (uint32_t(i + 1) * 0x9e3779b9u);
            const float jag = 0.75f + float((hash ^ (hash >> 13)) & 255u) * (0.27f / 255.0f);
            const float a = rock.angle + float(i) * (2.0f * Pi / 11.0f);
            points[i] = {rock.position.x + std::cos(a) * radius * jag,
                         rock.position.y + std::sin(a) * radius * jag};
        }
        for (int i = 0; i < 11; ++i)
            Segment(ui, c, points[i], points[(i + 1) % 11], Amber);
    }
    static void DrawShip(tour::GameUiRenderer& ui,
                         const Canvas& c,
                         const asteroids::Ship& ship,
                         bool thrust) {
        const float cs = std::cos(ship.angle), sn = std::sin(ship.angle);
        const auto at = [&](float x, float y) -> asteroids::Vec2 {
            return {ship.position.x + cs * x - sn * y, ship.position.y + sn * x + cs * y};
        };
        const std::array<asteroids::Vec2, 4> hull{
            at(2.4f, 0), at(-1.7f, -1.35f), at(-1.0f, 0), at(-1.7f, 1.35f)};
        for (size_t i = 0; i < hull.size(); ++i)
            Segment(ui, c, hull[i], hull[(i + 1) % hull.size()], Cyan);
        if (thrust) {
            Segment(ui, c, at(-1.5f, -0.7f), at(-3.8f, 0), Amber);
            Segment(ui, c, at(-3.8f, 0), at(-1.5f, 0.7f), Amber);
        }
    }

    // One GPU-textured glyph per character from assets/font.png.
    void DrawText(tour::GameUiRenderer& ui,
                  glm::vec2 origin,
                  std::string_view text,
                  float pixel,
                  Color tint,
                  bool center = false) const {
        m_art.Text(ui, origin, text, pixel, tint, center);
    }

    void Draw(tour::GameUiRenderer& ui, smvk::Extent2D extent) const {
        if (extent.width < 80 || extent.height < 80)
            return;
        const glm::vec2 viewport{float(extent.width), float(extent.height)};
        const float scale = std::min((viewport.x - 32.0f) / asteroids::Width,
                                     (viewport.y - 32.0f) / asteroids::Height);
        const Canvas c{{(viewport.x - asteroids::Width * scale) * 0.5f,
                        (viewport.y - asteroids::Height * scale) * 0.5f},
                       scale,
                       {{(viewport.x - asteroids::Width * scale) * 0.5f,
                         (viewport.y - asteroids::Height * scale) * 0.5f},
                        {asteroids::Width * scale, asteroids::Height * scale}}};
        const glm::vec2 right = c.bounds.origin + c.bounds.size;
        Rect(ui,
             c.bounds.origin,
             c.bounds.size,
             {0.02f, 0.05f, 0.11f, 0.42f},
             std::nullopt,
             m_scanlines ? 1u : 0u);
        const Color border{0.54f, 0.81f, 0.9f, 0.6f};
        const float w = 2.0f;
        Rect(ui, c.bounds.origin, {c.bounds.size.x, w}, border);
        Rect(ui, {c.bounds.origin.x, right.y - w}, {c.bounds.size.x, w}, border);
        Rect(ui, c.bounds.origin, {w, c.bounds.size.y}, border);
        Rect(ui, {right.x - w, c.bounds.origin.y}, {w, c.bounds.size.y}, border);

        for (const auto& rock : m_game.rocks()) {
            if (m_art.HasSprites()) {
                const char* animation = rock.tier == 3   ? "large"
                                        : rock.tier == 2 ? "medium"
                                                         : "small";
                const float radius = rock.tier == 3 ? 5.0f : rock.tier == 2 ? 3.1f : 1.7f;
                m_art.SpriteFrame(ui,
                                  animation,
                                  m_game.seconds() + float(rock.shape % 8u) * .13f,
                                  c.At(rock.position),
                                  glm::vec2(2.5f * radius * c.scale),
                                  White,
                                  c.bounds,
                                  PixelGrid());
            } else
                DrawRock(ui, c, rock);
        }
        for (const auto& bullet : m_game.bullets()) {
            if (m_art.HasSprites())
                m_art.Sprite(ui,
                             56,
                             c.At(bullet.position),
                             glm::vec2(1.8f * c.scale),
                             White,
                             c.bounds,
                             PixelGrid());
            else
                c.Dot(ui, bullet.position, 2.2f, White);
        }
        for (const auto& spark : m_game.sparks()) {
            const Color tint{
                1.0f, 1.0f, 1.0f, std::clamp(spark.lifetime / spark.maxLifetime, 0.0f, 1.0f)};
            if (m_art.HasSprites())
                m_art.Sprite(
                    ui, 57, c.At(spark.position), glm::vec2(1.9f * c.scale), tint, c.bounds);
            else
                c.Dot(ui, spark.position, 1.7f, {1.0f, 0.69f, 0.38f, tint.a});
        }
        if (m_game.state() == asteroids::Game::State::Playing &&
            (m_game.ship().invulnerable <= 0.0f || int(m_game.seconds() * 9.0f) % 2 == 0)) {
            if (m_art.HasSprites()) {
                const float angle = m_game.ship().angle;
                int dir = int(std::floor(angle * (8.0f / Pi) + 0.5f));
                dir = (dir % 16 + 16) % 16;
                const uint32_t cell = uint32_t(dir + (m_thrust ? 16 : 0));
                m_art.Sprite(ui,
                             cell,
                             c.At(m_game.ship().position),
                             glm::vec2(5.8f * c.scale),
                             White,
                             c.bounds,
                             PixelGrid());
            } else
                DrawShip(ui, c, m_game.ship(), m_thrust);
        }

        const float fontPixel = std::clamp(scale * 0.22f, 1.3f, 3.0f);
        char stats[100]{};
        std::snprintf(stats,
                      sizeof(stats),
                      "SCORE %06d  WAVE %d  LIVES %d",
                      m_game.score(),
                      m_game.wave(),
                      m_game.lives());
        DrawText(ui, c.bounds.origin + glm::vec2(16, 12), stats, fontPixel, White);
        DrawText(ui,
                 {c.bounds.origin.x + 16, right.y - 19.0f},
                 "ARROWS TURN/THRUST  SPACE FIRE",
                 fontPixel * 0.75f,
                 White);
        char effects[80]{};
        std::snprintf(effects,
                      sizeof(effects),
                      "5 PIX %u  6 CRT %s",
                      PixelGrid(),
                      m_scanlines ? "ON" : "OFF");
        DrawText(
            ui,
            {right.x - 14.0f - 6.0f * fontPixel * float(std::char_traits<char>::length(effects)),
             c.bounds.origin.y + 13.0f},
            effects,
            fontPixel,
            White);

        const glm::vec2 center = c.bounds.origin + c.bounds.size * 0.5f;
        if (m_game.state() == asteroids::Game::State::Ready) {
            DrawText(ui, center + glm::vec2(0, -28), "ASTEROIDS", fontPixel * 2.0f, Cyan, true);
            DrawText(ui, center + glm::vec2(0, 14), "PRESS ENTER TO START", fontPixel, White, true);
        } else if (m_game.state() == asteroids::Game::State::GameOver) {
            DrawText(ui, center + glm::vec2(0, -22), "GAME OVER", fontPixel * 2.0f, Amber, true);
            DrawText(ui, center + glm::vec2(0, 14), "ENTER TO RESTART", fontPixel, White, true);
        } else if (m_game.waveDelay() > 0.0f) {
            DrawText(ui, center, "WAVE CLEARED", fontPixel, Cyan, true);
        }
    }
};
} // namespace

int main(int argc, const char* argv[]) {
    app::GameLaunchOptions options;
    options.defaultManifest = "content/paks/sponza_10.tapp";
    options.extraOwnsCamera = true; // arrow keys fly the ship, not the camera
    AsteroidsDemo game;
    return app::RunGame<app::ProjectRuntime::CompiledAssetDesc>(
        &game, argc, argv, nullptr, 0, 0, std::move(options));
}
