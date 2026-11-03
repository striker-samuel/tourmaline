// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "PixelArt.h"
// The sample owns its PNG decode entry points. Do not rely on STB
// implementation symbols exported from the engine's static libraries.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace asteroids {
namespace {
bool ReadSheet(const std::filesystem::path& path,
    std::string_view texture, uint32_t w, uint32_t h,
    size_t requiredCells, tour::asset::SpriteSet& out) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "[Asteroids] missing SpriteSet: %s\n",
            path.string().c_str());
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    std::string error;
    if (!tour::asset::ParseSpriteSetText(text, out, error)) {
        std::fprintf(stderr, "[Asteroids] SpriteSet: %s\n", error.c_str());
        return false;
    }
    if (out.texture != texture || out.width != w || out.height != h ||
        out.sprites.size() != requiredCells) {
        std::fprintf(stderr, "[Asteroids] unexpected SpriteSet atlas contract: %s\n",
            path.string().c_str());
        return false;
    }
    return true;
}
smvk::Holder<smvk::TextureHandle> ReadRgbaTexture(
    smvk::ResourceManager& ctx, const std::filesystem::path& file,
    uint32_t expectedWidth, uint32_t expectedHeight, const char* label) {
    const std::string path = file.string();
    int width = 0, height = 0, channels = 0;
    stbi_uc* rgba = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!rgba || width != int(expectedWidth) || height != int(expectedHeight)) {
        std::fprintf(stderr, "[Asteroids] missing/invalid %s (%s): %dx%d\n",
            label, path.c_str(), width, height);
        stbi_image_free(rgba);
        return {};
    }
    smvk::ImageCreateInfo ci{};
    ci.extent = {uint32_t(width), uint32_t(height), 1};
    ci.format = smvk::Format::RGBA8_UNorm;
    ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
    ci.mipLevels = 1;
    ci.name = label;
    auto texture = ctx.CreateTexture(ci, rgba);
    ctx.FlushUploads();
    stbi_image_free(rgba);
    return texture;
}
} // namespace

bool PixelArt::Load(smvk::ResourceManager& ctx,
    const std::filesystem::path& directory) {
    Unload();
    if (!ReadSheet(directory / "sprites.spriteset",
        "content/asteroids/sprites.png", 256, 256, 64, m_spriteSet)
        || !ReadSheet(directory / "font.spriteset",
            "content/asteroids/font.png", 128, 60, 96, m_fontSet))
        return false;
    m_sprites = ReadRgbaTexture(ctx, directory / "sprites.png",
        m_spriteSet.width, m_spriteSet.height, "asteroidsSprites");
    m_font = ReadRgbaTexture(ctx, directory / "font.png",
        m_fontSet.width, m_fontSet.height, "asteroidsFont");
    if (m_sprites.empty() || m_font.empty()) {
        std::fprintf(stderr, "[Asteroids] atlas texture upload failed\n");
        Unload();
        return false;
    }
    return true;
}
void PixelArt::Unload() {
    m_font = {};
    m_sprites = {};
    m_spriteSet = {};
    m_fontSet = {};
}
void PixelArt::Sprite(tour::GameUiRenderer& ui, uint32_t index,
    glm::vec2 center, glm::vec2 size, glm::vec4 tint,
    std::optional<tour::UiRect2D> clip, uint32_t pixelGrid) const {
    if (m_sprites.empty() || index >= m_spriteSet.sprites.size()) return;
    const auto& region = m_spriteSet.sprites[index];
    const auto uv = m_spriteSet.UV(region);
    const glm::vec2 inset{0.5f / float(m_spriteSet.width),
                          0.5f / float(m_spriteSet.height)};
    ui.DrawRect({.bounds = {center-size*0.5f,size}, .tint=tint,
        .texture=m_sprites,
        .uvMin={uv[0] + inset.x, uv[1] + inset.y},
        .uvMax={uv[2] - inset.x, uv[3] - inset.y}, .clip=clip,
        .effect=pixelGrid});
}
void PixelArt::SpriteFrame(tour::GameUiRenderer& ui,
    std::string_view animation, float seconds, glm::vec2 center,
    glm::vec2 size, glm::vec4 tint,
    std::optional<tour::UiRect2D> clip, uint32_t pixelGrid) const {
    const auto* frames = m_spriteSet.FindAnimation(animation);
    if (!frames) return;
    Sprite(ui, frames->FrameAt(seconds), center, size, tint, clip, pixelGrid);
}
void PixelArt::Text(tour::GameUiRenderer& ui, glm::vec2 position,
    std::string_view value, float pixelSize, glm::vec4 tint, bool center) const {
    if (m_font.empty() || pixelSize <= 0.0f) return;
    if (center) position.x -= 3.0f * pixelSize * float(value.size());
    const glm::vec2 glyphSize{5.0f*pixelSize,7.0f*pixelSize};
    for (const unsigned char ch : value) {
        if (ch >= 33u && ch <= 127u) {
            const auto& region = m_fontSet.sprites[unsigned(ch) - 32u];
            const auto uv = m_fontSet.UV(region);
            const float du = 1.0f / float(m_fontSet.width);
            const float dv = 1.0f / float(m_fontSet.height);
            // 1px cell padding; sample the 5x7 painted glyph only.
            ui.DrawRect({.bounds = {position,glyphSize}, .tint=tint,
                .texture=m_font,
                .uvMin={uv[0]+du,uv[1]+dv},
                .uvMax={uv[0]+6.0f*du,uv[1]+8.0f*dv}});
        }
        position.x += 6.0f*pixelSize;
    }
}
} // namespace asteroids
