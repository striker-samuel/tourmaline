// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// test_env_volumes: Unreal-style env volumes (EnvVolume over the
// transform box). GPU-less: exact 3D coverage (ZoneCovers), priority
// resolution (ZoneAtPosition/ZonePriority), and tour replay of the
// envvol.n keys (parse + validate + look-defaulted priority).

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/tour.h>
#include <Engine/Rendergraph/envzones.h>
#include <Engine/Scene/scene.h>
#include <Engine/WorldPartition/WorldEnvTiles.h>
#include <Engine/WorldPartition/WorldEnvTileProvider.h>
#include <Engine/WorldPartition/env_tile_pak.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
    if (++g_checks, cond)
        std::cout << "PASS " << name << "\n";
    else {
        ++g_failures;
        std::cout << "FAIL " << name << "\n";
    }
}

// One-node zone: unit box scaled to half extents, placed at origin.
uint32_t MakeZone(::Scene& s, const glm::vec3& half) {
    const int n = addNode(s, -1, 0);
    if (int(s.globalTransform.size()) <= n)
        s.globalTransform.resize(size_t(n) + 1, glm::mat4(1.0f));
    s.globalTransform[size_t(n)] = glm::scale(glm::mat4(1.0f), half);
    tour::scene::SceneEnv e{};
    SetEnv(s, NodeId(n), e);
    return uint32_t(n);
}

// Create-or-mutate an env-volume row (the old volume-map operator[]
// semantics: default-construct when absent, apply the field
// write, store back through the table mutator).
template <typename Fn> void MutateEnvVolume(::Scene& s, uint32_t node, Fn&& fn) {
    EnvVolume v{};
    if (const EnvVolumeRow* row = FindEnvVolume(s, node))
        v = row->volume;
    fn(v);
    SetEnvVolume(s, node, v);
}

} // namespace

