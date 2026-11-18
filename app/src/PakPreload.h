// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Private (app-host-internal, NOT public API): background pak file read +
// parse (CRC-verified; LZ4 entries decompress lazily at first use inside
// the reader), kicked before renderer init so it overlaps Vulkan init +
// shader loading. Consumed by PakWorldLoader.cpp's world loading.
#include <Engine/Asset/pak.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace app {

// Background pak preload result. The backend owns the image + the parsed
// reader (compressed entries decompress on first read); the world loader
// consumes it on the render thread once the context exists. `backend` is
// null on any failure (error filled).
struct PakPreload {
    std::string scenePak; // display name (also the VFS mount name)
    std::string openPath; // resolved host path (empty = not found)
    std::shared_ptr<const tour::asset::PakImage> image;
    std::shared_ptr<smvk::PakBackend> backend;
    std::string error;
    // The camera the streaming planner seeds from (world space position +
    // forward direction). Defaults to the origin: a page streamer armed from
    // this preload starts with an unbiased (origin) camera until the renderer
    // feeds its live position each frame.
    glm::vec3 cameraPos{0.0f};
    glm::vec3 viewDir{0.0f};
};

// --pak accepts host paths AND vfs-style paths ("/content/..."): try as-is,
// then CWD-relative, then exe-relative. Empty when nothing exists.
std::string ResolvePakOpenPath(const std::string& scenePak);

std::future<PakPreload> KickPakPreload(const std::string& pakPath,
                                       glm::vec3 cameraPos = {},
                                       glm::vec3 viewDir = {});

} // namespace app
