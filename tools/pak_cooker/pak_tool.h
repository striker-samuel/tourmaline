#pragma once
// Pak as a library (the editor links it; the CLI stays a thin user
// interface over the same Run). PakArgs carries only what embedders
// set (content root, tour-or-artifacts, scratch, out pak); every other
// flag keeps its CLI default. Fill it directly instead of building
// argv strings.
#include <string>
#include <vector>

namespace paktool {

struct PakArgs {
	std::string contentDir = "content";
	std::vector<std::string> inVfsList; // processed .geometry artifacts
	std::string inVfs; // primary identity (first artifact / tour ref)
	std::string tour; // definition tour (editor scene overlay)
	std::string scratchDir; // artifact-cache root (empty = off)
	std::string outPak; // output host path
};

// Byte-identical to the CLI run (same return codes). Prints go through
// tool::out()/tool::err() so embedders can capture lines via
// tool::SetLineSink.
int Run(const PakArgs& args);

} // namespace paktool
