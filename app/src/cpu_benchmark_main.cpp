// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// CPU-only synthetic scene stress: exercises the actual World draw-item
// iterator and renderer's CPU sphere/frustum helpers. No RHI or GPU init.
#include <Engine/App/world.h>
#include <Engine/Renderer/renderview.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;
using smvk::World;

int Integer(const char* str, int min, int max) {
    const std::string text(str);
    size_t pos = 0;
    long long value = 0;
    try { value = std::stoll(text, &pos); }
    catch (const std::exception&) { throw std::runtime_error("invalid integer: " + text); }
    if (pos != text.size() || value < min || value > max)
        throw std::runtime_error("out of range integer: " + text);
    return static_cast<int>(value);
}

struct Config {
    int instances = 32768;
    int frames = 120;
    int warmup = 10;
    int seed = 1;
    bool rotating = false;
};

double Ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}
struct FrameStats {
    double traversalMs = 0;
    double lodMs = 0;
    double boundsMs = 0;
    double lightmapMs = 0;
    double pushMs = 0;
    uint64_t processed = 0;
    uint64_t visible = 0;
    uint64_t accepted = 0;
    uint64_t checksum = 0;
};

FrameStats RunFrame(const World& world, const Config& config, int frame) {
    FrameStats stats;
    smvk::RenderView view{};
    // Axis-aligned conservative camera volume; rotation changes its center,
    // not the input geometry. Exactly the same CPU frustum helper as renderer.
    const float shift = config.rotating ? 80.0f * std::sin(float(frame) * 0.03f) : 0.0f;
    view.m_viewPlanes[0] = { 1, 0, 0, 100 - shift };
    view.m_viewPlanes[1] = {-1, 0, 0, 100 + shift };
    view.m_viewPlanes[2] = { 0, 1, 0, 60 };
    view.m_viewPlanes[3] = { 0,-1, 0, 60 };
    view.m_viewPlanes[4] = { 0, 0, 1, 100 };
    view.m_viewPlanes[5] = { 0, 0,-1, 100 };
    std::vector<uint32_t> pushed;
    pushed.reserve(4096);
    const auto start = Clock::now();
    const auto mask = smvk::IteratorFlagBit_Renderable |
                      smvk::IteratorFlagBit_Opaque |
                      smvk::IteratorFlagBit_StaticVertexFmt;
    world.forEachRenderItem(mask, [&](const World::RenderItem& item) {
        ++stats.processed;
        if (item.material == World::kInvalidIndex) return;
        const auto& model = world.scene.globalTransform[item.node];
        auto tick = Clock::now();
        const glm::vec3 position(model[3]);
        const float distance2 = glm::dot(position, position);
        const uint32_t lod = distance2 > 40000.0f ? 2u :
                             distance2 > 10000.0f ? 1u : 0u;
        stats.lodMs += Ms(Clock::now() - tick);

        tick = Clock::now();
        glm::vec3 center{};
        float radius = 0;
        smvk::MeshBoundsToWorld(model, {-1, -1, -1}, {1, 1, 1}, center, radius);
        const bool visible = smvk::RenderViewIntersectsSphere(view, center, radius);
        stats.boundsMs += Ms(Clock::now() - tick);
        if (!visible) return;
        ++stats.visible;

        tick = Clock::now();
        const uint32_t chart = item.mesh * 3u + lod;
        const uint32_t lightmap = (chart ^ item.material) & 255u;
        stats.lightmapMs += Ms(Clock::now() - tick);

        tick = Clock::now();
        if (pushed.size() < 4096) {
            pushed.push_back((item.mesh << 16) ^ (lightmap << 8) ^ item.node);
            ++stats.accepted;
        }
        stats.pushMs += Ms(Clock::now() - tick);
    });
    stats.traversalMs = Ms(Clock::now() - start);
    // Observable output prevents the entire workload from being optimized out.
    for (const auto value : pushed)
        stats.checksum = (stats.checksum * 1099511628211ull) ^ value;
    return stats;
}
}

