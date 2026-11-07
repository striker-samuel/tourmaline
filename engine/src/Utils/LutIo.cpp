// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// STATIC stb linkage: the symbols are internal to this TU so the engine
// (linked into App.exe and the Editor) never collides with consumers
// that compile their own stb_image implementation (the editor's
// branding/logo path, lm_png).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <Engine/Utils/LutIo.h>
#include <climits>

namespace engine {

bool LutLoadPngMemory(const uint8_t* bytes,
                      size_t count,
                      std::vector<uint8_t>& rgba,
                      int& w,
                      int& h) {
    w = h = 0;
    if (!bytes || !count || count > static_cast<size_t>(INT_MAX))
        return false;
    int comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(count), &w, &h, &comp, 4);
    if (!pixels)
        return false;
    rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

bool LutWritePng(const char* path, int w, int h, const uint8_t* rgba) {
    return stbi_write_png(path, w, h, 4, rgba, w * 4) != 0;
}

} // namespace engine
