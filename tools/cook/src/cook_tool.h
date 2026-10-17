// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Cook as a library (the editor links it; the CLI stays a thin user
// interface over the same Run). CookArgs mirrors the FULL CLI surface:
// every --flag has a field here and the CLI main maps its parsed Args
// 1:1 (the original lib split mapped only 6 fields — --cubemap,
// --dump-pak, the lightmap/cvolume knobs and the tour-merge explicitSet
// silently no-op'd through the CLI; that lossy map is what this struct
// kills). Embedders (the editor) fill the subset they drive; every other
// field keeps the CLI default.
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/GI/lightmap.h>

namespace cooktool {

struct CookArgs {
	std::string contentDir = "content";
	std::string tour; // definition tour (preferred; else inVfsList/inVfs)
	std::vector<std::string> inVfsList; // processed .geometry artifacts
	std::string inVfs; // primary identity (first artifact)
	std::string outPak; // destination pak (stem source for bake sidecar
	                    // names: cvol manifest/volumes, cubemap base fallback).
	                    // No pak is written — pak.exe assembles it.
	std::unordered_set<std::string> explicitSet; // CLI-wins tour-merge keys
	bool meshopt = true;
	float overdraw = 1.05f;
	// Cooked LOD chain (meshopt_simplify, sibling blobs "<base>/lodN").
	bool lods = false;
	std::string lodRatios = "0.6,0.3,0.12";
	float lodError = 0.02f;
	bool meshlets = false;
	bool autoKtx = true;
	bool collapseStatic = false;
	// Lightmap cook (--lightmap on).
	bool lightmap = false;
	float lmMetersPerTexel = 0.25f;
	uint32_t lmLobes = 12;
	uint32_t lmAtlasWidth = 1024;
	uint32_t lmMaxHeight = 8192;
	uint32_t lmGutter = 1;
	smvk::gi::SgLightmapCompression lmCompress =
		smvk::gi::SgLightmapCompression::Bc6HOpt;
	uint32_t lmRays = 1024;
	uint32_t lmBounces = 3;
	int lmProxyRes = 256;
	float lmLobeRelErr = 0.15f;
	std::string lmLodLock = "attr";
	bool lmDebugPink = false;
	bool lmStitch = true;
	float lmCurveTarget = 7.0f;
	float lmCurveMax = 2.5f;
	std::string lmExclude;
	std::vector<std::string> lmExcludeKeywords;
	float lmExcludeSmall = 0.0f;
	// Bake sun/sky (the tour env block wins when the definition carries
	// one — see EnvForBake).
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
	float skyIntensity = 600.0f;
	std::vector<std::string> spots;
	std::string backend = "embree";
	// NOTE: no giProbes here (pak owns probe registration; a --gi-probe
	// CLI flag never took effect in cook and was removed).
	// Preconvolved specular cubemaps (--cubemap on; cook.exe bakes, pak
	// embeds the loose sidecars at the canonical gi/cubemaps/ paths).
	// Capture sets are NAMELESS: the base is the scene stem (pak.exe
	// discovers + embeds by gi/cubemaps/<stem>.cubemanifest).
	bool cubemap = false;
	uint32_t cubemapSize = 128;
	uint32_t cubemapRays = 128;
	uint32_t cubemapBounces = 5;
	uint32_t cubemapSpp = 8;
	std::vector<std::string> cubemapPos;
	float cubemapHeight = 1.5f;
	float cubemapGrid = 8.0f;
	bool cubemapOctree = false;
	float cubemapOctreeMax = 8.0f;
	float cubemapOctreeMin = 2.0f;
	uint32_t cubemapOctreeDepth = 4;
	std::vector<std::string> cubemapExtraPos;
	float cubemapExtraRadius = 5.5f;
	bool cubemapPng = true;
	bool cubemapPngMips = true;
	// Authored convex GI volumes baked INTO the pak (default on).
	bool cvolume = true;
	bool quickBakes = false; // capture-only refresh, no pak write
	// Read-only inspection (--dump-pak): no cooking, no writes.
	std::string dumpPak;
	std::vector<uint32_t> dumpNodes;
	// Explicit programmatic registry root (editor store; bare CLI
	// cooks derive it from the content root). Optional acceleration,
	// NEVER a precondition.
	std::string registryDir;
};

// Byte-identical to the CLI run (same return codes). Prints go through
// tool::out()/tool::err() so embedders can capture lines via
// tool::SetLineSink.
int Run(const CookArgs& args);

} // namespace cooktool
