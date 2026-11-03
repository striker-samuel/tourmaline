// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless client of the ENGINE ReferenceSceneRenderer. JSON scene snapshots
// are useful for deterministic Material Lab references and external tools.
#include <Engine/GI/bake.h>
#include <Engine/Renderer/ReferenceSceneRenderer.h>
#include <Engine/Utils/LutIo.h>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using json = nlohmann::json;

glm::vec3 Vec3(const json& j) {
	if (!j.is_array() || j.size() != 3)
		throw std::runtime_error("expected RGB or xyz array with 3 elements");
	return {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()};
}
glm::vec3 Csv3(const std::string& s) {
	std::array<float, 3> v{};
	size_t begin = 0;
	for (int i = 0; i < 3; ++i) {
		const size_t comma = s.find(',', begin);
		if ((comma == std::string::npos) != (i == 2))
			throw std::runtime_error("expected comma-separated xyz");
		const std::string part = s.substr(begin,
			comma == std::string::npos ? comma : comma - begin);
		size_t consumed = 0;
		v[i] = std::stof(part, &consumed);
		if (consumed != part.size())
			throw std::runtime_error("bad camera coordinate");
		begin = comma == std::string::npos ? s.size() : comma + 1;
	}
	return {v[0], v[1], v[2]};
}
void LoadScene(const std::string& path, tour::gi::GiTraceScene& scene) {
	std::ifstream in(path);
	if (!in)
		throw std::runtime_error("cannot open scene " + path);
	json j;
	in >> j;
	if (j.at("schema_version") != 1)
		throw std::runtime_error("unsupported reference scene schema");
	for (const auto& x : j.at("positions"))
		scene.positions.push_back(Vec3(x));
	for (const auto& x : j.at("indices"))
		scene.indices.push_back(x.get<uint32_t>());
	for (const auto& x : j.at("triangle_materials"))
		scene.triMaterial.push_back(x.get<uint32_t>());
	if (j.contains("normals")) {
		for (const auto& x : j.at("normals"))
			scene.normals.push_back(Vec3(x));
	}
	if (j.contains("uvs")) {
		for (const auto& x : j.at("uvs"))
			scene.uvs.push_back({x.at(0).get<float>(), x.at(1).get<float>()});
	}
	for (const auto& x : j.at("materials")) {
		tour::gi::GiTraceMaterial m{};
		m.baseColor = Vec3(x.at("baseColor"));
		m.metallic = std::clamp(x.value("metallic", 0.0f), 0.0f, 1.0f);
		m.roughness = std::clamp(x.value("roughness", 1.0f), 0.04f, 1.0f);
		m.albedo = m.baseColor * (1.0f - m.metallic);
		m.emissive = x.contains("emissive") ? Vec3(x.at("emissive")) : glm::vec3(0);
		scene.materials.push_back(m);
	}
	if (j.contains("environment")) {
		const auto& e = j.at("environment");
		scene.sunDirection = glm::normalize(Vec3(e.at("sunDirection")));
		scene.sunColor = Vec3(e.at("sunColor"));
		scene.sunIntensity = e.value("sunIntensity", 0.0f);
		scene.skyZenith = Vec3(e.at("skyZenith"));
		scene.skyHorizon = Vec3(e.at("skyHorizon"));
		scene.skyGround = Vec3(e.at("skyGround"));
		scene.skyIntensity = e.value("skyIntensity", 0.0f);
		scene.skyExponent = e.value("skyExponent", 2.0f);
		if (e.contains("spots")) {
			for (const auto& s : e.at("spots")) {
				tour::gi::GiTraceSpot spot{};
				spot.position = Vec3(s.at("position"));
				spot.direction = glm::normalize(Vec3(s.value("direction",
					json::array({0.0f, -1.0f, 0.0f}))));
				spot.color = Vec3(s.at("color"));
				spot.range = s.value("range", 0.0f);
				spot.cosInner = s.value("cosInner", -1.0f);
				spot.cosOuter = s.value("cosOuter", -1.0f);
				scene.spots.push_back(spot);
			}
		}
	}
	if (scene.indices.size() % 3 != 0 ||
		scene.triMaterial.size() != scene.indices.size() / 3 ||
		(!scene.normals.empty() && scene.normals.size() != scene.positions.size()) ||
		(!scene.uvs.empty() && scene.uvs.size() != scene.positions.size()))
		throw std::runtime_error("invalid triangle/material/vertex array lengths");
	for (uint32_t t : scene.triMaterial)
		if (t >= scene.materials.size())
			throw std::runtime_error("invalid material index");
	for (uint32_t i : scene.indices)
		if (i >= scene.positions.size())
			throw std::runtime_error("invalid vertex index");
	scene.SetTraceBackend(tour::gi::TraceBackend::Embree);
	scene.Finalize();
	if (scene.Bvh().Backend() != tour::gi::TraceBackend::Embree)
		throw std::runtime_error("Embree acceleration structure unavailable");
}
} // namespace

