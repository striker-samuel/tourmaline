#pragma once
// Private (engine-internal, NOT public API): background pak file read +
// parse + decompress, kicked before renderer init so it overlaps Vulkan
// init + shader loading. Consumed by PakApp.cpp's world source.
#include <Engine/Asset/pak.h>
#include <Engine/Flintbase/flintbase_backend.h>

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace smvk {

// Background pak preload result. The backend owns the image + the parsed
// reader (already decompressed); the world source consumes it on the render
// thread once the context exists. `backend` is null on any failure (error
// filled).
struct PakPreload {
	std::string scenePak; // display name (also the VFS mount name)
	std::string openPath; // resolved host path (empty = not found)
	std::shared_ptr<const std::vector<uint8_t>> image;
	std::shared_ptr<PakBackend> backend;
	std::string error;
};

// --pak accepts host paths AND vfs-style paths ("/content/..."): try as-is,
// then CWD-relative, then exe-relative. Empty when nothing exists.
std::string ResolvePakOpenPath(const std::string& scenePak);

std::future<PakPreload> KickPakPreload(const std::string& pakPath);

} // namespace smvk
