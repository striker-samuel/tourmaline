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
	                      // GI volume nodes = the convex bake inputs
	// RETIRED (stage 3 of the octree retirement — the .sggi world-field
	// bake is deleted): --out still parses (old scripts get the loud
	// refusal) but Run() REFUSES any non-empty out — the convex/regular
	// bakes mint their own artifacts.
	std::string out;      // non-empty => Run() refuses (exit 2)
	uint32_t rays = 1024;  // regular-bake rays/probe (convex bakes per-probe
	uint32_t bounces = 3;  // transport depth (regular + convex + lm chains)
	uint32_t threads = 0;  // 0 = hardware concurrency
	// (The oct-bake-only knobs — errorRays/root/minBrick/maxProbes/
	// tolerance/nearFactor/waves/diffusion/smoothingPasses/support — are
	// deleted with BakeSgGiVolume.)
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
	std::string analyze; // .sgrv / .givolume host path: inspect instead of
	                      // baking (.sggi is retired — Run() refuses it)
	// White-world albedo: default "0.7" (single known albedo everywhere).
	// "off" restores the textured bake (per-material albedo from factors
	// + KTX averages; known to print chromatic dots on contrasty scenes).
	// "r,g,b" bakes a custom uniform tint instead of grey.
	std::string uniformAlbedo = "0.7";
	// Regular-lattice prototype (Ready-at-Dawn style, .sgrv alongside
	// the adaptive .sggi): fixed OBB grid, diagonal projection, hardware
	// trilinear at runtime. Empty regularOut = no regular bake.
	std::string regularOut;
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
	// Empty giVolumeSpec = no convex bake.
	std::string giVolumeSpec;
	uint32_t giVolumeDirs = 1536;       // capture direction count (6*16^2)
	float giVolumeCaptureSpacing = 0.0f; // INERT (per-probe bake — no capture lattice); CLI compat
	uint32_t giVolumeSmoothing = 3;     // Laplacian volume-smoothing passes (per-probe bake needs it — see GiVolumeBakeConfig)
	// Weave transmission for MASK (cutout cloth) materials: the cloth
	// keeps its lit albedo AND passes light tinted per texel (the GI
	// field behind the banners sees the tinted pools; the tint stays
	// REAL in the white-world bake - GiSceneBuildOptions::
	// maskTransmissive). The AUTHORING surface is the exportSettings
	// node's maskTransmission field (the invalidation hashes fold the
	// effective value); the CLI flag stays an explicit override whose
	// records then mismatch the pak gate by design. Default OFF.
	bool maskTransmission = false;
	bool maskTransmissionSet = false; // the CLI flag was given
};

// Byte-identical to the CLI run (same return codes): parses argv in
// main(), runs here. Prints go through tool::out()/tool::err() so
// embedders can capture lines via tool::SetLineSink.
bool ParseArgs(int argc, char** argv, Args& out);
void Usage();
// Runs the bake; returns the process exit code.
int Run(Args& args);

} // namespace gibaker
