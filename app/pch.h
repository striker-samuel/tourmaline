#pragma once

// ---------------------------------------------------------------------------
// Precompiled Header for the App module (world_renderer, App.exe, Editor).
// Includes the foundational Engine headers that are always kept stable.
// Keep this header minimal — adding new headers forces full PCH rebuild.
// ---------------------------------------------------------------------------

// C++ language & standard library (most stable, rarely changes).
// NOTE: keep this list stable. Adding/removing headers forces a full PCH
// rebuild, which is exactly the slow world_renderer.obj compile we avoid.
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Engine core types (minimal, stable — always present)
#include <Engine/core.h>

// Math (GLM) – header-only, very stable
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>

// Vulkan / core infrastructure (always available)
#include <Engine/Renderer/renderview.h>
#include <Engine/Renderer/texture_feedback.h>

// Memory / asset forward declarations
#include <Engine/Vfs/vfs.h>

// Profile (lightweight)
#include <Engine/Profile/profile.h>

// imgui (forward-compatible; full include pulled in .cpp if needed)
#include <imgui.h>

// Engine render graph minimal types (core only, keeps stable)
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/hdr.h>

// String / utility
#include <Engine/Utils/texture_loader.h>