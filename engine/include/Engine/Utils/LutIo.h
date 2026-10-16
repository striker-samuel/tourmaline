// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include <cstdint>
#include <vector>

namespace engine {

// PNG decode/encode wrappers for the artistic LUT workflow (stb-based).
// RGBA8, tightly packed, row-major top-down.
bool LutLoadPng(const char* path, std::vector<uint8_t>& rgba, int& w, int& h);
bool LutWritePng(const char* path, int w, int h, const uint8_t* rgba);

} // namespace engine
