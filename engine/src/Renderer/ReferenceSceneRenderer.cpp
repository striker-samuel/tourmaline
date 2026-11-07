// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/ReferenceSceneRenderer.h>

#include <Engine/GI/bake.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace tour::renderer {
namespace {
constexpr float kPi = 3.14159265358979323846f;

bool Finite(const glm::vec3& x) {
    return std::isfinite(x.x) && std::isfinite(x.y) && std::isfinite(x.z);
}
float Filmic(float x) {
    x = std::max(x, 0.0f);
    return std::clamp(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}
uint8_t Srgb8(float linear) {
    const float v = Filmic(linear);
    const float s = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::clamp(std::lround(s * 255.0f), 0L, 255L));
}
uint64_t Hash64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27u)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31u);
}
} // namespace

std::vector<uint8_t> ReferenceImage::ToRgba8(float exposure) const {
    if (!(std::isfinite(exposure) && exposure >= 0.0f) ||
        linearRadiance.size() != size_t(width) * height)
        return {};
    std::vector<uint8_t> out(linearRadiance.size() * 4u);
    for (size_t i = 0; i < linearRadiance.size(); ++i) {
        for (size_t c = 0; c < 3; ++c) {
            const float f = linearRadiance[i][int(c)] * exposure;
            out[i * 4u + c] = Srgb8(std::isfinite(f) ? f : 0.0f);
        }
        out[i * 4u + 3] = 255;
    }
    return out;
}

bool ReferenceImage::WritePfm(const std::string& filename) const {
    if (width == 0 || height == 0 || linearRadiance.size() != size_t(width) * height)
        return false;
    std::ofstream out(filename, std::ios::binary);
    if (!out)
        return false;
    out << "PF\n" << width << " " << height << "\n-1.0\n";
    for (uint32_t y = height; y > 0; --y) {
        for (uint32_t x = 0; x < width; ++x) {
            const glm::vec3& rgb = linearRadiance[size_t(y - 1) * width + x];
            const float v[3] = {rgb.x, rgb.y, rgb.z};
            for (float component : v)
                out.write(reinterpret_cast<const char*>(&component), sizeof(float));
        }
    }
    return bool(out);
}

ReferenceImage ReferenceSceneRenderer::Render(const gi::GiTraceScene& scene,
                                              const ReferenceCamera& camera,
                                              const ReferenceRenderSettings& settings,
                                              const std::atomic_bool* cancel,
                                              std::atomic<uint32_t>* completedRows) {
    if (settings.width == 0 || settings.height == 0 || settings.width > 4096 ||
        settings.height > 4096 || settings.samplesPerPixel == 0 ||
        settings.samplesPerPixel > 4096 || settings.maxBounces > 16 ||
        !(camera.fovYDeg > 0.0f && camera.fovYDeg < 180.0f) || !std::isfinite(camera.fovYDeg) ||
        !std::isfinite(camera.orthoHalfHeight) || camera.orthoHalfHeight < 0.0f ||
        !Finite(camera.position) || !Finite(camera.forward) || !Finite(camera.up) ||
        glm::dot(camera.forward, camera.forward) < 1e-12f ||
        !(std::isfinite(settings.exposure) && settings.exposure >= 0.0f))
        throw std::invalid_argument("ReferenceSceneRenderer: invalid camera or render settings");

    if (completedRows)
        completedRows->store(0, std::memory_order_relaxed);
    const glm::vec3 forward = glm::normalize(camera.forward);
    glm::vec3 right = glm::cross(forward, camera.up);
    if (glm::dot(right, right) < 1e-12f) {
        const glm::vec3 fallback =
            std::abs(forward.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        right = glm::cross(forward, fallback);
    }
    right = glm::normalize(right);
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));
    const float aspect = float(settings.width) / float(settings.height);
    const float halfH = std::tan(camera.fovYDeg * kPi / 360.0f);
    const bool orthographic = camera.orthoHalfHeight > 0.0f;

    ReferenceImage result;
    result.width = settings.width;
    result.height = settings.height;
    result.linearRadiance.resize(size_t(settings.width) * settings.height, glm::vec3(0.0f));
    std::atomic<uint32_t> nextRow{0};
    const uint32_t workers = std::clamp(
        settings.threads ? settings.threads : std::thread::hardware_concurrency(), 1u, 64u);
    auto work = [&]() {
        while (true) {
            if (cancel && cancel->load(std::memory_order_relaxed))
                return;
            const uint32_t y = nextRow.fetch_add(1, std::memory_order_relaxed);
            if (y >= settings.height)
                return;
            for (uint32_t x = 0; x < settings.width; ++x) {
                glm::vec3 sum(0.0f);
                for (uint32_t sample = 0; sample < settings.samplesPerPixel; ++sample) {
                    if (cancel && cancel->load(std::memory_order_relaxed))
                        return;
                    // Independent per-pixel/per-sample seeds make output
                    // invariant under work-stealing and thread count.
                    const uint64_t pixel = uint64_t(y) * settings.width + x;
                    gi::Pcg32 rng(Hash64(uint64_t(settings.seed) ^ Hash64(pixel) ^ Hash64(sample)));
                    const float jx = settings.samplesPerPixel == 1 ? 0.5f : rng.Float();
                    const float jy = settings.samplesPerPixel == 1 ? 0.5f : rng.Float();
                    const float sx = (2.0f * (float(x) + jx) / settings.width - 1.0f) * aspect;
                    const float sy = 1.0f - 2.0f * (float(y) + jy) / settings.height;
                    const glm::vec3 origin =
                        orthographic ? camera.position + right * sx * camera.orthoHalfHeight +
                                           up * sy * camera.orthoHalfHeight
                                     : camera.position;
                    const glm::vec3 direction =
                        orthographic
                            ? forward
                            : glm::normalize(forward + right * sx * halfH + up * sy * halfH);
                    const glm::vec3 radiance = scene.TraceIncoming(
                        origin, direction, rng, settings.maxBounces, nullptr, nullptr, true);
                    if (Finite(radiance))
                        sum += glm::max(radiance, glm::vec3(0.0f));
                }
                result.linearRadiance[size_t(y) * settings.width + x] =
                    sum / float(settings.samplesPerPixel);
            }
            if (completedRows)
                completedRows->fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(workers > 0 ? workers - 1u : 0u);
    for (uint32_t i = 1; i < workers; ++i)
        threads.emplace_back(work);
    work();
    for (auto& worker : threads)
        worker.join();
    if (cancel && cancel->load(std::memory_order_relaxed))
        return {}; // Never expose partial frames.
    return result;
}
} // namespace tour::renderer
