#pragma once
// Cook as a library (the editor links it; the CLI stays a thin user
// interface over the same Run). CookArgs carries only what embedders
// set (content root, tour-or-artifacts, scratch, quick mode); every
// other flag keeps its CLI default. Fill it directly instead of
// building argv strings.
#include <string>
#include <vector>

namespace cooktool {

struct CookArgs {
	std::string contentDir = "content";
	std::string tour; // definition tour (preferred; else inVfsList/inVfs)
	std::vector<std::string> inVfsList; // processed .geometry artifacts
	std::string inVfs; // primary identity (first artifact)
	std::string scratchDir; // artifact-cache root (empty = off)
	bool quickBakes = false; // capture-only refresh, no pak write
};

// Byte-identical to the CLI run (same return codes). Prints go through
// tool::out()/tool::err() so embedders can capture lines via
// tool::SetLineSink.
int Run(const CookArgs& args);

} // namespace cooktool
