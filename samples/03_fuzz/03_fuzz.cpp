// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// 03_fuzz: a MINIMAL hard-fuzz host for the target-recreate /
// deferred-destroy machinery — the surface the editor's WYSIWYG live
// view exposed (resize churn -> VK_ERROR_DEVICE_LOST around the ~29th
// target-heap claim; the heap claim offsets march monotonically = the
// deferred vmaVirtualFree tasks never execute).
//
// The whole point is MINIMALITY: no editor, no ImGui panels, no scene
// import, no world — just the App skeleton + the three failing
// mechanisms, one per fuzz mode, so the repro is fast and the signal
// unburied:
//
//   rhi   — the inline-target churn pattern verbatim: a private image +
//          view + bindless + graph-external logical recreated at a
//          TOGGLING extent (two shapes), the old objects retired through
//          the Holder-swap deferred-destroy queue, ProcessDeferredTasks
//          every frame (the editor's exact cadence).
//   graph — the rendergraph transient churn: a graph-MANAGED image
//          (AddImage — a CompileTransientHeap candidate) recreated at a
//          toggling extent + a clear pass per frame + Execute — the
//          detonate/repack path the world renderer's target chain rides.
//   swap  — swapchain Recreate spam (the user-resize path: swap retire +
//          external swap-view re-import + the format drift).
//   all   — everything at once (default — the full editor-equivalent
//          stress without the editor).
//
// Knobs (env): FUZZ_FRAMES (600), FUZZ_EVERY (1 = recreate every frame,
// the hardest), FUZZ_SWAP_EVERY (0 = off), FUZZ_MODE (rhi|graph|swap|all).
//
// Verdict: the engine's own loud paths report failure (DEVICE_LOST at a
// submit, the [TargetHeap] abort); a clean exit prints [FUZZ] PASS.
// Watch stderr's [TargetHeap] claim offsets while it runs: offsets that
// MARCH MONOTONICALLY (never reuse the low freed ranges) are the
// undrained-deferred-queue signature — the fix makes them flat/reusing.

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <Engine/core.h>
#include <Engine/Rendergraph/rendergraph.h>

namespace {

constexpr uint32_t kFuzzExtentA[2] = { 620, 358 };
constexpr uint32_t kFuzzExtentB[2] = { 668, 406 };

int EnvInt(const char* name, int def) {
	const char* e = std::getenv(name);
	return e ? std::atoi(e) : def;
}

} // namespace

class FuzzApp : public smvk::App {
	using Super = smvk::App;

public:
	FuzzApp() = default;

private:
	bool Initialize() override {
		if (!Super::Initialize())
			return false;
		ImportSwapViews();
		std::fprintf(stderr,
			"[FUZZ] mode=%s frames=%d every=%d swapEvery=%d\n",
			m_modeName, m_frames, m_every, m_swapEvery);
		return true;
	}

	void ImportSwapViews() {
		for (auto h : m_swapViews)
			m_rendergraph->FreeImageView(h);
		m_swapViews.clear();
		for (uint32_t i = 0; i < m_swapchain->GetImageCount(); ++i)
			m_swapViews.push_back(m_rendergraph->AddExternalImageView(
				m_swapchain->GetImageViewHandle(i),
				m_swapchain->GetSurfaceFormat(), "fuzzSwap"));
	}

	// ---- rhi mode: the inline-target churn pattern ------------------
	// (EnsureInlineTarget's shape, verbatim: create new objects, free the
	// old LOGICAL in the graph, Holder-move-assign the new handles in so
	// the old ones retire deferred at scope exit — the in-flight-safe
	// swap.)
	bool EnsureRhiTarget(uint32_t w, uint32_t h) {
		if (m_rhiExtent.width == w && m_rhiExtent.height == h
			&& !m_rhiLogical.empty())
			return true;
		const smvk::Format fmt = m_swapchain->GetSurfaceFormat();
		smvk::ImageCreateInfo ci{};
		ci.extent = { w, h, 1 };
		ci.format = fmt;
		ci.usage = smvk::ImageUsage::ColorAttachment
			| smvk::ImageUsage::Sampled;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "fuzzRhiTarget";
		auto newImage = m_context->CreateImage(ci, nullptr);
		if (newImage.empty())
			return false;
		const smvk::ImageViewCreateInfo vci{
			.viewType = smvk::ImageViewType::View2D,
			.format = fmt,
			.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 },
			.name = "fuzzRhiTargetView",
		};
		auto newView = m_context->CreateImageView(newImage, vci);
		if (newView.empty())
			return false;
		auto newBindless = m_context->CreateBindlessImage(newView,
			smvk::ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex =
			m_context->GetBindlessIndex(newBindless);
		const auto newLogical = m_rendergraph->AddExternalImageView(
			newView, fmt, "fuzzRhiLogical");
		if (newLogical.empty())
			return false;
		if (!m_rhiLogical.empty())
			m_rendergraph->FreeImageView(m_rhiLogical);
		m_rhiBindless = std::move(newBindless);
		m_rhiBindlessIndex = newBindlessIndex;
		m_rhiView = std::move(newView);
		m_rhiImage = std::move(newImage);
		m_rhiLogical = newLogical;
		m_rhiExtent = { w, h };
		return true;
	}

