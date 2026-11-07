// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// SpriteSet metadata owns names/rects/animation, pixel textures stay host-owned.

#include <Engine/Asset/spriteset.h>
#include <Engine/Memory/handle.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/GameUiRenderer.h>
#include <filesystem>
#include <optional>
#include <string_view>

namespace asteroids {
class PixelArt {
  public:
    bool Load(smvk::ResourceManager& ctx, const std::filesystem::path& directory);
    void Unload();
    [[nodiscard]] bool HasSprites() const {
        return !m_sprites.empty();
    }
    [[nodiscard]] bool HasFont() const {
        return !m_font.empty();
    }

    // Sprite names/animations resolve through the first-class SpriteSet.
    void Sprite(tour::GameUiRenderer& ui,
                uint32_t index,
                glm::vec2 center,
                glm::vec2 size,
                glm::vec4 tint,
                std::optional<tour::UiRect2D> clip = std::nullopt,
                uint32_t pixelGrid = 0) const;
    void SpriteFrame(tour::GameUiRenderer& ui,
                     std::string_view animation,
                     float elapsedSeconds,
                     glm::vec2 center,
                     glm::vec2 size,
                     glm::vec4 tint,
                     std::optional<tour::UiRect2D> clip = std::nullopt,
                     uint32_t pixelGrid = 0) const;
    void Text(tour::GameUiRenderer& ui,
              glm::vec2 position,
              std::string_view value,
              float pixelSize,
              glm::vec4 tint,
              bool center = false) const;

  private:
    tour::asset::SpriteSet m_spriteSet;
    tour::asset::SpriteSet m_fontSet;
    smvk::Holder<smvk::TextureHandle> m_sprites{};
    smvk::Holder<smvk::TextureHandle> m_font{};
};
} // namespace asteroids