int main(int argc, char** argv) {
    try {
        Config cfg;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
            const char* value = argv[++i];
            if (arg == "--instances") cfg.instances = Integer(value, 1, 1000000);
            else if (arg == "--frames") cfg.frames = Integer(value, 1, 100000);
            else if (arg == "--warmup") cfg.warmup = Integer(value, 0, 10000);
            else if (arg == "--seed") cfg.seed = Integer(value, 0, 1000000);
            else if (arg == "--camera") {
                const std::string mode = value;
                if (mode != "fixed" && mode != "sweep")
                    throw std::runtime_error("camera must be fixed or sweep");
                cfg.rotating = mode == "sweep";
            } else throw std::runtime_error("unknown flag: " + arg);
        }

        World world;
        world.drawItems.reserve(cfg.instances);
        world.itemFlags.reserve(cfg.instances);
        world.scene.globalTransform.reserve(cfg.instances);
        uint32_t state = static_cast<uint32_t>(cfg.seed) | 1u;
        auto random = [&]() {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        for (int i = 0; i < cfg.instances; ++i) {
            const float x = float(int(random() % 100000) - 50000) * 0.01f;
            const float y = float(int(random() % 2000) - 1000) * 0.01f;
            const float z = float(int(random() % 100000) - 50000) * 0.01f;
            glm::mat4 transform(1.0f);
            transform[3] = glm::vec4(x, y, z, 1.0f);
            world.scene.globalTransform.push_back(transform);
            World::RenderItem item{};
            item.node = static_cast<tour::scene::NodeId>(i);
            item.mesh = static_cast<uint32_t>(i % 128);
            item.material = static_cast<uint32_t>(i % 32);
            item.flags = smvk::IteratorFlagBit_Renderable |
                         smvk::IteratorFlagBit_Opaque |
                         smvk::IteratorFlagBit_StaticVertexFmt;
            world.drawItems.push_back(item);
            world.itemFlags.push_back(item.flags);
        }

        // Prime the real lazy World::ItemIndicesFor list outside measurement.
        (void)world.ItemIndicesFor(smvk::IteratorFlagBit_Renderable |
                                   smvk::IteratorFlagBit_Opaque |
                                   smvk::IteratorFlagBit_StaticVertexFmt);
        FrameStats total;
        for (int i = -cfg.warmup; i < cfg.frames; ++i) {
            const FrameStats frame = RunFrame(world, cfg, std::max(0, i));
            if (i < 0) continue;
            total.traversalMs += frame.traversalMs;
            total.lodMs += frame.lodMs;
            total.boundsMs += frame.boundsMs;
            total.lightmapMs += frame.lightmapMs;
            total.pushMs += frame.pushMs;
            total.processed += frame.processed;
            total.visible += frame.visible;
            total.accepted += frame.accepted;
            total.checksum ^= frame.checksum + static_cast<uint64_t>(i);
        }
        const double n = double(cfg.frames);
        const double residual = std::max(0.0, total.traversalMs -
            total.lodMs - total.boundsMs - total.lightmapMs - total.pushMs);
        std::cout << std::fixed << std::setprecision(6)
                  << "{\"mode\":\"cpu-only-synthetic\",\"instances\":" << cfg.instances
                  << ",\"frames\":" << cfg.frames << ",\"warmup\":" << cfg.warmup
                  << ",\"seed\":" << cfg.seed
                  << ",\"camera\":\"" << (cfg.rotating ? "sweep" : "fixed") << "\""
                  << ",\"frame_ms\":" << total.traversalMs / n
                  << ",\"lod_ms\":" << total.lodMs / n
                  << ",\"bounds_ms\":" << total.boundsMs / n
                  << ",\"lightmap_ms\":" << total.lightmapMs / n
                  << ",\"push_ms\":" << total.pushMs / n
                  << ",\"traversal_other_ms\":" << residual / n
                  << ",\"processed_per_frame\":" << total.processed / cfg.frames
                  << ",\"visible_per_frame\":" << total.visible / cfg.frames
                  << ",\"accepted_per_frame\":" << total.accepted / cfg.frames
                  << ",\"checksum\":" << total.checksum << "}\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "CpuBenchmark: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