int main(int argc, char** argv) {
	try {
		tour::renderer::ReferenceCamera camera{};
		tour::renderer::ReferenceRenderSettings opts{};
		std::string input, output;
		for (int i = 1; i < argc; ++i) {
			const std::string k = argv[i];
			if (k == "--help" || k == "-h") {
				std::cout << "ReferenceScene --scene scene.refscene.json --output out.png "
					"[--camera-pos x,y,z] [--camera-fwd x,y,z] [--camera-up x,y,z] "
					"[--width N] [--height N] [--spp N] [--bounces N] "
					"[--fov deg] [--ortho-half-height m] [--exposure value] "
					"[--seed N] [--threads N]\n";
				return 0;
			}
			if (i + 1 >= argc)
				throw std::runtime_error("missing value after " + k);
			const std::string val = argv[++i];
			if (k == "--scene") input = val;
			else if (k == "--output") output = val;
			else if (k == "--camera-pos") camera.position = Csv3(val);
			else if (k == "--camera-fwd") camera.forward = Csv3(val);
			else if (k == "--camera-up") camera.up = Csv3(val);
			else if (k == "--width") opts.width = uint32_t(std::stoul(val));
			else if (k == "--height") opts.height = uint32_t(std::stoul(val));
			else if (k == "--spp") opts.samplesPerPixel = uint32_t(std::stoul(val));
			else if (k == "--bounces") opts.maxBounces = uint32_t(std::stoul(val));
			else if (k == "--fov") camera.fovYDeg = std::stof(val);
			else if (k == "--ortho-half-height") camera.orthoHalfHeight = std::stof(val);
			else if (k == "--exposure") opts.exposure = std::stof(val);
			else if (k == "--seed") opts.seed = uint32_t(std::stoul(val));
			else if (k == "--threads") opts.threads = uint32_t(std::stoul(val));
			else throw std::runtime_error("unknown option " + k);
		}
		if (input.empty() || output.empty())
			throw std::runtime_error("--scene and --output are required (see --help)");
		tour::gi::GiTraceScene scene;
		LoadScene(input, scene);
		auto image = tour::renderer::ReferenceSceneRenderer::Render(scene, camera, opts);
		auto rgba = image.ToRgba8(opts.exposure);
		std::filesystem::path dest(output);
		if (!dest.parent_path().empty())
			std::filesystem::create_directories(dest.parent_path());
		if (rgba.empty() || !engine::LutWritePng(output.c_str(),
			int(image.width), int(image.height), rgba.data()))
			throw std::runtime_error("PNG export failed");
		const std::string pfm = dest.replace_extension(".pfm").string();
		if (!image.WritePfm(pfm))
			throw std::runtime_error("HDR PFM export failed");
		std::cout << "ReferenceScene: " << output << " + " << pfm << "\n";
		return 0;
	}
	catch (const std::exception& e) {
		std::cerr << "ReferenceScene: " << e.what() << "\n";
		return 1;
	}
}
