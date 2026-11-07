// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Asset/spriteset.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

static bool Read(const std::string& path, tour::asset::SpriteSet& sprites) {
    std::ifstream file(path);
    if (!file)
        return false;
    const std::string content((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
    std::string err;
    if (!tour::asset::ParseSpriteSetText(content, sprites, err)) {
        std::cerr << path << ": " << err << "\n";
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    const std::string dir = argv[1];
    tour::asset::SpriteSet sprites, font;
    if (!Read(dir + "/sprites.spriteset", sprites) || !Read(dir + "/font.spriteset", font))
        return 2;
    const auto* ship = sprites.Find("ship_31");
    const auto* rock = sprites.Find("rock_0");
    const auto* animation = sprites.FindAnimation("large");
    if (!ship || ship->rect.x != 224 || ship->rect.y != 96 || !rock || rock->rect.y != 128 ||
        sprites.sprites.size() != 64 || !animation || animation->frames.size() != 8 ||
        !font.Find("glyph_95") || font.sprites.size() != 96)
        return 3;
    const auto uv = sprites.UV(*rock);
    if (std::abs(uv[1] - 0.5f) > .00001f || animation->FrameAt(.3f) != animation->frames[1])
        return 4;
    std::cout << "Asteroids SpriteSet grid and animation tests passed\n";
    return 0;
}
