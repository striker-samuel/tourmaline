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
// the hardest), FUZZ_SWAP_EVERY (0 = off), FUZZ_MODE (rhi|graph|swap|all),
// FUZZ_SAME_SHAPE (0; 1 = the graph target recreates at the SAME extent -
// the identity-churn epoch hole, not the shape-churn path),
// FUZZ_TWO_GRAPH (0; 1 = a second graph candidate recreated at odd frames
// while the primary's placement stands - the epoch check is the only
// repack detector there).
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

#include <SDL3/SDL_video.h> // the window self-resize mode (SDL_SetWindowSize)

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
		// The window self-resize sizes: the creation size + a
		// maximized-ish alternate (the usable display bounds minus the
		// taskbar — the closest synthetic to the editor's
		// maximize/restore gesture).
		if (GetWindow()) {
			int bw = 0, bh = 0;
			SDL_GetWindowSize(GetWindow(), &bw, &bh);
			if (bw > 0 && bh > 0) {
				m_resizeBaseW = uint32_t(bw);
				m_resizeBaseH = uint32_t(bh);
			}
			SDL_DisplayID disp =
				SDL_GetDisplayForWindow(GetWindow());
			if (disp) {
				SDL_Rect usable{};
				if (SDL_GetDisplayUsableBounds(disp, &usable)) {
					m_resizeAltW = uint32_t(usable.w);
					m_resizeAltH = uint32_t(usable.h);
				}
			}
		}
		std::fprintf(stderr,
			"[FUZZ] mode=%s frames=%d every=%d swapEvery=%d "
			"resizeEvery=%d (%ux%u <-> %ux%u)\n",
			m_modeName, m_frames, m_every, m_swapEvery, m_resizeEvery,
			m_resizeBaseW, m_resizeBaseH, m_resizeAltW, m_resizeAltH);
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
		// FUZZ_SAME_SHAPE=1: the forced recreate keeps the SAME extent -
		// identical shape + lifetime, a NEW logical identity (the epoch
		// fingerprint's identity hole: pre-fix, destroy-A/create-B at the
		// same shape kept the epoch and the steady-state return stood on
		// stale placements - the new image never bound).
		if (!m_forceGraphRecreate
			&& m_graphExtent.width == w && m_graphExtent.height == h
			&& !m_graphView.empty())
			return true;
		m_forceGraphRecreate = false;
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

	// ---- candidate #2 (FUZZ_TWO_GRAPH=1): an independent graph target
	// recreated on the ODD cadence (one frame offset from the primary) so
	// a free never empties the placements map - the epoch check is the
	// ONLY repack detector in this regime, exercising the reviewer's
	// destroy-A/create-B identity hole directly.
	smvk::LogicalImageHandle m_graphImage2{};
	smvk::LogicalImageViewHandle m_graphView2{};
	bool m_twoGraph = false;
	bool EnsureGraphTarget2(uint32_t w, uint32_t h) {
		if (m_graphExtent2.width == w && m_graphExtent2.height == h
			&& !m_graphView2.empty())
			return true;
		if (!m_graphView2.empty())
			m_rendergraph->FreeImageView(m_graphView2);
		if (!m_graphImage2.empty())
			m_rendergraph->FreeImage(m_graphImage2);
		smvk::ImageCreateInfo ci{};
		ci.extent = { w, h, 1 };
		ci.format = m_swapchain->GetSurfaceFormat();
		ci.usage = smvk::ImageUsage::ColorAttachment;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		m_graphImage2 = m_rendergraph->AddImage(ci, "fuzzGraphTarget2");
		if (m_graphImage2.empty())
			return false;
		m_graphView2 = m_rendergraph->AddImageView(m_graphImage2,
			smvk::ImageViewType::View2D, smvk::Format::Undefined, {},
			"fuzzGraphTarget2View");
		if (m_graphView2.empty())
			return false;
		m_graphExtent2 = { w, h };
		return true;
	}

	// ---- claims mode: the DepthTarget churn pattern (depth.h, the
	// world renderer's actual target path): an UNBOUND CreateHeapImage
	// + ClaimTargetHeapRange + view + bindless, recreated at a toggling
	// extent, the claim Holder declared FIRST (destruction order: the
	// range free lands BEHIND the image's deferred destroy in the FIFO)
	// — verbatim depth.h. This is the EDITOR's death path (its world
	// chain claims at [TargetHeap], marching offsets, ~33 claims ->
	// DEVICE_LOST). FUZZ_CLAIM_TARGETS=<N> (default 1) churns N targets
	// per extent toggle — the world chain's ~30-target burst per
	// gesture, the remaining editor delta.
	struct ClaimTarget {
		smvk::Holder<smvk::TargetHeapClaimHandle> claim;
		smvk::Holder<smvk::ImageHandle> image;
		smvk::Holder<smvk::ImageViewHandle> view;
		smvk::Holder<smvk::BindlessImageHandle> bindless;
		smvk::LogicalImageViewHandle logical;
		smvk::Extent2D extent{ 0, 0 };
	};
	std::vector<ClaimTarget> m_claimTargets;
	uint32_t m_claimTargetCount = 1; // FUZZ_CLAIM_TARGETS
	bool EnsureClaimTarget(uint32_t w, uint32_t h) {
		if (m_claimTargets.size() != m_claimTargetCount)
			m_claimTargets.resize(m_claimTargetCount);
		bool allOk = true;
		for (auto& t : m_claimTargets) {
			if (t.extent.width == w && t.extent.height == h
				&& !t.view.empty())
				continue; // this one already at the right shape
			ClaimTarget fresh;
			fresh.image = m_context->CreateHeapImage(
				[] (uint32_t w, uint32_t h) {
					smvk::ImageCreateInfo ci{};
					ci.extent = { w, h, 1 };
					ci.format = smvk::Format::RGBA8_UNorm;
					ci.usage = smvk::ImageUsage::ColorAttachment
						| smvk::ImageUsage::Sampled;
					ci.mipLevels = 1;
					ci.arrayLayers = 1;
					ci.name = "fuzzClaimTarget";
					return ci;
				}(w, h));
			if (fresh.image.empty()) {
				allOk = false;
				break;
			}
			fresh.claim = m_context->ClaimTargetHeapRange(fresh.image,
				smvk::TargetPhase::Shadows,
				smvk::TargetPhase::Composite);
			if (fresh.claim.empty()) {
				allOk = false;
				break;
			}
			const smvk::ImageViewCreateInfo vci{
				.viewType = smvk::ImageViewType::View2D,
				.format = smvk::Format::Undefined,
				.subresourceRange = { smvk::ImageAspect::Color,
					0, 1, 0, 1 },
				.name = "fuzzClaimTargetView",
			};
			fresh.view = m_context->CreateImageView(fresh.image, vci);
			if (fresh.view.empty()) {
				allOk = false;
				break;
			}
			fresh.bindless = m_context->CreateBindlessImage(fresh.view,
				smvk::ImageLayout::ShaderReadOnlyOptimal);
			if (fresh.bindless.empty()) {
				allOk = false;
				break;
			}
			fresh.logical = m_rendergraph->AddExternalImageView(
				fresh.view, smvk::Format::RGBA8_UNorm,
				"fuzzClaimLogical");
			if (fresh.logical.empty()) {
				allOk = false;
				break;
			}
			if (!t.logical.empty())
				m_rendergraph->FreeImageView(t.logical);
			t = std::move(fresh);
			t.extent = { w, h };
		}
		return allOk;
	}

	// ---- CONCURRENT BUFFER CHURN (the world chain's missing resource
	// class): the world renderer's re-ensure recreates extent-sized
	// CONCURRENT buffers (ssrRayList/ssrRadiance/gfx hiz/ds counters —
	// CreateBuffer's Storage CONCURRENT path, families [graphics,
	// transfer]) at every extent change — the editor's Sponza.tour
	// mash died mid-burst exactly there (the last log lines before the
	// DEVICE_LOST). The fuzzer never churned buffers — images only.
	// FUZZ_CLAIM_BUFFERS=<N> churns N Storage buffers per extent toggle.
	struct ChurnBuffer {
		smvk::Holder<smvk::BufferHandle> buffer;
		smvk::Extent2D extent{ 0, 0 };
	};
	std::vector<ChurnBuffer> m_churnBuffers;
	uint32_t m_claimBufferCount = 0; // FUZZ_CLAIM_BUFFERS (0 = off)
	bool EnsureChurnBuffers(uint32_t w, uint32_t h) {
		if (m_churnBuffers.size() != m_claimBufferCount)
			m_churnBuffers.resize(m_claimBufferCount);
		bool allOk = true;
		for (auto& b : m_churnBuffers) {
			if (b.extent.width == w && b.extent.height == h
				&& !b.buffer.empty())
				continue;
			// Extent-scaled like the real ones (ssrRayList ~ 1KB/px-ish
			// rows, gfx hiz ~ 4B/px) — a few distinct sizes so the VMA
			// churn sees the real allocation mix.
			const size_t bytes = size_t(w) * h
				* (4 + 16 + 16 + 32);
			b.buffer = m_context->CreateBuffer(bytes,
				smvk::BufferType_Storage, "fuzzChurnBuffer");
			if (b.buffer.empty())
				allOk = false;
			b.extent = { w, h };
		}
		return allOk;
	}

	// ---- READBACK CHURN (the world renderer's per-frame async
	// readbacks — the last submit class the fuzzer never made):
	// SubmitAsyncDownloadDataFromBuffers over the churned extent-sized
	// buffer — the same RM API the world's DS-counter/SDSM/exposure
	// tickets use, riding the TRANSFER submitter's private timeline
	// (unordered from the swapchain's that keys the deferred retires).
	// The editor's death signature fits: the readback copy references
	// the OLD churned buffer 1-2 frames in flight; the destroy keys on
	// the swapchain's timeline; the readback's private timeline never
	// ordered with it -> USE AFTER DESTROY -> DEVICE LOST.
	uint32_t m_readbackEvery = 0; // FUZZ_READBACK (0 = off; every N frames)
	uint64_t m_readbackTicket = 0;
	std::vector<uint32_t> m_readbackScratch;
	void TickReadback() {
		// Poll the in-flight ticket (the world polls every frame —
		// the copy completes 1-2 frames later, exactly the gap the
		// churn exploit needs).
		if (m_readbackTicket != 0) {
			if (m_context->PollAsyncDownloadDataFromBuffers(
					m_readbackTicket,
					std::span<const smvk::BufferDownload>{}))
				m_readbackTicket = 0;
		}
		// Submit the next copy over the FIRST churn buffer (the
		// extent-sized Storage buffer the churn recreates each toggle —
		// the destroyed-old-buffer reference the death needs).
		if (m_readbackEvery > 0 && m_readbackTicket == 0
			&& m_frame % uint32_t(m_readbackEvery) == 0
			&& !m_churnBuffers.empty()
			&& !m_churnBuffers[0].buffer.empty()) {
			m_readbackScratch.assign(1024, 0);
			smvk::BufferDownload dl{
				m_churnBuffers[0].buffer,
				m_readbackScratch.data(),
				m_readbackScratch.size() * sizeof(uint32_t), 0 };
			m_readbackTicket = m_context
				->SubmitAsyncDownloadDataFromBuffers(
					std::span<const smvk::BufferDownload>(&dl, 1));
		}
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
		// ---- WINDOW SELF-RESIZE (the WM-driven gesture path — the
		// editor's maximize/restore crash class, WITHOUT the editor):
		// FUZZ_RESIZE_STYLE (default 'jump') picks the gesture form:
		//   jump: one SetWindowSize per cadence — a maximize/restore
		//         (single big deltas, settle between).
		//   drag: a small delta EVERY FRAME (the drag storm — the WM
		//         fires a resize event per intermediate size, the flag
		//         recreates the swapchain at EVERY intermediate: the
		//         real window-edge-drag path; a user drag = hundreds of
		//         intermediates, and the recreate SHOULD only happen at
		//         the release — if the storm form dies, the engine is
		//         recreating per-event instead of debouncing to the
		//         settle). Ping-pongs within [base, alt].
		if (m_resizeEvery > 0 && m_frame > 0
			&& m_frame % uint32_t(m_resizeEvery) == 0) {
			if (m_resizeDrag) {
				m_resizePing = !m_resizePing;
				int cw = 0, ch = 0;
				SDL_GetWindowSize(GetWindow(), &cw, &ch);
				if (cw <= 0 || ch <= 0) {
					cw = int(m_resizeBaseW);
					ch = int(m_resizeBaseH);
				}
				const int dir = m_resizePing ? 1 : -1;
				const int stepW = 32, stepH = 18;
				int nw = cw + dir * stepW, nh = ch + dir * stepH;
				if (nw > int(m_resizeAltW)) {
					nw = int(m_resizeAltW);
					m_resizePing = false; // clamp + turn around
				}
				if (nw < int(m_resizeBaseW) / 2) {
					nw = int(m_resizeBaseW) / 2;
					m_resizePing = true;
				}
				if (nh > int(m_resizeAltH)) {
					nh = int(m_resizeAltH);
					m_resizePing = false;
				}
				if (nh < int(m_resizeBaseH) / 2) {
					nh = int(m_resizeBaseH) / 2;
					m_resizePing = true;
				}
				SDL_SetWindowSize(GetWindow(), nw, nh);
			}
			else {
				m_resizeToggle = !m_resizeToggle;
				SDL_SetWindowSize(GetWindow(),
					int(m_resizeToggle ? m_resizeAltW : m_resizeBaseW),
					int(m_resizeToggle ? m_resizeAltH : m_resizeBaseH));
			}
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
		if (m_every > 0 && m_frame % uint32_t(m_every) == 0) {
			if (m_sameShape)
				m_forceGraphRecreate = true; // same shape, new identity
			else
				m_toggle = !m_toggle;
		}
		if (m_passToggleEvery > 0 && m_frame > 0
			&& m_frame % uint32_t(m_passToggleEvery) == 0)
			m_extraPass = !m_extraPass;
		const uint32_t* e = m_toggle ? kFuzzExtentA : kFuzzExtentB;
		const uint32_t w = e[0], h = e[1];

		if (m_modeRhi)
			EnsureRhiTarget(w, h);
		if (m_modeGraph) {
			EnsureGraphTarget(w, h);
			if (m_twoGraph)
				EnsureGraphTarget2(w, h);
		}
		// the ODD-cadence churn of candidate #2: same shape most frames,
		// recreated at odd offsets - when it frees, the primary's
		// placement still stands (placements non-empty at compile).
		if (m_twoGraph && m_modeGraph && (m_frame & 1) == 1) {
			if (!m_graphView2.empty())
				m_rendergraph->FreeImageView(m_graphView2);
			if (!m_graphImage2.empty())
				m_rendergraph->FreeImage(m_graphImage2);
			smvk::ImageCreateInfo ci2{};
			ci2.extent = { w, h, 1 };
			ci2.format = m_swapchain->GetSurfaceFormat();
			ci2.usage = smvk::ImageUsage::ColorAttachment;
			ci2.mipLevels = 1;
			ci2.arrayLayers = 1;
			m_graphImage2 = m_rendergraph->AddImage(ci2,
				"fuzzGraphTarget2");
			if (!m_graphImage2.empty())
				m_graphView2 = m_rendergraph->AddImageView(m_graphImage2,
					smvk::ImageViewType::View2D, smvk::Format::Undefined, {},
					"fuzzGraphTarget2View");
			m_graphExtent2 = { w, h };
		}
		if (m_modeClaims) {
			EnsureClaimTarget(w, h);
			if (m_claimBufferCount > 0)
				EnsureChurnBuffers(w, h);
			// The readback tick BEFORE the pass churn (the world polls
			// first thing in its update; the copy in flight spans the
			// churn recreates — the destroyed-old-buffer window).
			if (m_readbackEvery > 0)
				TickReadback();
		}

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
			if (m_modeClaims) {
				// A clear pass over EVERY claim target — the world
				// chain's whole-set write per frame (all N targets
				// touched, exactly like the world's passes).
				for (const auto& t : m_claimTargets) {
					if (t.logical.empty())
						continue;
					m_rendergraph->AddPass("FuzzClaimClear")
						.SetOffset(0, 0)
						.SetExtent(smvk::Extent2D{ w, h })
						.SetColor({ t.logical },
							smvk::AttachmentLoadOp::Clear,
							smvk::AttachmentStoreOp::Store,
							smvk::ClearColor(0.2f, 0.3f, 0.4f, 1.0f))
						.SetRecordingFunc([](smvk::CommandBuffer& cmd) {
							(void)cmd;
						});
				}
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
				// FUZZ_PASS_TOGGLE: a SECOND clear pass over the SAME target
				// on alternate cadences - the pass count + the target's use
				// interval change while its placement stays alive, which is
				// the in-compile DETONATION branch (the repack-with-
				// live-placements path; the extent toggle never hits it - it
				// frees the target first, so placements are empty at
				// compile). Watch stderr for the "repacked" tags.
				if (m_extraPass) {
					m_rendergraph->AddPass("FuzzGraphClear2")
						.SetOffset(0, 0)
						.SetExtent(smvk::Extent2D{ w, h })
						.SetColor({ m_graphView },
							smvk::AttachmentLoadOp::Load,
							smvk::AttachmentStoreOp::Store,
							smvk::ClearColor(0.3f, 0.2f, 0.1f, 1.0f))
						.SetRecordingFunc([](smvk::CommandBuffer& cmd) {
							(void)cmd;
						});
				}
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
	int m_resizeEvery = 0;      // FUZZ_RESIZE_EVERY: the window self-resize cadence (0 = off)
	bool m_modeRhi = false;
	bool m_modeGraph = false;
	bool m_modeClaims = false;
	char m_modeBuf[16]{};
	const char* m_modeName = m_modeBuf;
	uint32_t m_frame = 0;
	bool m_toggle = false;
	bool m_resizeToggle = false;
	bool m_resizePing = true;   // the drag form's direction
	bool m_resizeDrag = false; // FUZZ_RESIZE_STYLE=drag: the storm form
	uint32_t m_resizeBaseW = 1280, m_resizeBaseH = 720;   // captured at init
	uint32_t m_resizeAltW = 1600, m_resizeAltH = 900;     // maximized-ish alternate
	bool m_graphOnly = false; // FUZZ_GRAPH_ONLY=1: no swap pass (bisect knob)
	bool m_sameShape = false; // FUZZ_SAME_SHAPE=1: recreate at the SAME extent (identity churn)
	bool m_forceGraphRecreate = false;
	int m_passToggleEvery = 0; // FUZZ_PASS_TOGGLE: flip an extra pass every N frames (the detonation branch)
	bool m_extraPass = false;
	bool m_attachRhi = false; // FUZZ_ATTACH_RHI=1: the external RHI target as the attachment (the control)
	bool m_hostTransfer = false; // FUZZ_HOST_TRANSFER=1: the graph target on the VMA path (the control)

	// the rhi churn target (the inline-target pattern)
	smvk::Holder<smvk::ImageHandle> m_rhiImage;
	smvk::Holder<smvk::ImageViewHandle> m_rhiView;
	smvk::Holder<smvk::BindlessImageHandle> m_rhiBindless;
	uint32_t m_rhiBindlessIndex = UINT32_MAX;
	smvk::LogicalImageViewHandle m_rhiLogical{};
	smvk::Extent2D m_rhiExtent{ 0, 0 };

	// the claims churn targets live in the EnsureClaimTarget section
	// (std::vector<ClaimTarget> m_claimTargets — N per the
	// FUZZ_CLAIM_TARGETS knob, the world chain's multi-target burst)

	// the graph churn target (the transient candidate)
	smvk::LogicalImageHandle m_graphImage{};
	smvk::LogicalImageViewHandle m_graphView{};
	smvk::Extent2D m_graphExtent{ 0, 0 };
	smvk::Extent2D m_graphExtent2{ 0, 0 };

	std::vector<smvk::LogicalImageViewHandle> m_swapViews;

public:
	int RunFuzz() {
		m_frames = EnvInt("FUZZ_FRAMES", 600);
		m_every = EnvInt("FUZZ_EVERY", 1);
		m_swapEvery = EnvInt("FUZZ_SWAP_EVERY", 0);
		m_resizeEvery = EnvInt("FUZZ_RESIZE_EVERY", 0);
		m_claimTargetCount = uint32_t(
			EnvInt("FUZZ_CLAIM_TARGETS", 1));
		m_claimBufferCount = uint32_t(
			EnvInt("FUZZ_CLAIM_BUFFERS", 0));
		m_readbackEvery = uint32_t(
			EnvInt("FUZZ_READBACK", 0));
		m_graphOnly = EnvInt("FUZZ_GRAPH_ONLY", 0) != 0;
		m_sameShape = EnvInt("FUZZ_SAME_SHAPE", 0) != 0;
		m_passToggleEvery = EnvInt("FUZZ_PASS_TOGGLE", 0);
		m_twoGraph = EnvInt("FUZZ_TWO_GRAPH", 0) != 0;
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
		const bool modeResize = !std::strcmp(m_modeName, "resize");
		if (modeResize && m_resizeEvery <= 0)
			m_resizeEvery = 60; // a gesture-per-second cadence by default
		if (const char* style = std::getenv("FUZZ_RESIZE_STYLE"))
			m_resizeDrag = !std::strcmp(style, "drag");
		if (!m_modeRhi && !m_modeGraph && !m_modeClaims && !modeSwap
			&& !modeResize) {
			std::fprintf(stderr, "[FUZZ] unknown mode '%s' "
				"(rhi|graph|claims|swap|resize|all)\n", m_modeName);
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
			"(mode=%s every=%d swapEvery=%d resizeEvery=%d)\n", m_frame,
			m_modeName, m_every, m_swapEvery, m_resizeEvery);
		return EXIT_SUCCESS;
	}
};

int main() {
	FuzzApp app;
	return app.RunFuzz();
}
