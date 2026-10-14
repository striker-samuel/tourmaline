// RendergraphContractTests: CPU-testable frame-renderer contracts.
// The 14k-line WorldRenderer + 11k-line Vulkan impl have no GPU unit tests;
// these pin the parts that are pure logic so refactors fail fast:
//
//   1. Overlay-scratch discipline: every VS tail push lives in bytes
//      192..256 of IRenderer::PushConstants and fits (filtered 32B at 192,
//      mesh tail 16B at 224, uvflat 16B at 192, placement 32B at 192).
//   2. Mesh eligibility: MeshDrawEligible truth table (opaque-only,
//      single-sided) — the CPU partition feeds DrawMeshTasks exactly the
//      ranges the indexed loop skips (exactly-once).
//   3. GPU-driven culling is the only path (no kill switch, no legacy
//      CPU regime); RunConfig mesh default ON.
//   4. Filtered/motion tail prefix identity: FilteredVsPush's first 24B
//      (draws/renderViews/renderViewIndex at 192..216) match the motion
//      VS tail's reads (documented in gfx_filter.h:348-359).
//
// Heavier coverage (barrier emission, cull-builder differentials) follows
// the test_gfxfilter.cpp pattern; headless goldens via
// tools/golden_capture.py (VULKANTOY_CAPTURE + --bench + PNG hash).

#include <cstddef>
#include <cstdint>
#include <iostream>

#include <Engine/App/RunConfig.h>
#include <Engine/Placement/placement_draw.h>
#include <Engine/Renderer/Renderer.h>
#include <Engine/Renderer/gfx_filter.h>

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

} // namespace

int main() {
	using PC = engine::IRenderer::PushConstants;
	constexpr size_t kScratchStart = 192;
	constexpr size_t kScratchEnd = 256;

	// 1. Overlay-scratch discipline.
	Check(smvk::gfx::kFilteredVsPushOffset == kScratchStart, "contract.filtered-offset-192");
	Check(sizeof(smvk::gfx::FilteredVsPush) == 32, "contract.filtered-size-32");
	Check(smvk::gfx::kFilteredVsPushOffset + sizeof(smvk::gfx::FilteredVsPush) <= kScratchEnd,
		"contract.filtered-fits-scratch");
	Check(smvk::gfx::kMeshTailPushOffset == 224, "contract.meshtail-offset-224");
	Check(sizeof(smvk::gfx::MeshTailPush) == 16, "contract.meshtail-size-16");
	Check(smvk::gfx::kMeshTailPushOffset + sizeof(smvk::gfx::MeshTailPush) <= kScratchEnd,
		"contract.meshtail-fits-scratch");
	Check(smvk::gfx::kUvFlatVsPushOffset == kScratchStart, "contract.uvflat-offset-192");
	Check(sizeof(smvk::gfx::UvFlatVsPushTail) == 16, "contract.uvflat-size-16");
	Check(smvk::placement::kPlacementVsPushOffset == kScratchStart,
		"contract.placement-offset-192");
	Check(sizeof(smvk::placement::PlacementVsPush) == 32, "contract.placement-size-32");
	Check(smvk::placement::kPlacementVsPushOffset + sizeof(smvk::placement::PlacementVsPush) <= kScratchEnd,
		"contract.placement-fits-scratch");
	// Mesh tail stacks after the filtered tail (192+32=224).
	Check(smvk::gfx::kMeshTailPushOffset
		== smvk::gfx::kFilteredVsPushOffset + sizeof(smvk::gfx::FilteredVsPush),
		"contract.meshtail-stacks-after-filtered");
	Check(sizeof(smvk::gfx::GfxMeshParams) == 224, "contract.meshparams-224");

	// 2. Mesh eligibility truth table.
	Check(smvk::gfx::MeshDrawEligible(smvk::gfx::DrawSetBit_Opaque, 0),
		"contract.mesh-opaque-singlesided");
	Check(!smvk::gfx::MeshDrawEligible(smvk::gfx::DrawSetBit_Opaque,
		smvk::gfx::DrawFlagBit_TwoSided),
		"contract.mesh-twosided-excluded");
	Check(!smvk::gfx::MeshDrawEligible(smvk::gfx::DrawSetBit_Masked, 0),
		"contract.mesh-masked-excluded");
	Check(!smvk::gfx::MeshDrawEligible(0, 0), "contract.mesh-empty-set-excluded");

	// 3. GPU-driven culling is the only path (no kill switch, no legacy
	// CPU regime — the chain always runs; VULKANTOY_GFX_GPU_CULL is an
	// unknown var now).
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		const char* mesh = std::getenv("VULKANTOY_MESH");
		const bool expectMesh = !(mesh && std::string(mesh) == "0");
		Check((!c.meshSet && c.mesh) || (c.mesh == expectMesh),
			"contract.runconfig-mesh-default-on");
	}

	// 4. Filtered/motion tail prefix identity (192..216 shared).
	Check(offsetof(smvk::gfx::FilteredVsPush, draws) == 0, "contract.tail-draws-0");
	Check(offsetof(smvk::gfx::FilteredVsPush, renderViews) == 8,
		"contract.tail-renderviews-8");
	Check(offsetof(smvk::gfx::FilteredVsPush, renderViewIndex) == 16,
		"contract.tail-viewindex-16");
	// lmXforms rides the trailing word (216..224) the motion VS overwrites
	// with its FrameUniforms BDA — same offset, intentional alias.
	Check(offsetof(smvk::gfx::FilteredVsPush, lmXforms) == 24,
		"contract.tail-lmxforms-24");

	// Scratch invariant anchor: shared fields end exactly at 192.
	Check(offsetof(PC, prevModel) == kScratchStart, "contract.scratch-starts-192");
	Check(sizeof(PC) == kScratchEnd, "contract.push-ends-256");

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
