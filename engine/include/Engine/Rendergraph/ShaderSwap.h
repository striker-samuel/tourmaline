// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE PER-BUILDER HOT-RELOAD SWAP HELPER (the consumer leg's second
// wave — Utils/shader_hot_reload.h): one shared, exact implementation
// of the module swap every rendergraph builder's ReloadShaders uses.
//
// The contract (mirrors WorldRenderer::ReloadLitShaders's per-module
// discipline):
//   - FRESH-FIRST: the new module is created BEFORE the old handle is
//     touched, so a failed read keeps the standing module — the
//     consumer-side last-known-good (the service already held the
//     last-known-good FILE; this is its pipeline twin).
//   - RELEASE, never assign-over: the old handle is RELEASED from its
//     Holder into the caller's retire list (an empty-assign would
//     destroy the module IMMEDIATELY; the retirement belongs behind
//     the in-flight frames through RenderGraph::RetireShaderModule +
//     the deferred queue).
//   - The read goes through smvk::shaders (the unified overlay): the
//     dev-override tree where the reloader lands fresh bytes serves
//     FIRST, and the bytecode magic is validated before it reaches a
//     module — a corrupt blob fails HERE with a named provider, never
//     inside a driver.

#include <Engine/RHI/resource_manager.h>
#include <Engine/Utils/shader_library.h>

#include <utility>
#include <vector>

namespace smvk {

// Swaps ONE builder shader module. Returns true when the member now
// holds a fresh module (false = the read failed, the standing module
// kept). `retired` collects the released old handles — the caller owns
// the retirement pass.
[[nodiscard]] inline bool SwapShaderModule(ResourceManager& ctx,
	const char* path, const char* debugName,
	Holder<ShaderModuleHandle>& member,
	std::vector<ShaderModuleHandle>& retired)
{
	Holder<ShaderModuleHandle> fresh;
	try {
		fresh = ctx.CreateShaderModule(shaders::Read(path), debugName);
	}
	catch (...) {
		return false; // last-known-good: the standing module stays
	}
	if (fresh.empty())
		return false;
	if (!member.empty())
		retired.push_back(member.release());
	member = std::move(fresh);
	return true;
}

} // namespace smvk