	// ---- graph mode: the graph-managed transient churn ----------------
	// (the CompileTransientHeap candidate path: a graph-owned image
	// re-created at a toggling extent -> the packed-set epoch changes ->
	// detonate + repack -> [TargetHeap] claims + deferred frees.)
	bool EnsureGraphTarget(uint32_t w, uint32_t h) {
		if (m_graphExtent.width == w && m_graphExtent.height == h
			&& !m_graphView.empty())
			return true;
		if (!m_graphView.empty())
			m_rendergraph->FreeImageView(m_graphView);
		if (!m_graphImage.empty())
			m_rendergraph->FreeImage(m_graphImage);
		smvk::ImageCreateInfo ci{};
		ci.extent = { w, h, 1 };
		ci.format = m_swapchain->GetSurfaceFormat();
		ci.usage = smvk::ImageUsage::ColorAttachment;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.hostTransfer = m_hostTransfer; // FUZZ_HOST_TRANSFER=1: VMA path
		m_graphImage = m_rendergraph->AddImage(ci, "fuzzGraphTarget");
		if (m_graphImage.empty())
			return false;
		m_graphView = m_rendergraph->AddImageView(m_graphImage,
			smvk::ImageViewType::View2D, smvk::Format::Undefined, {},
			"fuzzGraphTargetView");
		if (m_graphView.empty())
			return false;
		m_graphExtent = { w, h };
		return true;
	}

	// ---- claims mode: the DepthTarget churn pattern (depth.h, the
	// world renderer's actual target path): an UNBOUND CreateHeapImage
	// + ClaimTargetHeapRange + view + bindless, recreated at a toggling
	// extent, the claim Holder declared FIRST (destruction order: the
	// range free lands BEHIND the image's deferred destroy in the FIFO)
	// — verbatim depth.h. This is the EDITOR's death path (its world
	// chain claims at [TargetHeap], marching offsets, ~33 claims ->
	// DEVICE_LOST).
	bool EnsureClaimTarget(uint32_t w, uint32_t h) {
		if (m_claimExtent.width == w && m_claimExtent.height == h
			&& !m_claimView.empty())
			return true;
		// LOCAL DECLARATION ORDER MATTERS (depth.h): newClaim FIRST
		// destroys LAST - the free lands behind the image destroy.
		smvk::Holder<smvk::TargetHeapClaimHandle> newClaim;
		smvk::ImageCreateInfo ci{};
		ci.extent = { w, h, 1 };
		ci.format = smvk::Format::RGBA8_UNorm;
		ci.usage = smvk::ImageUsage::ColorAttachment
			| smvk::ImageUsage::Sampled;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "fuzzClaimTarget";
		auto newImage = m_context->CreateHeapImage(ci);
		if (newImage.empty())
			return false;
		newClaim = m_context->ClaimTargetHeapRange(newImage,
			smvk::TargetPhase::Shadows, smvk::TargetPhase::Composite);
		if (newClaim.empty())
			return false;
		const smvk::ImageViewCreateInfo vci{
			.viewType = smvk::ImageViewType::View2D,
			.format = smvk::Format::Undefined,
			.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 },
			.name = "fuzzClaimTargetView",
		};
		auto newView = m_context->CreateImageView(newImage, vci);
		if (newView.empty())
			return false;
		auto newBindless = m_context->CreateBindlessImage(newView,
			smvk::ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex =
			m_context->GetBindlessIndex(newBindless);
		const auto newLogical = m_rendergraph->AddExternalImageView(
			newView, smvk::Format::RGBA8_UNorm, "fuzzClaimLogical");
		if (newLogical.empty())
			return false;
		if (!m_claimLogical.empty())
			m_rendergraph->FreeImageView(m_claimLogical);
		m_claimClaim = std::move(newClaim);
		m_claimBindless = std::move(newBindless);
		m_claimBindlessIndex = newBindlessIndex;
		m_claimView = std::move(newView);
		m_claimImage = std::move(newImage);
		m_claimLogical = newLogical;
		m_claimExtent = { w, h };
		return true;
	}

