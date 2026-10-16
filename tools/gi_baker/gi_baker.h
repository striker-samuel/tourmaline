// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GiBaker as a library (the editor links it; the CLI stays a thin
// interface over the same Run). Args mirror the --help flags 1:1;
// fill them directly instead of building argv strings.
#include <cstdint>
#include <string>
#include <vector>

namespace gibaker {

struct Args {
	std::string contentDir = "content";
	std::string scene;    // sponza | bistro | bistro-niagara
	std::string inVfs;    // arbitrary model (overrides --scene input)
	std::string tour;     // definition tour: root reference = the source,
	                      // cvolume nodes = the convex bake inputs
	std::string out;      // output host path (default <content>/gi/<scene>.sggi)
	uint32_t rays = 1024;
	uint32_t bounces = 3;
	uint32_t errorRays = 32;
	float root = 8.0f;
	float minBrick = 0.5f;
	uint32_t maxProbes = 262144;
	float tolerance = 0.12f;
	float nearFactor = 1.0f;
	uint32_t waves = 8;
	uint32_t threads = 0; // 0 = hardware concurrency
	uint32_t diffusion = 0; // placement jitter+repulsion (keep 0: post-bake
	                      // moves invalidate baked radiance)
	uint32_t smoothingPasses = 0; // Laplacian radiance passes (keep 0: smoothing
	                              // without visibility leaks across walls)
	float support = 1.0f; // support radius = factor * brickSize (1.0 =
	                       // partition of unity + ~27 probes/cell; higher
	                       // stuffs cells to the cap and drops probes)
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
	bool textures = true;
	bool emissive = true;
	bool verify = true;
	// Ray-tracing backend for the bake transport: "embree" (vendored
	// external/embree, packet/SIMD traversal — the fast default) or
	// "builtin" (the deterministic binned-SAH BVH reference; same
	// transport math, agrees within Monte-Carlo noise).
	std::string backend = "embree";
	uint32_t lmProbe = 0; // >0: lightmap-chain diagnostic on N meshes
	std::string analyze; // .sggi host path: inspect instead of baking
	// White-world albedo: default "0.7" (single known albedo everywhere).
	// "off" restores the textured bake (per-material albedo from factors
	// + KTX averages; known to print chromatic dots on contrasty scenes).
	// "r,g,b" bakes a custom uniform tint instead of grey.
	std::string uniformAlbedo = "0.7";
	// Regular-lattice prototype (Ready-at-Dawn style, .sgrv alongside
	// the adaptive .sggi): fixed OBB grid, diagonal projection, hardware
	// trilinear at runtime. Empty regularOut = no regular bake.
	std::string regularOut;
	bool outExplicit = false;
	std::string regMin, regMax;   // "x,y,z" world AABB (axis-aligned OBB)
	std::string regDims = "32,16,32"; // "nx,ny,nz"
	float regSpacing = 0.0f;      // voxel spacing override (m); 0 = use regDims
	std::string regPreset;        // bistro-bar | bistro-street | scene
	bool printBounds = false;     // print trace-scene AABB and exit
	bool scanInterior = false;    // enclosure map + enclosed-component boxes
	// Static spot lights for the bake (same spec as PakCooker --spot).
	std::vector<std::string> spots;
	// Convex GI volumes (the probes-only pivot's confined-space path):
	// --cv-spec authoring file, baked by sparse radiance+depth captures
	// + cubemap reprojection into dense probe grids (BO3 pipeline).
	// Empty cvSpec = no convex bake.
	std::string cvSpec;
	uint32_t cvDirs = 1536;       // capture direction count (6*16^2)
	float cvCaptureSpacing = 0.0f; // 0 = 3x probe spacing per volume
	uint32_t cvSmoothing = 1;     // Laplacian volume-smoothing passes
};

// Byte-identical to the CLI run (same return codes): parses argv in
// main(), runs here. Prints go through tool::out()/tool::err() so
// embedders can capture lines via tool::SetLineSink.
bool ParseArgs(int argc, char** argv, Args& out);
void Usage();
// Runs the bake; returns the process exit code.
int Run(Args& args);

} // namespace gibaker