int main() {
    // Half-spaces: box (10,10,10) at origin + single Y faces.
    {
        ::Scene s;
        const uint32_t z = MakeZone(s, glm::vec3(10, 10, 10));
        MutateEnvVolume(s, z, [](EnvVolume& v) {
            v.infMask = kEnvInfNegY; // covers y <= +10
        });
        Check(ZoneCovers(s, z, glm::vec3(0, -100, 0)), "envvol.neginf-covers-below");
        Check(!ZoneCovers(s, z, glm::vec3(0, 100, 0)), "envvol.neginf-clips-above");
        Check(ZoneCovers(s, z, glm::vec3(5, 0, -5)), "envvol.neginf-inside-box");
        Check(!ZoneCovers(s, z, glm::vec3(50, 0, 0)), "envvol.neginf-clips-x");
    }
    {
        ::Scene s;
        const uint32_t z = MakeZone(s, glm::vec3(10, 2, 10));
        MutateEnvVolume(s, z, [](EnvVolume& v) {
            v.infMask = kEnvInfPosY; // covers y >= -2
        });
        Check(ZoneCovers(s, z, glm::vec3(0, 100, 0)), "envvol.posinf-covers-above");
        Check(!ZoneCovers(s, z, glm::vec3(0, -100, 0)), "envvol.posinf-clips-below");
    }
    // All-inf = infinite everywhere; disabled = nothing.
    {
        ::Scene s;
        const uint32_t z = MakeZone(s, glm::vec3(10, 10, 10));
        MutateEnvVolume(s, z, [](EnvVolume& v) { v.infMask = 63u; });
        Check(ZoneCovers(s, z, glm::vec3(1e6f, -1e6f, 1e6f)), "envvol.allinf-covers");
        MutateEnvVolume(s, z, [](EnvVolume& v) { v.enabled = false; });
        Check(!ZoneCovers(s, z, glm::vec3(0, 0, 0)), "envvol.disabled-covers-nothing");
        Check(ZoneAtPosition(s, glm::vec3(0, 0, 0)) == UINT32_MAX, "envvol.disabled-skipped");
    }
    // Legacy INF scale still unbounds the whole axis (regression).
    {
        ::Scene s;
        const uint32_t z = MakeZone(s, glm::vec3(10, 10, 10));
        glm::mat4 g = s.globalTransform[z];
        g[1] = glm::vec4(std::numeric_limits<float>::infinity(), 0, 0, 0);
        s.globalTransform[z] = g;
        Check(ZoneCovers(s, z, glm::vec3(0, 1e6f, 0)), "envvol.legacy-inf-slab");
    }
    // Priority: volume entry beats the look; absent entry falls back.
    {
        ::Scene s;
        const uint32_t a = MakeZone(s, glm::vec3(100, 100, 100));
        const uint32_t b = MakeZone(s, glm::vec3(10, 10, 10));
        if (EnvRow* row = FindEnv(s, a))
            row->environment.zonePriority = 5.0f;
        if (EnvRow* row = FindEnv(s, b))
            row->environment.zonePriority = 1.0f;
        MutateEnvVolume(s, b, [](EnvVolume& v) { v.priority = 9.0f; });
        const EnvRow* rowA = FindEnv(s, a);
        const EnvRow* rowB = FindEnv(s, b);
        Check(rowA && ZonePriority(s, a, rowA->environment) == 5.0f, "envvol.priority-fallback");
        Check(rowB && ZonePriority(s, b, rowB->environment) == 9.0f, "envvol.priority-volume-wins");
        Check(ZoneAtPosition(s, glm::vec3(0, 0, 0)) == b, "envvol.resolve-volume-priority");
    }
    // Tour replay: envvol.n keys parse, validate, and default priority
    // from the look; dangling nodes drop; volumes-only docs leave the
    // look unauthored.
    {
        auto cluster = std::make_unique<smvk::Cluster>(nullptr);
        cluster->SetOfflineMode(true);
        const int root = addNode(cluster->scene, -1, 0);
        const int n1 = addNode(cluster->scene, root, 0);
        (void)n1;
        smvk::tour::Doc doc;
        doc.editorSettings.emplace_back("env.n.0.node", "1");
        doc.editorSettings.emplace_back("env.n.0.zonePriority", "5");
        doc.editorSettings.emplace_back("envvol.n.0.node", "1");
        doc.editorSettings.emplace_back("envvol.n.0.mask", "12");
        doc.editorSettings.emplace_back("envvol.n.0.weight", "0.5");
        doc.editorSettings.emplace_back("envvol.n.0.radius", "4");
        doc.editorSettings.emplace_back("envvol.n.0.enabled", "1");
        doc.editorSettings.emplace_back("envvol.n.1.node", "99");
        doc.editorSettings.emplace_back("envvol.n.1.mask", "63");
        std::vector<std::string> warns;
        std::string err;
        Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns, &err), "envvol.replay-ok");
        const EnvVolumeRow* it = FindEnvVolume(cluster->scene, 1);
        Check(it != nullptr, "envvol.replay-stored");
        if (it != nullptr) {
            Check(it->volume.infMask == 12u, "envvol.replay-mask");
            Check(it->volume.priority == 5.0f, "envvol.replay-priority-from-look");
            Check(std::abs(it->volume.blendWeight - 0.5f) < 1e-6f, "envvol.replay-weight");
            Check(it->volume.blendRadius == 4.0f, "envvol.replay-radius");
            Check(it->volume.enabled, "envvol.replay-enabled");
        }
        Check(FindEnvVolume(cluster->scene, 99) == nullptr, "envvol.replay-dangling-dropped");
    }
    {
        // Volumes-only doc: no look blocks, no fake authoring.
        auto cluster = std::make_unique<smvk::Cluster>(nullptr);
        cluster->SetOfflineMode(true);
        const int root = addNode(cluster->scene, -1, 0);
        const int n1 = addNode(cluster->scene, root, 0);
        (void)n1;
        smvk::tour::Doc doc;
        doc.editorSettings.emplace_back("envvol.n.0.node", "1");
        doc.editorSettings.emplace_back("envvol.n.0.mask", "63");
        doc.editorSettings.emplace_back("envvol.n.0.priority", "7");
        std::vector<std::string> warns;
        std::string err;
        Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns, &err), "envvol.volsonly-ok");
        const EnvVolumeRow* it = FindEnvVolume(cluster->scene, 1);
        Check(it != nullptr && it->volume.priority == 7.0f, "envvol.volsonly-explicit-priority");
        Check(!cluster->scene.envAuthored, "envvol.volsonly-no-fake-author");
    }

    // Scene master switch: the env.zonesEnabled tour key is RETIRED with
    // the always-on rework (75568c82: the default env packs as the infinite
    // base zone - tour.cpp parses the key for format compatibility but
    // forces the flag true). The check pins that contract.
    {
        auto cluster = std::make_unique<smvk::Cluster>(nullptr);
        cluster->SetOfflineMode(true);
        addNode(cluster->scene, -1, 0);
        smvk::tour::Doc doc;
        doc.editorSettings.emplace_back("env.zonesEnabled", "0");
        std::vector<std::string> warns;
        std::string err;
        Check(smvk::tour::ApplyTourToCluster(*cluster, doc, &warns, &err), "envzone.tour-ok");
        Check(cluster->scene.envZonesEnabled, "envzone.tour-disable-no-blocks");
    }

    // Analytic zone packer + resolver: exact OBB coverage, rotation,
    // inf-mask slabs, blend falloff and weight-0 skip. Mirrors the shader.
    {
        smvk::envzones::EnvZone look;
        look.skyTint = glm::vec3(1.0f, 0.5f, 0.25f);
        look.ambientI = 7.0f;
        look.fogColor = glm::vec3(0.1f, 0.2f, 0.3f);
        look.fogDensity = 0.02f;
        look.volumetrics = 0.6f;
        look.volumetricDensity = 0.03f;
        smvk::envzones::EnvZoneGpu z;
        Check(smvk::envzones::MakeEnvZoneGpu(
                  glm::scale(glm::mat4(1.0f), glm::vec3(10, 2, 10)), 0u, look, 5.0f, 0.8f, 4.0f, z),
              "envzone.pack");
        Check(std::abs(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0)) - 1.0f) < 1e-6f,
              "envzone.inside");
        Check(std::abs(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, 4, 0)) - 0.5f) < 1e-5f,
              "envzone.radius-mid");
        Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, 6, 0)) == 0.0f, "envzone.radius-out");
        std::vector<smvk::envzones::EnvZoneGpu> zones{z};
        float w = 0.0f;
        smvk::envzones::EnvZone got;
        Check(smvk::envzones::EnvZoneAt(zones, glm::vec3(0, 4, 0), w, got), "envzone.at-hit");
        Check(std::abs(w - 0.4f) < 1e-5f, "envzone.at-weight");
        Check(got.skyTint == glm::vec3(1.0f, 0.5f, 0.25f) && std::abs(got.ambientI - 7.0f) < 1e-6f,
              "envzone.at-look");
        Check(!smvk::envzones::EnvZoneAt(zones, glm::vec3(0, 6, 0), w, got) && w == 0.0f,
              "envzone.at-outside");
    }

    // Rotated OBB: the long X box turns along Z; inf-mask half-space and
    // a flat finite axis (empty) are handled like ZoneCovers.
    {
        smvk::envzones::EnvZone look;
        smvk::envzones::EnvZoneGpu z;
        const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));
        Check(smvk::envzones::MakeEnvZoneGpu(rot * glm::scale(glm::mat4(1.0f), glm::vec3(10, 1, 1)),
                                             0u,
                                             look,
                                             0.0f,
                                             1.0f,
                                             0.0f,
                                             z),
              "envzone.rot-pack");
        Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, 0, 9)) == 1.0f, "envzone.rot-inside");
        Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(9, 0, 0)) == 0.0f, "envzone.rot-outside");

        Check(smvk::envzones::MakeEnvZoneGpu(glm::scale(glm::mat4(1.0f), glm::vec3(10, 10, 10)),
                                             kEnvInfNegY,
                                             look,
                                             0.0f,
                                             1.0f,
                                             0.0f,
                                             z),
              "envzone.infmask-pack");
        Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, -1e6f, 0)) == 1.0f,
              "envzone.infmask-below");
        Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, 1e6f, 0)) == 0.0f,
              "envzone.infmask-above");

        Check(!smvk::envzones::MakeEnvZoneGpu(
                  glm::scale(glm::mat4(1.0f), glm::vec3(10, 0, 10)), 0u, look, 0.0f, 1.0f, 0.0f, z),
              "envzone.flat-empty");
        Check(smvk::envzones::MakeEnvZoneGpu(glm::scale(glm::mat4(1.0f), glm::vec3(10, 0, 10)),
                                             kEnvInfNegY | kEnvInfPosY,
                                             look,
                                             0.0f,
                                             1.0f,
                                             0.0f,
                                             z),
              "envzone.flat-inf");
        Check(smvk::envzones::EnvZoneFalloff(z, glm::vec3(0, 1e6f, 0)) == 1.0f,
              "envzone.flat-inf-covers");
    }

    // Zero blend weight is skipped (the resolver keeps the plain look).
    {
        smvk::envzones::EnvZone look;
        smvk::envzones::EnvZoneGpu z;
        Check(
            smvk::envzones::MakeEnvZoneGpu(
                glm::scale(glm::mat4(1.0f), glm::vec3(10, 10, 10)), 0u, look, 0.0f, 0.0f, 0.0f, z),
            "envzone.zero-pack");
        std::vector<smvk::envzones::EnvZoneGpu> zones{z};
        float w = 1.0f;
        smvk::envzones::EnvZone got;
        Check(!smvk::envzones::EnvZoneAt(zones, glm::vec3(0), w, got) && w == 0.0f,
              "envzone.zero-skip");
    }

    // WorldEnvTiles (the bake behind the shader's tile-address resolver):
    // the vertical profile per tile column. The retired lo/mid/hi triple
    // + a single lo/hi bisection could only find ONE flip — an enclosed
    // room (base below, interior inside, base above) bisected between the
    // SAME base winners at both ends and the interior record VANISHED.
    // These pin the reworked bake: the room (3 records), the stacked pair,
    // the explicit 4+ reject (the largest vertical mass, never garbage),
    // and the preserved no-split / underwater half-space shapes.
    {
        using engine::worldpartition::WorldEnvTiles;
        const auto makeTileZone = [](::Scene& s,
                                     const glm::vec3& half,
                                     const glm::vec3& pos,
                                     const glm::vec3& fog,
                                     float priority) {
            const int n = addNode(s, -1, 0);
            if (int(s.globalTransform.size()) <= n)
                s.globalTransform.resize(size_t(n) + 1, glm::mat4(1.0f));
            s.globalTransform[size_t(n)] =
                glm::translate(glm::mat4(1.0f), pos) * glm::scale(glm::mat4(1.0f), half);
            tour::scene::SceneEnv e{};
            e.fogColor = fog; // the look: distinct per zone = record identity
            e.zonePriority = priority;
            SetEnv(s, NodeId(n), e);
            return uint32_t(n);
        };
        const auto recOf = [](const WorldEnvTiles& t, const glm::vec3& fog) {
            for (uint32_t r = 0; r < t.RecordCount(); ++r)
                if (glm::vec3(t.Records()[r].fog) == fog)
                    return r;
            return 0xFFFFFFFFu;
        };

        // Runtime fog settings live in the RESOLVED environment; the
        // base tile record is an unbounded +/- XYZ zone at weight 1,
        // even outside the grid. The source default can differ after
        // an active look edit and must not overwrite that active look.
        {
            ::Scene s;
            s.defaultEnvironment.volumetrics = 0.0f;
            s.defaultEnvironment.volumetricDensity = 0.01f;
            s.environment.volumetrics = 1.5f;
            s.environment.volumetricDensity = 0.075f;
            s.environment.fogDensity = 0.2f;
            WorldEnvTiles t;
            Check(t.Build(s, glm::vec2(0, 0), 1, 1, 0.0f, 40.0f), "envtile.active-base-build");
            Check(std::abs(t.Records()[0].vol.x - 1.0f) < 1e-6f &&
                      std::abs(t.Records()[0].vol.y - 0.075f) < 1e-6f &&
                      std::abs(t.Records()[0].fog.w - 0.2f) < 1e-6f,
                  "envtile.active-base-settings");
            const auto inside = t.At(glm::vec3(32, 20, 32));
            const auto outside = t.At(glm::vec3(-1000, -1000, 1000));
            Check(inside.valid && inside.record == 0 && inside.weight == 1.0f && outside.valid &&
                      outside.record == 0 && outside.weight == 1.0f,
                  "envtile.active-base-infinite-all-axes");
        }

        // THE ROOM: one bounded interior (Y 15..25 over a 0..40 range) —
        // base below, the interior inside, base above.
        {
            ::Scene s;
            (void)makeTileZone(
                s, glm::vec3(32, 5, 32), glm::vec3(32, 20, 32), glm::vec3(1, 0, 0), 5.0f);
            WorldEnvTiles t;
            Check(t.Build(s, glm::vec2(0, 0), 1, 1, 0.0f, 40.0f), "envtile.room-build");
            const uint32_t room = recOf(t, glm::vec3(1, 0, 0));
            Check(room != 0xFFFFFFFFu && room != 0, "envtile.room-record");
            Check(t.At(glm::vec3(32, 5, 32)).record == 0, "envtile.room-below-is-base");
            Check(t.At(glm::vec3(32, 20, 32)).record == room,
                  "envtile.room-inside-is-interior (the vanished-interior regression)");
            Check(t.At(glm::vec3(32, 35, 32)).record == 0, "envtile.room-above-is-base");
        }

        // THE STACK: base -> A (Y 10..20) -> B (Y 20..40) — three records.
        {
            ::Scene s;
            (void)makeTileZone(
                s, glm::vec3(32, 5, 32), glm::vec3(32, 15, 32), glm::vec3(0, 1, 0), 5.0f);
            (void)makeTileZone(
                s, glm::vec3(32, 10, 32), glm::vec3(32, 30, 32), glm::vec3(0, 0, 1), 5.0f);
            WorldEnvTiles t;
            Check(t.Build(s, glm::vec2(0, 0), 1, 1, 0.0f, 40.0f), "envtile.stack-build");
            const uint32_t a = recOf(t, glm::vec3(0, 1, 0));
            const uint32_t b = recOf(t, glm::vec3(0, 0, 1));
            Check(a != 0xFFFFFFFFu && b != 0xFFFFFFFFu && a != b, "envtile.stack-records");
            Check(t.At(glm::vec3(32, 5, 32)).record == 0, "envtile.stack-below-is-base");
            Check(t.At(glm::vec3(32, 15, 32)).record == a, "envtile.stack-inside-A");
            Check(t.At(glm::vec3(32, 30, 32)).record == b, "envtile.stack-inside-B");
        }

        // Four vertical intervals remain distinguishable: there is no
        // three-slot approximation or largest-mass collapse.
        {
            ::Scene s;
            (void)makeTileZone(s, glm::vec3(32,2,32), glm::vec3(32,12,32),
                glm::vec3(1,0,0), 5.0f);
            (void)makeTileZone(s, glm::vec3(32,2,32), glm::vec3(32,16,32),
                glm::vec3(0,1,0), 6.0f);
            (void)makeTileZone(s, glm::vec3(32,11,32), glm::vec3(32,29,32),
                glm::vec3(0,0,1), 7.0f);
            WorldEnvTiles tiles;
            Check(tiles.Build(s, glm::vec2(0), 1, 1, 0, 40), "envtile.stack4-build");
            Check(tiles.At(glm::vec3(32,2,32)).record == 0, "envtile.stack4-base");
            Check(tiles.At(glm::vec3(32,12,32)).record ==
                recOf(tiles,glm::vec3(1,0,0)), "envtile.stack4-first");
            Check(tiles.At(glm::vec3(32,16,32)).record ==
                recOf(tiles,glm::vec3(0,1,0)), "envtile.stack4-second");
            Check(tiles.At(glm::vec3(32,29,32)).record ==
                recOf(tiles,glm::vec3(0,0,1)), "envtile.stack4-third");
        }
        // A 6m interior within a 64m cell must not be lost by a missed
        // center sample, nor smeared across the tile footprint.
        {
            ::Scene s;
            const uint32_t node = makeTileZone(s, glm::vec3(3,3,3),
                glm::vec3(8,10,8), glm::vec3(1,0,0), 5.0f);
            MutateEnvVolume(s,node,[](EnvVolume& v) {
                v.blendRadius = 4.0f;
                v.blendWeight = 0.8f;
            });
            WorldEnvTiles tiles;
            Check(tiles.Build(s,glm::vec2(0),1,1,0,30),"envtile.subtile-build");
            const uint32_t room = recOf(tiles,glm::vec3(1,0,0));
            Check(tiles.At(glm::vec3(8,10,8)).record == room,
                "envtile.subtile-interior");
            Check(tiles.At(glm::vec3(56,10,56)).record == 0,
                "envtile.subtile-no-bleed");
            Check(std::abs(tiles.At(glm::vec3(8,15,8)).weight - 0.4f)<1e-5f,
                "envtile.subtile-continuous-falloff");
        }

        // Preserved shapes: no-split outside every zone + the underwater
        // half-space split (kEnvInfNegY: the zone wins below its top).
        {
            ::Scene s;
            const uint32_t z = makeTileZone(
                s, glm::vec3(32, 8, 32), glm::vec3(32, 8, 32), glm::vec3(0.5f, 0.5f, 0.1f), 5.0f);
            MutateEnvVolume(s, z, [](EnvVolume& v) {
                v.infMask = kEnvInfNegY; // covers y <= 16
            });
            WorldEnvTiles t;
            Check(t.Build(s, glm::vec2(0, 0), 1, 1, 0.0f, 40.0f), "envtile.halfspace-build");
            const uint32_t under = recOf(t, glm::vec3(0.5f, 0.5f, 0.1f));
            Check(under != 0xFFFFFFFFu && under != 0, "envtile.halfspace-record");
            Check(t.At(glm::vec3(32, 5, 32)).record == under, "envtile.halfspace-below-is-zone");
            Check(t.At(glm::vec3(32, 30, 32)).record == 0, "envtile.halfspace-above-is-base");
            const WorldEnvTiles::Lookup out = t.At(glm::vec3(500.0f, 5.0f, 500.0f));
            Check(out.valid && out.record == 0 && out.weight == 1.0f, "envtile.out-of-grid-base");
        }

        // AnyVolumetricFog (the volOn gates' zone-density leg + the
        // shader's envZoneFlags bit1 source — the scan that keeps the
        // volumetric pass marching at ZERO pass density whenever the
        // env table authors fog): only a record's ABSOLUTE
        // volumetricDensity counts; the base record carries the scene
        // env's own (tour::scene::SceneEnv's 0.015 default), so a default-constructed
        // env already authors fog.
        {
            // The pure-base bake (no node zones): the base record's
            // 0.015 default alone makes the scan true.
            {
                ::Scene s;
                WorldEnvTiles t;
                Check(t.Build(s, glm::vec2(0, 0), 1, 1, 0.0f, 40.0f), "envtile.fogscan-base-build");
                Check(t.AnyVolumetricFog(),
                      "envtile.fogscan-base-authored (the base record carries the scene env's "
                      "volumetrics)");
            }
            // NO fog authored anywhere (the base env zeroed + every zone
            // zeroed): the zone tables cannot raise extinction from a
            // zero pass density — the scan is false.
            {
                ::Scene s;
                s.defaultEnvironment.volumetricDensity = 0.0f;
                s.environment.volumetricDensity = 0.0f;
                const uint32_t z = makeTileZone(
                    s, glm::vec3(32, 5, 32), glm::vec3(32, 20, 32), glm::vec3(1, 0, 0), 5.0f);
                if (EnvRow* row = FindEnv(s, z))
                    row->environment.volumetricDensity = 0.0f;
                WorldEnvTiles t;
                Check(t.Build(s, glm::vec2(0, 0), 1, 1, 0.0f, 40.0f), "envtile.fogscan-none-build");
                Check(!t.AnyVolumetricFog(), "envtile.fogscan-none (no fog authored anywhere)");
            }
            // ONE node zone authoring absolute volumetricDensity: the
            // scan is true — the zone raises extinction from a zero
            // pass density (the review's zone-fog gate case).
            {
                ::Scene s;
                s.defaultEnvironment.volumetricDensity = 0.0f;
                s.environment.volumetricDensity = 0.0f;
                const uint32_t z = makeTileZone(
                    s, glm::vec3(32, 5, 32), glm::vec3(32, 20, 32), glm::vec3(1, 0, 0), 5.0f);
                if (EnvRow* row = FindEnv(s, z))
                    row->environment.volumetricDensity = 0.02f;
                WorldEnvTiles t;
                Check(t.Build(s, glm::vec2(0, 0), 1, 1, 0.0f, 40.0f), "envtile.fogscan-zone-build");
                Check(t.AnyVolumetricFog(),
                      "envtile.fogscan-zone-authored (a node zone's absolute density)");
            }
        }
        // The grid EXTENT contract (ComputeEnvGridExtent — the review's
        // finding: mesh-only bounds left volumes outside the grid, their
        // look silently reverting to the base record outside it).
        {
            using engine::worldpartition::ComputeEnvGridExtent;
            using engine::worldpartition::EnvGridBounds;
            using engine::worldpartition::kEnvTileMaxDim;
            // Volumes-only scene (degenerate mesh bounds): the zone's own
            // bounds grow the grid out of the fallback window.
            {
                ::Scene s;
                (void)makeTileZone(s,
                                   glm::vec3(32, 5, 32),
                                   glm::vec3(1000.0f, 20.0f, 1000.0f),
                                   glm::vec3(1, 0, 0),
                                   5.0f);
                const auto e = ComputeEnvGridExtent(
                    s, EnvGridBounds{glm::vec3(0.0f), glm::vec3(0.0f)}); // flat mesh bounds
                const bool covers = e.gridMin.x <= 1000.0f - 32.0f &&
                                    e.gridMin.y <= 1000.0f - 32.0f &&
                                    (e.gridMin.x + float(e.dimX) * 64.0f) >= 1000.0f + 32.0f &&
                                    (e.gridMin.y + float(e.dimZ) * 64.0f) >= 1000.0f + 32.0f;
                Check(covers, "envtile.extent-volumes-only-scene-covered");
            }
            // A zone hanging PAST the mesh bounds: the union covers it.
            {
                ::Scene s;
                (void)makeTileZone(s,
                                   glm::vec3(32, 5, 32),
                                   glm::vec3(500.0f, 20.0f, 500.0f),
                                   glm::vec3(1, 0, 0),
                                   5.0f);
                const auto e = ComputeEnvGridExtent(
                    s, EnvGridBounds{glm::vec3(0.0f), glm::vec3(64.0f, 8.0f, 64.0f)});
                const bool covers = (e.gridMin.x + float(e.dimX) * 64.0f) >= 532.0f &&
                                    (e.gridMin.y + float(e.dimZ) * 64.0f) >= 532.0f;
                Check(covers, "envtile.extent-zone-past-mesh-bounds-covered");
            }
            // An infinite XZ zone (all-inf mask): the grid stretches to
            // the 16 km design edge — the zone applies wherever the grid
            // reaches.
            {
                ::Scene s;
                const uint32_t z =
                    makeTileZone(s, glm::vec3(1, 1, 1), glm::vec3(0.0f), glm::vec3(0, 1, 0), 5.0f);
                MutateEnvVolume(s, z, [](EnvVolume& v) {
                    v.infMask = 63u; // every face infinite
                });
                const auto e =
                    ComputeEnvGridExtent(s, EnvGridBounds{glm::vec3(-32.0f), glm::vec3(32.0f)});
                Check(e.dimX == kEnvTileMaxDim && e.dimZ == kEnvTileMaxDim,
                      "envtile.extent-infinite-zone-full-grid");
            }
            // The underwater half-space (infinite Y face ONLY): the grid
            // must NOT stretch (the zone's XZ is the finite box) and the
            // vertical range must stay the scene's (an infinite-Y zone
            // wins at every ladder height — stretching it would tank the
            // vertical resolution).
            {
                ::Scene s;
                const uint32_t z = makeTileZone(
                    s, glm::vec3(32, 8, 32), glm::vec3(0.0f, 8.0f, 0.0f), glm::vec3(0, 1, 0), 5.0f);
                MutateEnvVolume(s, z, [](EnvVolume& v) { v.infMask = kEnvInfNegY; });
                const auto e = ComputeEnvGridExtent(s,
                                                    EnvGridBounds{glm::vec3(-32.0f, -16.0f, -32.0f),
                                                                  glm::vec3(32.0f, 48.0f, 32.0f)});
                const float spanX = float(e.dimX) * 64.0f;
                const float spanZ = float(e.dimZ) * 64.0f;
                Check(spanX <= 512.0f && spanZ <= 512.0f,
                      "envtile.extent-underwater-grid-not-stretched");
                Check(e.yMin >= -16.0f && e.yMax <= 48.0f,
                      "envtile.extent-underwater-vertical-range-kept");
            }
        }
    }

    // WorldPartition environment residency: subcell selection is
    // interest-driven; distant authored cells must not remain resident.
    {
        ::Scene scene;
        engine::worldpartition::WorldEnvTiles baked;
        Check(baked.Build(scene, glm::vec2(0), 4, 1, 0, 40),
              "envtile.stream-source-build");
        engine::worldpartition::WorldEnvTileProvider provider;
        provider.SetSource(&baked, 1);
        engine::worldpartition::WorldPartition partition;
        partition.AddProvider(&provider);
        engine::worldpartition::StreamingInterest interest{};
        interest.position = glm::dvec3(32, 0, 32);
        interest.radiusMeters = 0;
        interest.layerMask = engine::worldpartition::StreamLayerBit(
            engine::worldpartition::StreamLayer::Environment);
        partition.SetInterests(&interest, 1);
        partition.UpdateFrame(1);
        const auto first = provider.Revision();
        Check(first != 0 && provider.CellCount() == 4 &&
              provider.Cells()[0].candidateCount > 0 &&
              provider.Cells()[3].candidateCount == 0,
              "envtile.stream-near-cell-resident");
        partition.UpdateFrame(2);
        Check(provider.Revision() == first,
              "envtile.stream-steady-no-republish");
        interest.position.x = 224.0;
        partition.SetInterests(&interest, 1);
        partition.UpdateFrame(3);
        Check(provider.Revision() > first &&
              provider.Cells()[0].candidateCount == 0 &&
              provider.Cells()[3].candidateCount > 0,
              "envtile.stream-movement-retire-and-activate");
        Check(partition.RequestCount() > 0,
              "envtile.stream-requested-through-partition");
        partition.SetInterests(nullptr, 0);
        partition.UpdateFrame(4);
        Check(provider.ResidentCount() == 0 && provider.IndexCount() == 0,
              "envtile.stream-no-interests-retired");
    }

    // Async path: a completion is published only with its current ticket;
    // moving the interest must discard stale in-flight page payloads.
    {
        ::Scene scene;
        engine::worldpartition::WorldEnvTiles baked;
        Check(baked.Build(scene, glm::vec2(0), 4, 1, 0, 40),
              "envtile.async-source-build");
        using namespace engine::worldpartition;
        WorldEnvTileProvider provider;
        provider.SetSource(&baked, 20);
        provider.SetMaxNewRequestsPerFrame(1);
        WorldSubcellKey requested{};
        uint64_t ticket = 0;
        size_t requestCount = 0;
        provider.SetRequestCallback([&](WorldSubcellKey key, uint64_t t) {
            requested = key;
            ticket = t;
            ++requestCount;
        });
        WorldPartition partition;
        partition.AddProvider(&provider);
        StreamingInterest interest{};
        interest.position = glm::dvec3(32, 0, 32);
        interest.radiusMeters = 0;
        interest.layerMask = StreamLayerBit(StreamLayer::Environment);
        partition.SetInterests(&interest, 1);
        partition.UpdateFrame(1);
        const WorldSubcellKey oldKey = requested;
        const uint64_t oldTicket = ticket;
        Check(requestCount == 1 && provider.PendingCount() == 1 &&
              provider.ResidentCount() == 0,
              "envtile.async-one-request-in-flight");
        // A stale page should never enter a new camera window.
        interest.position.x = 224;
        partition.SetInterests(&interest, 1);
        partition.UpdateFrame(2);
        Check(!provider.Activate(oldKey, oldTicket, {0}) &&
              provider.ResidentCount() == 0,
              "envtile.async-stale-ticket-rejected");
        Check(ticket != oldTicket && provider.PendingCount() == 1,
              "envtile.async-new-request-after-teleport");
        const uint64_t activeTicket = ticket;
        Check(!provider.Activate(requested, activeTicket, {999}),
              "envtile.async-corrupt-record-id-rejected");
        Check(provider.Activate(requested, activeTicket, {0}),
              "envtile.async-valid-ticket-accepted");
        Check(provider.ResidentCount() == 1 && provider.IndexCount() == 1,
              "envtile.async-publication-only-after-activation");
        const auto revision = provider.Revision();
        partition.UpdateFrame(3);
        Check(provider.Revision() == revision,
              "envtile.async-steady-state-does-not-publish");
        // A new bake supersedes an old page, even if the tile key matches.
        provider.SetSource(&baked, 21);
        Check(provider.ResidentCount() == 0 && provider.PendingCount() == 0,
              "envtile.async-revision-invalidates-residency");
    }

    // Persistent baked pages: stable little-endian manifest and
    // independently decodable world-scoped 64m pages.
    {
        using namespace tour::envcook;
        ::Scene scene;
        engine::worldpartition::WorldEnvTiles baked;
        Check(baked.Build(scene,glm::vec2(0),2,2,0,40),"envtile.pak-source");
        Manifest manifest{};
        manifest.world = 12;
        manifest.gridMin = baked.GridMin();
        manifest.dimX = baked.DimX();
        manifest.dimZ = baked.DimZ();
        manifest.records.assign(baked.Records(),baked.Records()+baked.RecordCount());
        manifest.shapes.assign(baked.Shapes(),baked.Shapes()+baked.RecordCount());
        std::vector<uint8_t> bytes;
        Check(EncodeManifest(manifest,bytes),"envtile.pak-manifest-encode");
        Manifest decoded{};
        Check(DecodeManifest(bytes,12,decoded) && decoded.world==12 &&
              decoded.records.size()==baked.RecordCount() &&
              decoded.shapes.size()==baked.RecordCount() &&
              decoded.dimX==2 && decoded.dimZ==2,"envtile.pak-manifest-roundtrip");
        Check(!DecodeManifest(bytes,13,decoded),"envtile.pak-manifest-wrong-world");
        bytes.pop_back();
        Check(!DecodeManifest(bytes,12,decoded),"envtile.pak-manifest-truncated");
        const engine::worldpartition::WorldSubcellKey key{-1,7};
        const std::vector<uint32_t> ids{0};
        bytes.clear();
        Check(EncodePage(12,key,ids,bytes),"envtile.pak-page-encode");
        std::vector<uint32_t> read;
        Check(DecodePage(bytes,12,key,1,read) && read==ids,
              "envtile.pak-page-roundtrip-negative-key");
        Check(!DecodePage(bytes,12,{0,7},1,read),
              "envtile.pak-page-mismatched-coordinates");
        const auto corrupt = bytes.size()-1;
        bytes[corrupt] = 0xff;
        Check(!DecodePage(bytes,12,key,1,read) && read==ids,
              "envtile.pak-page-invalid-record-fail-atomic");
    }

    std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
    return g_failures == 0 ? 0 : 1;
}
