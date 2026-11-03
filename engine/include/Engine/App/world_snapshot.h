// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Portable, deterministic snapshot of the engine-owned *observable* scene.
// This is not a raw memcpy of a World: clusters, GPU handles, textures,
// physics solvers, audio voices and game-private memory require their own
// explicit providers/asset catalogs. Every float is its IEEE-754 bit pattern
// so cross-language state hashing and NaN/negative-zero fidelity are stable.
#include <Engine/App/world.h>
#include <Engine/App/cluster.h>
#include <Engine/Core/Random.h>
#include <Engine/App/snapshot_sequence.h>

#include <bit>
#include <cstdint>
#include <string>

namespace tour::snapshot {

inline uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }
inline uint64_t Bits64(double value) { return std::bit_cast<uint64_t>(value); }
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
inline std::string OwnedUri(smvk::StringViewT view) {
    return view.valid() ? view.Str() : std::string{};
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

inline Json RenderState(const tour::scene::SceneRenderSettings& r) {
    return {
        {"bloom_enabled", r.bloomEnabled},
        {"bloom_intensity_f32", Bits(r.bloomIntensity)},
        {"bloom_threshold_f32", Bits(r.bloomThreshold)},
        {"tonemap_mode", r.tonemapMode},
        {"auto_exposure", r.autoExposure},
        {"manual_exposure_f32", Bits(r.manualExposure)},
        {"exposure_ev_bias_f32", Bits(r.exposureEvBias)},
        {"exposure_speed_to_dark_f32", Bits(r.exposureSpeedToDark)},
        {"exposure_speed_to_bright_f32", Bits(r.exposureSpeedToBright)},
        {"exposure_min_f32", Bits(r.exposureMin)},
        {"exposure_max_f32", Bits(r.exposureMax)},
        {"ao_mode", r.aoMode},
        {"ao_radius_f32", Bits(r.aoRadius)},
        {"ao_bias_f32", Bits(r.aoBias)},
        {"ao_intensity_f32", Bits(r.aoIntensity)},
        {"ao_max_distance_f32", Bits(r.aoMaxDistance)},
        {"ao_ssao_samples", r.aoSsaoSamples},
        {"ao_blur_sharpness_f32", Bits(r.aoBlurSharpness)},
        {"ao_sao_samples", r.aoSaoSamples},
        {"ao_sao_spiral_turns", r.aoSaoSpiralTurns},
        {"ssr_enabled", r.ssrEnabled},
        {"ssr_intensity_f32", Bits(r.ssrIntensity)},
        {"motion_blur_enabled", r.motionBlurEnabled},
        {"dof_enabled", r.dofEnabled},
        {"aa_mode", r.aaMode},
        {"cubemap_enabled", r.cubemapEnabled},
        {"cubemap_intensity_f32", Bits(r.cubemapIntensity)}
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
            {"node_slot_generations", graph.nodeGeneration},
            {"node_free_list", graph.nodeFreeList},
            {"node_names", graph.nodeNames},
            {"material_names", graph.materialNames},
            {"static_roots_serialized", graph.staticRootsSerialized},
            {"env_authored", graph.envAuthored},
            {"env_zones_enabled", graph.envZonesEnabled},
            {"environment", EnvState(graph.environment)},
            {"default_environment", EnvState(graph.defaultEnvironment)},
            {"has_render_settings", graph.hasRenderSettings},
            {"render_settings", RenderState(graph.renderSettings)},
            {"default_render_settings", RenderState(graph.defaultRenderSettings)},
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


    // World-local numeric mesh/material/light ids are meaningful only
    // when paired with their URI catalog. Capture the authored reverse
    // lookup wherever one exists, not transient GPU/pool handles.
    // Empty URIs are deliberate: unnamed runtime/procedural resources
    // require an opt-in game/provider identity to recreate.
    state["asset_references"] = Json::object();
    auto& assets = state["asset_references"];
    assets["meshes"] = Json::array();
    for (const auto& location : world.meshLocations)
        assets["meshes"].push_back(location.cluster
            ? OwnedUri(location.cluster->GetMeshUri(location.handle)) : "");
    assets["materials"] = Json::array();
    for (const auto& location : world.materialLocations)
        assets["materials"].push_back(location.cluster
            ? OwnedUri(location.cluster->GetMaterialUri(location.handle)) : "");
    assets["lights"] = Json::array();
    for (const auto& location : world.lightLocations)
        assets["lights"].push_back(location.cluster
            ? OwnedUri(location.cluster->GetLightUri(location.handle)) : "");
    assets["cameras"] = Json::array();
    for (const auto& location : world.cameraLocations)
        assets["cameras"].push_back(location.cluster
            ? OwnedUri(location.cluster->GetCameraUri(location.handle)) : "");
    assets["decals"] = Json::array();
    for (const auto& location : world.decalLocations)
        assets["decals"].push_back(location.cluster
            ? OwnedUri(location.cluster->GetDecalUri(location.handle)) : "");

    // Runtime engine-owned entities (not just authored scene nodes).
    // Generational slot metadata and free-list order matter for the
    // determinism of the NEXT Spawn/Destroy operation. Raw pointers,
    // transient GPU handles and arbitrary gameplay objects are omitted.
    const auto ownerId = [&world](const tour::Cluster* owner) -> uint64_t {
        if (!owner) return 0;
        const auto it = world.gpuSceneClusterIds.find(owner);
        return it == world.gpuSceneClusterIds.end() ? 0 : it->second;
    };
    Json entities = {
        {"audio", Json::array()},
        {"meshes", Json::array()},
        {"material_instances", Json::array()},
        {"animators", Json::array()},
        {"audio_free", world.audioEntityFree},
        {"mesh_free", world.meshEntityFree},
        {"material_instance_free", world.materialInstanceFree},
        {"animator_free", world.animatorFree},
        {"material_user_data", world.materialUserData},
        {"material_shader_family", world.materialShaderFamily},
        {"next_gpu_scene_cluster_id", world.nextGpuSceneClusterId}
    };
    for (const auto& entity : world.audioEntities) {
        entities["audio"].push_back({
            {"alive", entity.alive}, {"generation", entity.generation},
            {"owner_cluster_id", ownerId(entity.owner)},
            {"cluster_node", entity.clusterNode},
            {"merged_node", entity.mergedNode}
        });
    }
    for (const auto& entity : world.meshEntities) {
        entities["meshes"].push_back({
            {"alive", entity.alive}, {"generation", entity.generation},
            {"transform_owner_cluster_id", ownerId(entity.transformOwner)},
            {"mesh_owner_cluster_id", ownerId(entity.meshOwner)},
            {"cluster_node", entity.clusterNode},
            {"merged_node", entity.mergedNode},
            {"mesh_uri", entity.meshOwner
                ? OwnedUri(entity.meshOwner->GetMeshUri(entity.mesh)) : ""},
            {"animator_slot", entity.animatorSlot},
            {"material_instance_handle", entity.materialInstance.value}
        });
    }
    for (const auto& instance : world.materialInstances) {
        entities["material_instances"].push_back({
            {"alive", instance.alive}, {"generation", instance.generation},
            {"owner_cluster_id", ownerId(instance.baseOwner)},
            {"base_material_uri", instance.baseOwner
                ? OwnedUri(instance.baseOwner->GetMaterialUri(instance.base)) : ""},
            {"user_data", instance.userData},
            {"overrides_user_data", instance.overridesUserData}
        });
    }
    const auto animatorLayer = [](const tour::AnimatorLayer& layer) -> Json {
        return {
            {"clip_name", layer.clip.name},
            {"clip_duration_f64", Bits64(layer.clip.duration)},
            {"clip_fps_f64", Bits64(layer.clip.fps)},
            {"cursor_f64", Bits64(layer.cursor)},
            {"weight_f32", Bits(layer.weight)},
            {"loop", layer.loop}, {"active", layer.active}
        };
    };
    for (const auto& entity : world.animators) {
        const auto& playback = entity.state;
        Json rest = Json::array();
        for (const auto& transform : playback.restLocals)
            rest.push_back(Mat4(transform));
        Json joints = Json::array();
        for (const auto& transform : playback.jointLocals)
            joints.push_back(Mat4(transform));
        entities["animators"].push_back({
            {"alive", entity.alive}, {"generation", entity.generation},
            {"mesh_entity_slot", entity.meshEntitySlot},
            {"current", animatorLayer(playback.current)},
            {"fade", animatorLayer(playback.fade)},
            {"fade_duration_f32", Bits(playback.fadeDuration)},
            {"fade_t_f32", Bits(playback.fadeT)},
            {"fading", playback.fading}, {"speed_f32", Bits(playback.speed)},
            {"paused", playback.paused}, {"playing", playback.playing},
            {"mesh_handle", playback.mesh.value},
            {"target_cluster_id", ownerId(playback.target)},
            {"rest_locals", std::move(rest)}, {"joint_locals", std::move(joints)}
        });
    }
    state["runtime_entities"] = std::move(entities);

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
