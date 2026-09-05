#include <Engine/Renderer/gfx_filter.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Renderer/mesh.h>
#include "../Vulkan/detail/vulkan_impl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace smvk {
namespace gfx {

static constexpr size_t kBatchBufferBytes = kMaxBatches * sizeof(Batch);
static constexpr size_t kDrawBufferBytes = kMaxDraws * sizeof(Draw);
static constexpr size_t kFilteredBytes = kMaxBatches * kMaxClusterTris * 3 * sizeof(uint32_t);
static constexpr size_t kIndirectBytes = kMaxBatches * 20; // VkDrawIndexedIndirectCommand

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool EnsureShaders(State& s, ResourceManager& ctx, const char* csPath) {
	if (s.filterProgram.shaderCount != 0 && !s.filterModule.empty())
		return true;
	try {
		if (s.filterModule.empty())
			s.filterModule = ctx.CreateShaderModule(
				readFile(csPath ? csPath : kGfxFilterCsPath), "gfxFilterCS");
	}
	catch (...) {
		return false;
	}
	if (s.filterModule.empty())
		return false;
	Shader cs{ .module = s.filterModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
	s.filterProgram = { .shaders = { cs }, .shaderCount = 1 };
	return true;
}

bool Ensure(State& s, ResourceManager& ctx, RenderGraph& rg, const char* csPath) {
	if (!s.batchBuffer.empty() && !s.drawBuffer.empty()) {
		if (s.batchLogical.empty())
			s.batchLogical = rg.AddExternalBuffer(s.batchBuffer, "gfx batches");
		if (s.drawLogical.empty())
			s.drawLogical = rg.AddExternalBuffer(s.drawBuffer, "gfx draws");
		return EnsureShaders(s, ctx, csPath);
	}
	s.batchBuffer = ctx.CreateBuffer(kBatchBufferBytes, BufferType_Storage, "gfx batches");
	s.drawBuffer = ctx.CreateBuffer(kDrawBufferBytes, BufferType_Storage, "gfx draws");
	if (s.batchBuffer.empty() || s.drawBuffer.empty()) {
		Destroy(s, rg);
		return false;
	}
	s.batchLogical = rg.AddExternalBuffer(s.batchBuffer, "gfx batches");
	s.drawLogical = rg.AddExternalBuffer(s.drawBuffer, "gfx draws");
	if (s.batchLogical.empty() || s.drawLogical.empty()) {
		Destroy(s, rg);
		return false;
	}
	return EnsureShaders(s, ctx, csPath);
}

void Destroy(State& s, RenderGraph& rg) {
	if (!s.batchLogical.empty()) rg.FreeBuffer(s.batchLogical);
	if (!s.drawLogical.empty()) rg.FreeBuffer(s.drawLogical);
	if (!s.filterIndexLogical.empty()) rg.FreeBuffer(s.filterIndexLogical);
	if (!s.clusterLogical.empty()) rg.FreeBuffer(s.clusterLogical);
	s.batchLogical = {};
	s.drawLogical = {};
	s.filterIndexLogical = {};
	s.clusterLogical = {};
	for (auto& v : s.views) {
		if (!v.filteredLogical.empty()) rg.FreeBuffer(v.filteredLogical);
		if (!v.indirectLogical.empty()) rg.FreeBuffer(v.indirectLogical);
	}
	s.views.clear();
	s.viewCount = 0;
	s.filterIndexBuffer = {};
	s.clusterBuffer = {};
	s.batchBuffer = {};
	s.drawBuffer = {};
	s.filterProgram = ShaderProgram{};
	s.filterModule = {};
}

// ---------------------------------------------------------------------------
// Registration (load-time)
// ---------------------------------------------------------------------------

static void AppendCluster(State& s, uint32_t mesh,
	const uint32_t* globalIds, uint32_t triCount,
	const glm::vec3& center, float radius,
	const glm::vec3& apex, const glm::vec3& axis, float cutoff) {
	Cluster cl{};
	cl.mesh = mesh;
	cl.indexOffset = (uint32_t)s.filterIndices.size();
	cl.triCount = triCount;
	cl.boundCenter = center;
	cl.boundRadius = radius;
	cl.coneApex = apex;
	cl.coneAxis = axis;
	cl.coneCutoff = cutoff;
	s.filterIndices.insert(s.filterIndices.end(), globalIds, globalIds + triCount * 3);
	s.clusters.push_back(cl);
}

// Expands one meshlet (local-id triangles + vertex window of upload-local
// ids) into heap-global ids. window = cooked.vertices + vertexOffset,
// baseVertex lifts upload-local ids to heap-global ids.
static void AppendMeshlet(State& s, uint32_t mesh, const Meshlet& m,
	const uint8_t* localTris, const uint32_t* window, uint32_t baseVertex) {
	if (m.triangleCount == 0 || m.triangleCount > kMaxClusterTris)
		return;
	std::vector<uint32_t> ids;
	ids.reserve(m.triangleCount * 3);
	for (uint32_t t = 0; t < m.triangleCount; ++t) {
		for (uint32_t k = 0; k < 3; ++k) {
			const uint32_t local = localTris[t * 3 + k];
			if (local >= m.vertexCount)
				return; // corrupt: drop the whole meshlet
			ids.push_back(baseVertex + window[local]);
		}
	}
	AppendCluster(s, mesh, ids.data(), m.triangleCount,
		m.boundCenter, m.boundRadius, m.coneApex, m.coneAxis, m.coneCutoff);
}

static uint32_t FinishMesh(State& s, uint32_t clusterStart) {
	Mesh entry{};
	entry.clusterStart = clusterStart;
	entry.clusterCount = (uint32_t)s.clusters.size() - clusterStart;
	s.meshes.push_back(entry);
	return (uint32_t)s.meshes.size() - 1;
}

uint32_t RegisterMeshFromUpload(State& s, const StaticMeshUpload& upload,
	uint32_t baseVertex) {
	if (upload.vertices.empty() || upload.indices.empty()
		|| (upload.indices.size() % 3) != 0)
		return UINT32_MAX;
	if (s.meshes.size() >= kMaxMeshes)
		return UINT32_MAX;
	const uint32_t clusterStart = (uint32_t)s.clusters.size();
	const uint32_t meshIndex = (uint32_t)s.meshes.size();

	MeshletConfig cfg{};
	cfg.enabled = true; // MeshletConfig defaults to off (cooker opts in)
	CookedMeshlets cooked{};
	const bool cookedOk = BuildMeshletsForUpload(upload, cfg, &cooked, nullptr, nullptr)
		&& !cooked.empty();

	bool useCooked = cookedOk;
	if (useCooked) {
		for (const Meshlet& m : cooked.meshlets) {
			if (m.triangleCount == 0 || m.triangleCount > kMaxClusterTris
				|| m.vertexCount == 0
				|| m.vertexOffset + m.vertexCount > (uint32_t)cooked.vertices.size()
				|| m.triangleOffset + m.triangleCount * 3 > (uint32_t)cooked.triangles.size()) {
				useCooked = false;
				break;
			}
		}
	}

	if (useCooked) {
		for (const Meshlet& m : cooked.meshlets) {
			AppendMeshlet(s, meshIndex, m,
				cooked.triangles.data() + m.triangleOffset,
				cooked.vertices.data() + m.vertexOffset, baseVertex);
		}
	}
	else {
		// Fallback: <=128-tri chunks with AABB-sphere bounds; cone cutoff
		// 1.0 disables the cone test (matches meshopt degenerate bounds).
		s.clusters.reserve(s.clusters.size() + upload.indices.size() / (kMaxClusterTris * 3) + 1);
		const uint32_t triTotal = (uint32_t)upload.indices.size() / 3;
		for (uint32_t first = 0; first < triTotal; first += kMaxClusterTris) {
			const uint32_t count = std::min(kMaxClusterTris, triTotal - first);
			glm::vec3 mn(std::numeric_limits<float>::infinity());
			glm::vec3 mx(-std::numeric_limits<float>::infinity());
			std::vector<uint32_t> ids;
			ids.reserve(count * 3);
			for (uint32_t t = 0; t < count; ++t) {
				for (uint32_t k = 0; k < 3; ++k) {
					const uint32_t local = upload.indices[(first + t) * 3 + k];
					ids.push_back(baseVertex + local);
					if (local < upload.vertices.size()) {
						const glm::vec3& p = upload.vertices[local].pos;
						mn = glm::min(mn, p);
						mx = glm::max(mx, p);
					}
				}
			}
			const glm::vec3 center = (mn + mx) * 0.5f;
			AppendCluster(s, meshIndex, ids.data(), count, center,
				glm::length(mx - center), center,
				glm::vec3(0.0f, 1.0f, 0.0f), 1.0f);
		}
	}

	if (s.clusters.size() == clusterStart)
		return UINT32_MAX; // nothing emitted
	return FinishMesh(s, clusterStart);
}

uint32_t RegisterMeshFromCooked(State& s, const CookedMeshlets& cooked,
	uint32_t baseVertex) {
	if (cooked.empty() || s.meshes.size() >= kMaxMeshes)
		return UINT32_MAX;
	const uint32_t clusterStart = (uint32_t)s.clusters.size();
	const uint32_t meshIndex = (uint32_t)s.meshes.size();
	for (const Meshlet& m : cooked.meshlets) {
		if (m.triangleCount == 0 || m.triangleCount > kMaxClusterTris
			|| m.vertexCount == 0)
			continue;
		if (m.vertexOffset + m.vertexCount > (uint32_t)cooked.vertices.size())
			continue;
		if (m.triangleOffset + m.triangleCount * 3 > (uint32_t)cooked.triangles.size())
			continue;
		// Local ids index the meshlet's vertex window (upload-local ids);
		// lift to heap-global ids with baseVertex.
		const uint32_t* window = cooked.vertices.data() + m.vertexOffset;
		const uint8_t* localTris = cooked.triangles.data() + m.triangleOffset;
		std::vector<uint32_t> globalIds;
		globalIds.reserve(m.triangleCount * 3);
		bool valid = true;
		for (uint32_t i = 0; i < m.triangleCount * 3; ++i) {
			if (localTris[i] >= m.vertexCount) {
				valid = false;
				break;
			}
			globalIds.push_back(baseVertex + window[localTris[i]]);
		}
		if (!valid)
			continue;
		AppendCluster(s, meshIndex, globalIds.data(), m.triangleCount,
			m.boundCenter, m.boundRadius, m.coneApex, m.coneAxis, m.coneCutoff);
	}
	if (s.clusters.size() == clusterStart)
		return UINT32_MAX;
	return FinishMesh(s, clusterStart);
}

void UploadStaticData(State& s, ResourceManager& ctx, RenderGraph& rg) {
	if (!s.filterIndexLogical.empty()) rg.FreeBuffer(s.filterIndexLogical);
	if (!s.clusterLogical.empty()) rg.FreeBuffer(s.clusterLogical);
	s.filterIndexLogical = {};
	s.clusterLogical = {};
	s.filterIndexBuffer = {};
	s.clusterBuffer = {};
	if (s.filterIndices.empty() || s.clusters.empty())
		return;
	s.filterIndexBuffer = ctx.CreateBuffer(
		s.filterIndices.size() * sizeof(uint32_t),
		BufferType_Storage, "gfx clusterIndices");
	std::vector<ClusterConstants> table(s.clusters.size());
	for (size_t i = 0; i < s.clusters.size(); ++i) {
		table[i].indexOffset = s.clusters[i].indexOffset;
		table[i].triCount = s.clusters[i].triCount;
	}
	s.clusterBuffer = ctx.CreateBuffer(
		table.size() * sizeof(ClusterConstants),
		BufferType_Storage, "gfx clusters");
	if (s.filterIndexBuffer.empty() || s.clusterBuffer.empty())
		return;
	ctx.UploadDataToBuffer(s.filterIndexBuffer,
		s.filterIndices.data(), s.filterIndices.size() * sizeof(uint32_t),
		PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	ctx.UploadDataToBuffer(s.clusterBuffer,
		table.data(), table.size() * sizeof(ClusterConstants),
		PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	s.filterIndexLogical = rg.AddExternalBuffer(s.filterIndexBuffer, "gfx clusterIndices");
	s.clusterLogical = rg.AddExternalBuffer(s.clusterBuffer, "gfx clusters");
}

// ---------------------------------------------------------------------------
// Per-frame views
// ---------------------------------------------------------------------------

void BeginFrame(State& s) {
	s.viewCount = 0;
}

static bool EnsureViewSlots(State& s, ResourceManager& ctx, RenderGraph& rg, uint32_t count) {
	if (count > kMaxViews)
		return false;
	while ((uint32_t)s.views.size() < count) {
		View slot{};
		slot.filteredBuffer = ctx.CreateBuffer(
			kFilteredBytes, BufferType_Storage, "gfx filteredIndices");
		slot.indirectBuffer = ctx.CreateBuffer(
			kIndirectBytes, BufferType_Indirect, "gfx indirect");
		if (slot.filteredBuffer.empty() || slot.indirectBuffer.empty())
			return false;
		slot.filteredLogical = rg.AddExternalBuffer(slot.filteredBuffer, "gfx filteredIndices");
		slot.indirectLogical = rg.AddExternalBuffer(slot.indirectBuffer, "gfx indirect");
		if (slot.filteredLogical.empty() || slot.indirectLogical.empty()) {
			if (!slot.filteredLogical.empty()) rg.FreeBuffer(slot.filteredLogical);
			if (!slot.indirectLogical.empty()) rg.FreeBuffer(slot.indirectLogical);
			return false;
		}
		s.views.push_back(std::move(slot));
	}
	return true;
}

uint32_t BeginView(State& s, ResourceManager& ctx, RenderGraph& rg,
	const RenderView& renderView, const glm::vec3& eye,
	CullFlags cullFlags, uint32_t width, uint32_t height) {
	const uint32_t slot = s.viewCount;
	if (!EnsureViewSlots(s, ctx, rg, slot + 1))
		return UINT32_MAX;
	View& v = s.views[slot];
	v.eye = eye;
	v.cullFlags = cullFlags;
	v.width = width;
	v.height = height;
	v.viewProj = renderView.m_viewProjectionMatrix;
	v.renderView = &renderView;
	v.draws.clear();
	v.batches.clear();
	v.drawConstants.clear();
	v.materialRanges.clear();
	v.batchCount = 0;
	v.stats = Stats{};
	++s.viewCount;
	return slot;
}

void PushDraw(State& s, uint32_t view, uint32_t mesh,
	const glm::mat4& model, uint32_t material) {
	assert(view < s.viewCount);
	assert(mesh < s.meshes.size());
	View& v = s.views[view];
	if (v.draws.size() >= kMaxDraws)
		return;
	DrawRequest d{};
	d.mesh = mesh;
	d.material = material;
	d.model = model;
	v.draws.push_back(d);
}

static float MaxBasisScale(const glm::mat4& m) {
	return std::max({
		glm::length(glm::vec3(m[0])),
		glm::length(glm::vec3(m[1])),
		glm::length(glm::vec3(m[2])) });
}

// CPU pre-cull port of GeometryFX's cluster test (AddRequest cone check)
// using meshoptimizer perspective formulas (see meshoptimizer.h:
// dot(normalize(apex - eye), axis) >= cutoff culls).
static bool CullCluster(const Cluster& c, const glm::mat4& model,
	const glm::mat4& invModel, const glm::vec3& eye,
	const RenderView& renderView) {
	const glm::vec3 worldCenter = glm::vec3(model * glm::vec4(c.boundCenter, 1.0f));
	const float worldRadius = c.boundRadius * MaxBasisScale(model);
	if (!RenderViewIntersectsSphere(renderView, worldCenter, worldRadius))
		return true;
	const glm::vec3 eyeObj = glm::vec3(invModel * glm::vec4(eye, 1.0f));
	const glm::vec3 d = c.coneApex - eyeObj;
	const float len2 = glm::dot(d, d);
	if (len2 < 1e-12f)
		return false; // eye inside the apex: keep (avoids NaN)
	if (glm::dot(d / std::sqrt(len2), c.coneAxis) >= c.coneCutoff)
		return true;
	return false;
}

Stats FinalizeView(State& s, ResourceManager& ctx, uint32_t view) {
	assert(view < s.viewCount);
	View& v = s.views[view];
	v.stats.draws = (uint32_t)v.draws.size();

	// Sort draws by material (stable: submission order kept per material).
	// Commands are emitted in this order, so each material owns one
	// contiguous indirect range (single multi-draw per material).
	std::vector<uint32_t> order(v.draws.size());
	std::iota(order.begin(), order.end(), 0u);
	std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		return v.draws[a].material < v.draws[b].material;
	});

	v.drawConstants.reserve(v.draws.size());
	std::vector<uint32_t> remap(v.draws.size(), UINT32_MAX); // old draw -> sorted draw
	for (uint32_t sorted = 0; sorted < (uint32_t)order.size(); ++sorted) {
		const DrawRequest& d = v.draws[order[sorted]];
		Draw dc{};
		dc.model = d.model;
		dc.mesh = d.mesh;
		v.drawConstants.push_back(dc);
		remap[order[sorted]] = sorted;
	}

	uint32_t batchSlot = 0;
	uint32_t cmdMaterial = UINT32_MAX;
	uint32_t rangeStart = 0;
	for (uint32_t sorted = 0; sorted < (uint32_t)order.size(); ++sorted) {
		const DrawRequest& d = v.draws[order[sorted]];
		const Mesh& mesh = s.meshes[d.mesh];
		if (d.material != cmdMaterial) {
			if (cmdMaterial != UINT32_MAX) {
				MaterialRange r{};
				r.material = cmdMaterial;
				r.firstCommand = rangeStart;
				r.commandCount = batchSlot - rangeStart;
				if (r.commandCount > 0)
					v.materialRanges.push_back(r);
			}
			cmdMaterial = d.material;
			rangeStart = batchSlot;
		}
		if (batchSlot >= kMaxBatches)
			break;
		const glm::mat4 invModel = glm::inverse(d.model);
		for (uint32_t ci = 0; ci < mesh.clusterCount; ++ci) {
			if (batchSlot >= kMaxBatches)
				break;
			const Cluster& c = s.clusters[mesh.clusterStart + ci];
			++v.stats.clustersProcessed;
			if (CullCluster(c, d.model, invModel, v.eye, *v.renderView)) {
				++v.stats.clustersCulled;
				continue;
			}
			Batch b{};
			b.cluster = mesh.clusterStart + ci;
			b.draw = sorted;
			b.outputOffset = batchSlot * kMaxClusterTris * 3;
			v.batches.push_back(b);
			v.stats.trianglesProcessed += c.triCount;
			++batchSlot;
		}
	}
	if (cmdMaterial != UINT32_MAX && batchSlot > rangeStart) {
		MaterialRange r{};
		r.material = cmdMaterial;
		r.firstCommand = rangeStart;
		r.commandCount = batchSlot - rangeStart;
		v.materialRanges.push_back(r);
	}
	v.batchCount = batchSlot;
	v.stats.draws = (uint32_t)v.draws.size();
	v.stats.commands = batchSlot;

	if (v.batchCount > 0) {
		ctx.UploadDataToBuffer(s.batchBuffer,
			v.batches.data(), v.batches.size() * sizeof(Batch),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	}
	if (!v.drawConstants.empty()) {
		ctx.UploadDataToBuffer(s.drawBuffer,
			v.drawConstants.data(), v.drawConstants.size() * sizeof(Draw),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
		// Zero-fill indirect (replaces ClearDrawIndirectArgsCS; each group
		// writes exactly its own command slot, so no atomics are needed).
		std::vector<uint8_t> zeros(v.batchCount * 20, 0);
		ctx.UploadDataToBuffer(v.indirectBuffer,
			zeros.data(), zeros.size(),
			PipelineStage(PipelineStageBit_Compute),
			AccessFlags::ShaderRead | AccessFlags::ShaderWrite);
	}
	return v.stats;
}

RenderPass& AddFilterPass(State& s, RenderGraph& rg, uint32_t view) {
	assert(view < s.viewCount);
	View& v = s.views[view];
	RenderPass& pass = rg.AddComputePass("GfxFilter");
	BufferSyncRequest computeRead{};
	computeRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
	computeRead.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(s.filterIndexLogical, computeRead);
	pass.AddBufferHint(s.clusterLogical, computeRead);
	pass.AddBufferHint(s.batchLogical, computeRead);
	pass.AddBufferHint(s.drawLogical, computeRead);
	BufferSyncRequest computeRW{};
	computeRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	computeRW.barrierMasks.stageMask = PipelineStageBit_Compute | PipelineStageBit_Vertex;
	pass.AddBufferHint(v.filteredLogical, computeRW);
	pass.AddBufferHint(v.indirectLogical, computeRW);
	return pass;
}

void AddConsumeHints(RenderPass& pass,
	LogicalBufferHandle filteredLogical, LogicalBufferHandle indirectLogical) {
	BufferSyncRequest indexRead{};
	indexRead.barrierMasks.accessMask = AccessFlags::IndexRead;
	indexRead.barrierMasks.stageMask = PipelineStageBit_Vertex;
	pass.AddBufferHint(filteredLogical, indexRead);
	BufferSyncRequest indirectRead{};
	indirectRead.barrierMasks.accessMask = AccessFlags::IndirectCommandRead;
	indirectRead.barrierMasks.stageMask = PipelineStageBit_Vertex;
	pass.AddBufferHint(indirectLogical, indirectRead);
}

void PushAndDispatch(State& s, CommandBuffer& cmd, ResourceManager& ctx, uint32_t view) {
	assert(view < s.views.size());
	View& v = s.views[view];
	if (v.batchCount == 0)
		return;
	cmd.BindComputePipeline(s.filterProgram);
	FilterPush push{};
	push.positions = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticVertexPosBuffer);
	push.indices = ctx.GetBufferDeviceAddress(s.filterIndexBuffer);
	push.clusters = ctx.GetBufferDeviceAddress(s.clusterBuffer);
	push.batches = ctx.GetBufferDeviceAddress(s.batchBuffer);
	push.draws = ctx.GetBufferDeviceAddress(s.drawBuffer);
	push.filtered = ctx.GetBufferDeviceAddress(v.filteredBuffer);
	push.indirect = ctx.GetBufferDeviceAddress(v.indirectBuffer);
	push.viewProj = v.viewProj;
	push.eye = v.eye;
	push.cullFlags = v.cullFlags;
	push.width = v.width;
	push.height = v.height;
	push.batchCount = v.batchCount;
	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	cmd.Dispatch(v.batchCount, 1, 1);
}

} // namespace gfx
} // namespace smvk