	void DrawFrame() override {
		// ---- resize handling (the base's default pump does this; a
		// custom DrawFrame must replicate it or the WM's boot-time
		// window settle rebuilds the swapchain under stale imported
		// views — recording a destroyed image = DEVICE LOST, which
		// cost a false bisect verdict): rebuild + re-import whenever
		// the flag is set, then the explicit swap-churn mode's own
		// recreate below.
		if (m_swapchain->FramebufferResized()) {
			m_swapchain->Recreate();
			m_swapchain->ClearFramebufferResized();
			ImportSwapViews();
		}
		// ---- swap churn (the resize path): Recreate at the frame
		// boundary (the sync-legal spot), re-import the swap views.
		if (m_swapEvery > 0 && m_frame > 0
			&& m_frame % uint32_t(m_swapEvery) == 0) {
			m_swapchain->Recreate();
			m_swapchain->ClearFramebufferResized();
			ImportSwapViews();
		}
		auto fb = m_swapchain->AcquireFramebuffer();
		const smvk::Extent2D swapExt = m_swapchain->GetExtent();
		// ---- the extent toggle: every FUZZ_EVERY frames the targets
		// swap shape (A <-> B) — a NEW heap shape each recreate, the
		// monotone-offset driver.
		if (m_every > 0 && m_frame % uint32_t(m_every) == 0)
			m_toggle = !m_toggle;
		const uint32_t* e = m_toggle ? kFuzzExtentA : kFuzzExtentB;
		const uint32_t w = e[0], h = e[1];

		if (m_modeRhi)
			EnsureRhiTarget(w, h);
		if (m_modeGraph)
			EnsureGraphTarget(w, h);
		if (m_modeClaims)
			EnsureClaimTarget(w, h);

		// ---- the frame graph: a clear pass over the graph-managed
		// target (the transient-heap compile/detonate exercise) + the
		// triangle-style swap clear when no graph target exists.
		// FUZZ_ATTACH_RHI=1 (the bisect control): the RHI mode's
		// EXTERNAL target becomes the pass attachment instead — the
		// same write through the non-heap path; separates "any pass
		// writing a private image dies" from "the transient-heap
		// binding dies".
		m_rendergraph->NewFrame();
		{
			if (m_attachRhi && !m_rhiLogical.empty()) {
				m_rendergraph->AddPass("FuzzRhiClear")
					.SetOffset(0, 0)
					.SetExtent(smvk::Extent2D{ w, h })
					.SetColor({ m_rhiLogical },
						smvk::AttachmentLoadOp::Clear,
						smvk::AttachmentStoreOp::Store,
						smvk::ClearColor(0.1f, 0.2f, 0.3f, 1.0f))
					.SetRecordingFunc([](smvk::CommandBuffer& cmd) {
						(void)cmd;
					});
			}
			if (m_modeClaims && !m_claimLogical.empty()) {
				m_rendergraph->AddPass("FuzzClaimClear")
					.SetOffset(0, 0)
					.SetExtent(smvk::Extent2D{ w, h })
					.SetColor({ m_claimLogical },
						smvk::AttachmentLoadOp::Clear,
						smvk::AttachmentStoreOp::Store,
						smvk::ClearColor(0.2f, 0.3f, 0.4f, 1.0f))
					.SetRecordingFunc([](smvk::CommandBuffer& cmd) {
						(void)cmd;
					});
			}
			if (m_modeGraph && !m_graphView.empty()) {
				m_rendergraph->AddPass("FuzzGraphClear")
					.SetOffset(0, 0)
					.SetExtent(smvk::Extent2D{ w, h })
					.SetColor({ m_graphView },
						smvk::AttachmentLoadOp::Clear,
						smvk::AttachmentStoreOp::Store,
						smvk::ClearColor(0.1f, 0.2f, 0.3f, 1.0f))
					.SetRecordingFunc([](smvk::CommandBuffer& cmd) {
						(void)cmd;
					});
			}
			if (!m_swapViews.empty() && fb.imageIndex < m_swapViews.size()
				&& !m_graphOnly) {
				m_rendergraph->AddPass("FuzzSwapClear")
					.SetOffset(0, 0)
					.SetExtent(swapExt)
					.SetColor({ m_swapViews[fb.imageIndex] },
						smvk::AttachmentLoadOp::Clear,
						smvk::AttachmentStoreOp::Store,
						smvk::ClearColor(0.05f, 0.05f, 0.05f, 1.0f))
					.SetRecordingFunc([](smvk::CommandBuffer& cmd) {
						(void)cmd;
					});
			}
		}
		m_rendergraph->Execute(fb);
		// The editor's drain cadence: the deferred-destroy queue should
		// retire the churned objects a few frames after each recreate.
		m_context->ProcessDeferredTasks();
		m_swapchain->EndAndPresentFramebuffer(fb);

		if (m_frame % 60 == 0)
			std::fprintf(stderr, "[FUZZ] frame %u ok (target %ux%u)\n",
				m_frame, w, h);
		++m_frame;
	}

private:
	// knobs
	int m_frames = 600;
	int m_every = 1;
	int m_swapEvery = 0;
	bool m_modeRhi = false;
	bool m_modeGraph = false;
	bool m_modeClaims = false;
	char m_modeBuf[16]{};
	const char* m_modeName = m_modeBuf;
	uint32_t m_frame = 0;
	bool m_toggle = false;
	bool m_graphOnly = false; // FUZZ_GRAPH_ONLY=1: no swap pass (bisect knob)
	bool m_attachRhi = false; // FUZZ_ATTACH_RHI=1: the external RHI target as the attachment (the control)
	bool m_hostTransfer = false; // FUZZ_HOST_TRANSFER=1: the graph target on the VMA path (the control)

