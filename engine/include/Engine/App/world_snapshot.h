// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Portable, deterministic snapshot of the engine-owned *observable* scene.
// This is not a raw memcpy of a World: clusters, GPU handles, textures,
// physics solvers, audio voices and game-private memory require their own
// explicit providers/asset catalogs. Every float is its IEEE-754 bit pattern
// so cross-language state hashing and NaN/negative-zero fidelity are stable.
#include <Engine/App/world.h>
#include <Engine/Core/Random.h>
#include <Engine/App/snapshot_sequence.h>

#include <bit>
#include <cstdint>
#include <string>

namespace tour::snapshot {

inline uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }
inline Json Vec3(const glm::vec3& value) {
    return Json::array({Bits(value.x), Bits(value.y), Bits(value.z)});
}
inline Json Mat4(const glm::mat4& value) {
    Json matrix = Json::array();
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            matrix.push_back(Bits(value[col][row]));
    return matrix;
}
inline uint32_t NodeFlag(const std::vector<uint8_t>& values, uint32_t node) {
    return node < values.size() ? values[node] : 0u;
}

inline Json EnvState(const tour::scene::SceneEnv& env) {
    return {
        {"sky_color", Vec3(env.skyColor)},
        {"sky_intensity", Bits(env.skyIntensity)},
        {"sun_color", Vec3(env.sunColor)},
        {"sun_azimuth", Bits(env.sunAzimuth)},
        {"sun_elevation", Bits(env.sunElevation)},
        {"sun_intensity", Bits(env.sunIntensity)},
        {"exposure", Bits(env.exposure)},
        {"fog_quality", env.fogQuality},
        {"fog_density", Bits(env.fogDensity)},
        {"fog_color", Vec3(env.fogColor)},
        {"volumetrics", Bits(env.volumetrics)},
        {"volumetric_steps", env.volumetricSteps},
        {"volumetric_max_distance", Bits(env.volumetricMaxDistance)},
        {"volumetric_density", Bits(env.volumetricDensity)},
        {"volumetric_anisotropy", Bits(env.volumetricAnisotropy)},
        {"volumetric_sun", Bits(env.volumetricSun)},
        {"volumetric_gi", Bits(env.volumetricGi)},
        {"volumetric_use_gi", env.volumetricUseGi},
        {"volumetric_use_sun", env.volumetricUseSun},
        {"volumetric_jitter", Bits(env.volumetricJitter)},
        {"volumetric_height_falloff", Bits(env.volumetricHeightFalloff)},
        {"volumetric_height_base", Bits(env.volumetricHeightBase)},
        {"zone_priority", Bits(env.zonePriority)},
        {"skybox_uri", env.skyboxUri}
    };
}

