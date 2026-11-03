// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Asset/spriteset.h>
#include <cassert>
#include <cmath>
#include <iostream>
using namespace tour::asset;
int main() {
    const char* valid = R"(spriteset 1
texture content/ui/characters.png
size 128 64
sprite button 0 0 32 32 pivot 0.25 0.75 border 4 5 6 7
sprite trimmed 32 0 16 14 pivot 0.5 1 trim 20 20 2 3
grid run 0 32 4 1 32 32 pivot 0.5 1
anim running 10 loop run_0 run_1 run_2 run_3
anim once 2 once trimmed button
)";
    SpriteSet sheet;
    std::string err;
    assert(ParseSpriteSetText(valid, sheet, err) && err.empty());
    assert(sheet.sprites.size() == 6);
    assert(sheet.animations.size() == 2);
    assert(sheet.Find("button")->border[3] == 7);
    assert(sheet.Find("trimmed")->sourceWidth == 20);
    assert(sheet.Find("trimmed")->trimY == 3);
    assert(sheet.Find("run_3")->rect.x == 96);
    assert(std::abs(sheet.UV(*sheet.Find("run_3"))[0] - .75f) < .0001f);
    assert(sheet.FindAnimation("running")->FrameAt(.35f) == 5);
    assert(sheet.FindAnimation("running")->FrameAt(.45f) == 2);
    assert(sheet.FindAnimation("once")->FrameAt(99) == 0);
    const auto originalCount = sheet.sprites.size();
    auto reject = [&](const char* text) {
        assert(!ParseSpriteSetText(text, sheet, err));
        assert(!err.empty());
        assert(sheet.sprites.size() == originalCount);
    };
    reject("spriteset 2\ntexture content/x.png\nsize 4 4\n");
    reject("spriteset 1\ntexture ../escape.png\nsize 4 4\n");
    reject("spriteset 1\ntexture content/x.png\nsize 4 4\nsprite x 3 0 2 2\n");
    reject("spriteset 1\ntexture content/x.png\nsize 4 4\nsprite x 0 0 2 2\nsprite x 2 0 2 2\n");
    reject("spriteset 1\ntexture content/x.png\nsize 4 4\nanim broken 12 loop missing\n");
    reject("spriteset 1\ntexture content/x.png\nsize 4 4\nsprite x 0 0 2 2 trim 2 2 1 0\n");
    reject("spriteset 1\ntexture content/x.png\nsize 4 4\nsprite x 0 0 2 2 border 2 0 2 0\n");
    reject("spriteset 1\ntexture content/x.png\nsize 4 4\ngrid x 0 0 3 1 2 2\n");
    std::cout << "sprite set tests passed\n";
}