	// the rhi churn target (the inline-target pattern)
	smvk::Holder<smvk::ImageHandle> m_rhiImage;
	smvk::Holder<smvk::ImageViewHandle> m_rhiView;
	smvk::Holder<smvk::BindlessImageHandle> m_rhiBindless;
	uint32_t m_rhiBindlessIndex = UINT32_MAX;
	smvk::LogicalImageViewHandle m_rhiLogical{};
	smvk::Extent2D m_rhiExtent{ 0, 0 };

	// the claims churn target (the DepthTarget pattern — the editor's
	// world-chain path; the claim Holder FIRST so its free lands behind
	// the image destroy at retirement)
	smvk::Holder<smvk::TargetHeapClaimHandle> m_claimClaim;
	smvk::Holder<smvk::ImageHandle> m_claimImage;
	smvk::Holder<smvk::ImageViewHandle> m_claimView;
	smvk::Holder<smvk::BindlessImageHandle> m_claimBindless;
	uint32_t m_claimBindlessIndex = UINT32_MAX;
	smvk::LogicalImageViewHandle m_claimLogical{};
	smvk::Extent2D m_claimExtent{ 0, 0 };

	// the graph churn target (the transient candidate)
	smvk::LogicalImageHandle m_graphImage{};
	smvk::LogicalImageViewHandle m_graphView{};
	smvk::Extent2D m_graphExtent{ 0, 0 };

	std::vector<smvk::LogicalImageViewHandle> m_swapViews;

public:
	int RunFuzz() {
		m_frames = EnvInt("FUZZ_FRAMES", 600);
		m_every = EnvInt("FUZZ_EVERY", 1);
		m_swapEvery = EnvInt("FUZZ_SWAP_EVERY", 0);
		m_graphOnly = EnvInt("FUZZ_GRAPH_ONLY", 0) != 0;
		m_attachRhi = EnvInt("FUZZ_ATTACH_RHI", 0) != 0;
		m_hostTransfer = EnvInt("FUZZ_HOST_TRANSFER", 0) != 0;
		if (const char* mode = std::getenv("FUZZ_MODE"))
			std::snprintf(m_modeBuf, sizeof(m_modeBuf), "%s", mode);
		else
			std::snprintf(m_modeBuf, sizeof(m_modeBuf), "all");
		m_modeName = m_modeBuf;
		m_modeRhi = !std::strcmp(m_modeName, "rhi")
			|| !std::strcmp(m_modeName, "all");
		m_modeGraph = !std::strcmp(m_modeName, "graph")
			|| !std::strcmp(m_modeName, "all");
		m_modeClaims = !std::strcmp(m_modeName, "claims");
		const bool modeSwap = !std::strcmp(m_modeName, "swap")
			|| !std::strcmp(m_modeName, "all");
		if (modeSwap && m_swapEvery <= 0)
			m_swapEvery = 137; // a resize-ish cadence when the mode asks for it
		if (!m_modeRhi && !m_modeGraph && !m_modeClaims && !modeSwap) {
			std::fprintf(stderr, "[FUZZ] unknown mode '%s' "
				"(rhi|graph|swap|all)\n", m_modeName);
			return EXIT_FAILURE;
		}
		// The App's bench budget is the auto-quit: 10 warmup + N
		// measured frames, then Run() returns and the verdict prints.
		SetFrameBudget(m_frames + 10);
		try {
			Run();
		}
		catch (const std::exception& e) {
			std::fprintf(stderr, "[FUZZ] FAIL: exception: %s\n", e.what());
			return EXIT_FAILURE;
		}
		std::fprintf(stderr, "[FUZZ] PASS: ~%u frames survived "
			"(mode=%s every=%d swapEvery=%d)\n", m_frame, m_modeName,
			m_every, m_swapEvery);
		return EXIT_SUCCESS;
	}
};

int main() {
	FuzzApp app;
	return app.RunFuzz();
}