// Captures associations in a canonical slot+generation namespace; a reused
// slot is a different entity and therefore a remove+add in subsequent deltas.
inline Json WorldState(const tour::World& world, const tour::RandomService& rng,
                       const glm::vec3& cameraPosition,
                       const glm::vec3& cameraForward) {
    const auto& graph = world.scene;
    Json state = {
        {"scope", "engine-scene-graph-v1"},
        {"topology_revision", world.gpuSceneTopologyRevision},
        {"camera", {{"position_f32", Vec3(cameraPosition)},
                    {"forward_f32", Vec3(cameraForward)}}},
        {"scene", {
            {"default_camera_node", graph.defaultCameraNode},
            {"active_environment", graph.activeEnvironment},
            {"env_authored", graph.envAuthored},
            {"env_zones_enabled", graph.envZonesEnabled},
            {"environment", EnvState(graph.environment)},
            {"gi_world_source", graph.giWorldSource},
            {"gi_world_guid", graph.giWorldGuid},
            {"gi_world_input_hash", graph.giWorldInputHash},
            {"gi_captures_source", graph.giCapturesSource},
            {"gi_captures_guid", graph.giCapturesGuid},
            {"gi_captures_hash", graph.giCapturesHash},
            {"anim_clip_refs", graph.animClipRefs}
        }},
        {"nodes", Json::object()},
        {"rng", Json::object()}
    };

    for (uint32_t n = 0; n < graph.hierarchy.size(); ++n) {
        if (!tour::scene::IsNodeAlive(graph, n))
            continue;
        const uint32_t generation = tour::scene::NodeGenerationOf(graph, n);
        const auto& hierarchy = graph.hierarchy[n];
        Json node = {
            {"slot", n}, {"generation", generation},
            {"parent", hierarchy.parent},
            {"first_child", hierarchy.firstChild},
            {"next_sibling", hierarchy.nextSibling},
            {"last_sibling", hierarchy.lastSibling},
            {"level", hierarchy.level},
            {"name", tour::scene::getNodeName(graph, static_cast<int>(n))},
            {"uuid", tour::scene::GetNodeUuid(graph, n)},
            {"hidden", NodeFlag(graph.hidden, n)},
            {"static_root", NodeFlag(graph.staticRoot, n)},
            {"no_lightmap", NodeFlag(graph.noLightmap, n)},
            {"lightmap", NodeFlag(graph.lightmap, n)},
            {"dither_closeup", NodeFlag(graph.ditherCloseup, n)},
            {"casts_volumetric_shadow", NodeFlag(graph.castsVolumetricShadow, n)}
        };
        if (n < graph.localTransform.size())
            node["local_f32"] = Mat4(graph.localTransform[n]);
        if (n < graph.globalTransform.size())
            node["global_f32"] = Mat4(graph.globalTransform[n]);

        if (const auto* row = tour::scene::FindMesh(graph, n))
            node["mesh"] = row->mesh;
        if (const auto* row = tour::scene::FindMaterial(graph, n))
            node["material"] = row->material;
        if (const auto* row = tour::scene::FindLight(graph, n))
            node["light"] = row->light;
        if (const auto* row = tour::scene::FindCamera(graph, n))
            node["camera_ref"] = row->camera;
        if (const auto* row = tour::scene::FindDecal(graph, n))
            node["decal"] = row->decal;
        if (const auto* row = tour::scene::FindCubemap(graph, n))
            node["cubemap_radius_f32"] = Bits(row->cubemap.radius);
        if (const auto* row = tour::scene::FindGiVolume(graph, n)) {
            const auto& v = row->volume;
            node["gi_volume"] = {
                {"priority", v.priority}, {"spacing_f32", Bits(v.spacing)},
                {"inside_fade_f32", Bits(v.insideFade)},
                {"outside_fade_f32", Bits(v.outsideFade)},
                {"spacing_low_f32", Bits(v.spacingLow)},
                {"spacing_high_f32", Bits(v.spacingHigh)},
                {"opt_spacing_f32", Bits(v.optSpacing)},
                {"opt_phase_f32", Json::array({
                    Bits(v.optPhase[0]), Bits(v.optPhase[1]), Bits(v.optPhase[2])})},
                {"bake_source", v.bakeSource}, {"bake_guid", v.bakeGuid},
                {"bake_input_hash", v.bakeInputHash}
            };
        }
        if (const auto* row = tour::scene::FindEnv(graph, n))
            node["environment"] = EnvState(row->environment);
        if (const auto* row = tour::scene::FindEnvVolume(graph, n)) {
            const auto& v = row->volume;
            node["env_volume"] = {
                {"inf_mask", v.infMask}, {"priority_f32", Bits(v.priority)},
                {"blend_weight_f32", Bits(v.blendWeight)},
                {"blend_radius_f32", Bits(v.blendRadius)},
                {"enabled", v.enabled}
            };
        }
        if (const auto* row = tour::scene::FindAudioSource(graph, n)) {
            const auto& source = row->source;
            Json sounds = Json::array();
            for (const auto& sound : source.sounds)
                sounds.push_back({
                    {"name", sound.name}, {"asset_path", sound.assetPath},
                    {"sound_id", sound.soundId}, {"gain_f32", Bits(sound.gain)},
                    {"loop", sound.loop}
                });
            node["audio_source"] = {
                {"volume_f32", Bits(source.volume)},
                {"spatial", source.spatial}, {"play_on_load", source.playOnLoad},
                {"min_distance_f32", Bits(source.minDistance)},
                {"max_distance_f32", Bits(source.maxDistance)},
                {"sounds", std::move(sounds)}
            };
        }
        if (const auto* row = tour::scene::FindAudioRoom(graph, n)) {
            const auto& room = row->room;
            node["audio_room"] = {
                {"enabled", room.enabled}, {"priority", room.priority},
                {"inside_fade_f32", Bits(room.insideFade)},
                {"outside_fade_f32", Bits(room.outsideFade)},
                {"wet_f32", Bits(room.wet)}, {"rt60_f32", Bits(room.rt60)},
                {"damping_f32", Bits(room.damping)},
                {"transition_f32", Bits(room.transitionSeconds)}
            };
        }
        if (const auto* row = tour::scene::FindPhysicsBody(graph, n)) {
            const auto& body = row->body;
            node["physics_authoring"] = {
                {"motion", body.motion}, {"shape", body.shape},
                {"mass_f32", Bits(body.mass)},
                {"friction_f32", Bits(body.friction)},
                {"restitution_f32", Bits(body.restitution)},
                {"half_extents_f32", Vec3(body.halfExtents)},
                {"radius_f32", Bits(body.radius)},
                {"height_f32", Bits(body.height)},
                {"sensor", body.sensor}
            };
        }
        if (const auto* row = tour::scene::FindTriggerVolume(graph, n))
            node["trigger"] = {
                {"enabled", row->trigger.enabled},
                {"once", row->trigger.once},
                {"tag", row->trigger.tag}
            };
        if (const auto* row = tour::scene::FindSchemaAssoc(graph, n))
            node["schema_association"] = {
                {"schema_id", row->schemaId},
                {"value_bytes", row->values}
            };
        state["nodes"][std::to_string(n) + ":" + std::to_string(generation)] = std::move(node);
    }

    const auto checkpoint = rng.Capture();
    state["rng"]["algorithm"] = "pcg32-xsh-rr-v1";
    state["rng"]["seed"] = checkpoint.seed;
    state["rng"]["streams"] = Json::object();
    for (const auto& [name, stream] : checkpoint.streams)
        state["rng"]["streams"][name] = {
            {"state", stream.state}, {"increment", stream.increment}
        };
    return state;
}

} // namespace tour::snapshot
