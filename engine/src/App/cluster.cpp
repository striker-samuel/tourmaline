// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/App/cluster.h>
#include <Engine/Renderer/vertex_pack.h>
#include <Engine/Renderer/static_vertex_heap.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Scene/scene.h>

#include <cstdio>

namespace tour {
// (migration compat) the impl still spells the RHI/engine smvk
// types unqualified; qualify as call sites migrate, then drop this.
using namespace ::smvk;
// -----------------------------------------------------------------------------
// Mesh heap-range retirement (the deferred-destruction contract — the
// facade's DeferDestroyTask, the Vulkan VulkanResourceManager::DeferDestroy
// key): the vertex/index arena ranges free RETIREMENT-ORDERED with the last published graphics timeline
// point, never in place. An in-flight frame can still be DRAWING a destroyed
// mesh's ranges — an immediate arena free hands the range to the next upload,
// and the old frame's draws then read the NEW mesh's vertices/indices through
// the old draw args (garbage rendering, the classic suballoc-vs-frames-in-
// flight hazard). The deferred task keys on the same point every other range
// free in the engine keys on (ReleaseTargetHeapClaim /
// ReleaseTransientPlacement), so the free lands strictly after every
// submitted frame that can still reference the range, and a reuse-allocated
// new mesh's uploads land strictly after that.
//
// Contract (the DeferDestroy API rule): only call from the render thread
// between frames — the published point then covers every in-flight producer.
// The arenas live in the lifetime-global staticVertexFmtHeap (inline storage
// — outlives every retirement), so capturing them by pointer here stays
// valid through the retirement window.
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// In-place variant (see the retirement contract above): ONLY safe for ranges
// that were never staged to the GPU and never referenced by any pending
// streamer batch (the batch allocation loops' all-or-nothing rollbacks, whose
// bulk uploads happen only after every range is carved + every pool entry
// created). Everything a draw could have touched must use the deferred form.
// -----------------------------------------------------------------------------
void FreeStaticMeshRangesNow(
	StaticHeapArena::AllocId vertexAllocation,
	StaticHeapArena::AllocId indexAllocation,
	StaticHeapArena::AllocId lightmapAllocation)
{
	if (vertexAllocation)
		staticVertexFmtHeap.m_staticVertexBlock.Free(vertexAllocation);
	if (indexAllocation)
		staticVertexFmtHeap.m_staticIndexBlock.Free(indexAllocation);
	if (lightmapAllocation)
		staticVertexFmtHeap.m_staticLightmapBlock.Free(lightmapAllocation);
}

void DeferFreeStaticMeshRanges(
	ResourceManager* ctx,
	StaticHeapArena::AllocId vertexAllocation,
	StaticHeapArena::AllocId indexAllocation,
	StaticHeapArena::AllocId lightmapAllocation)
{
	if (!ctx) {
		// Defensive (allocations imply a live ctx_): nothing was ever
		// staged through this path, free in place.
		FreeStaticMeshRangesNow(vertexAllocation, indexAllocation,
			lightmapAllocation);
		return;
	}
	StaticHeapArena* vertexBlock = &staticVertexFmtHeap.m_staticVertexBlock;
	StaticHeapArena* indexBlock = &staticVertexFmtHeap.m_staticIndexBlock;
	StaticHeapArena* lightmapBlock =
		&staticVertexFmtHeap.m_staticLightmapBlock;
	ctx->DeferDestroyTask(std::packaged_task<void()>(
		[vertexBlock, vertexAllocation, indexBlock, indexAllocation,
			lightmapBlock, lightmapAllocation] {
			if (vertexAllocation)
				vertexBlock->Free(vertexAllocation);
			if (indexAllocation)
				indexBlock->Free(indexAllocation);
			if (lightmapAllocation)
				lightmapBlock->Free(lightmapAllocation);
		}));
}
} // namespace smvk

smvk::Cluster::~Cluster() {

	staticMeshes.forEachHandle([&](ClusterStaticMeshHandle han) {
		DestroyMesh(han);
	});

	assert(meshAllocations_.empty());

	// Materials are just pods
	materials.clear();

	// textures own holders, when cleared holders go.
	textures.clear();
}

smvk::ClusterStaticMeshHandle smvk::Cluster::UploadStaticMesh(
	const StaticMeshUpload& data)
{
	const uint32_t vertexCount =
		static_cast<uint32_t>(data.vertices.size());

	const uint32_t indexCount =
		static_cast<uint32_t>(data.indices.size());


	if (vertexCount == 0)
		return {};

	// GPU-free offline mode: CPU geometry only (no VMA/streamer). Mirrors the
	// runtime single-surface layout so the serializer sees the same shape.
	if (offlineMode_) {
		StaticMesh mesh{};
		mesh.geometry.topology = PrimitiveTopology::TriangleList;
		mesh.geometry.vertexCount = vertexCount;
		mesh.geometry.indexCount = indexCount;
		mesh.geometry.aabbMin = data.aabbMin;
		mesh.geometry.aabbMax = data.aabbMax;
		mesh.geometry.sphereRadius =
			glm::length(data.aabbMax - data.aabbMin) * 0.5f;
		Surface surface{};
		surface.firstIndex = 0;
		surface.indexCount = indexCount;
		surface.baseVertex = 0;
		mesh.geometry.surfaces.push_back(surface);
		const ClusterStaticMeshHandle handle =
			staticMeshes.create(std::move(mesh));
		if (retainUploads_)
			retainedUploads_[handle.index()] = data;
		return handle;
	}

	StaticMesh result{};
	StaticMeshAllocation alloc{};


	// =========================================================================
	// 1. Allocate vertex range
	//
	// vertexBase is in VERTEX UNITS, not bytes.
	// =========================================================================

	uint64_t vertexBase = 0;

	{
		alloc.vertexAllocation =
			staticVertexFmtHeap.m_staticVertexBlock.Allocate(
				vertexCount, 1, vertexBase);

		if (alloc.vertexAllocation == 0) {
			return {};
		}
	}


	// =========================================================================
	// 2. Allocate index range
	// =========================================================================

	const uint64_t indexBytes =
		sizeof(uint32_t) *
		static_cast<uint64_t>(indexCount);

	uint64_t indexByteOffset = 0;

	if (indexCount > 0) {

		alloc.indexAllocation =
			staticVertexFmtHeap.m_staticIndexBlock.Allocate(
				indexBytes, alignof(uint32_t), indexByteOffset);

		if (alloc.indexAllocation == 0) {

			staticVertexFmtHeap.m_staticVertexBlock.Free(
				alloc.vertexAllocation);

			return {};
		}
	}


	// =========================================================================
	// 3. Convert AoS -> SoA-ish GPU streams
	// =========================================================================

	std::vector<PackedVertexPosition> positions(vertexCount);
	std::vector<PackedVertexShading> shading(vertexCount);
	std::vector<PackedVertexLightmap> lightmap(vertexCount);
	bool hasLightmap = false;

	for (uint32_t i = 0; i < vertexCount; ++i) {

		const ImportStaticVertex& src = data.vertices[i];

		PackStaticVertex(src, positions[i], shading[i], lightmap[i]);
		// The optional stream carves only when chart data exists (the
		// uncharted sentinel means "never lightmapped").
		if (src.chartId != 0xFFFFFFFFu)
			hasLightmap = true;
	}

	// =========================================================================
	// 3b. The OPTIONAL lightmap range — carved only for charted meshes,
	// independently packed (its base is NOT vertexBase).
	// =========================================================================

	if (hasLightmap) {
		uint64_t lightmapBase = 0;
		alloc.lightmapAllocation =
			staticVertexFmtHeap.m_staticLightmapBlock.Allocate(
				vertexCount, 1, lightmapBase);
		if (alloc.lightmapAllocation == 0) {
			// All-or-nothing: release the vertex + index ranges carved
			// above.
			staticVertexFmtHeap.m_staticVertexBlock.Free(
				alloc.vertexAllocation);
			if (alloc.indexAllocation)
				staticVertexFmtHeap.m_staticIndexBlock.Free(
					alloc.indexAllocation);
			return {};
		}
		alloc.lightmapBase = lightmapBase;
	}


	// =========================================================================
	// 4. Upload position stream
	// =========================================================================

	const uint64_t positionOffset =
		vertexBase * sizeof(PackedVertexPosition);

	const uint64_t positionBytes =
		vertexCount * sizeof(PackedVertexPosition);

	ctx_->UploadDataToBuffer(
		staticVertexFmtHeap.m_staticVertexPosBuffer,
		positions.data(),
		positionBytes,

		PipelineStageBit_Vertex,
		AccessFlags::VertexAttributeRead,

		0,
		positionOffset
	);


	// =========================================================================
	// 5. Upload shading stream
	// =========================================================================

	const uint64_t shadingOffset =
		vertexBase * sizeof(PackedVertexShading);

	const uint64_t shadingBytes =
		vertexCount * sizeof(PackedVertexShading);

	ctx_->UploadDataToBuffer(
		staticVertexFmtHeap.m_staticVertexShadingBuffer,
		shading.data(),
		shadingBytes,

		PipelineStageBit_Vertex,
		AccessFlags::VertexAttributeRead,

		0,
		shadingOffset
	);

	// =========================================================================
	// 5b. Upload the OPTIONAL lightmap stream (BDA-read by the mesh-shader
	// leg + the future chart-unpack VS — ShaderRead, not a vertex-binding
	// fetch: the stream is a storage buffer by design).
	// =========================================================================

	if (alloc.lightmapAllocation) {
		const uint64_t lightmapOffset =
			alloc.lightmapBase * sizeof(PackedVertexLightmap);
		const uint64_t lightmapBytes =
			vertexCount * sizeof(PackedVertexLightmap);
		ctx_->UploadDataToBuffer(
			staticVertexFmtHeap.m_staticVertexLightmapBuffer,
			lightmap.data(),
			lightmapBytes,
			PipelineStageBit_All,
			AccessFlags::ShaderRead,
			0,
			lightmapOffset);
	}


	// =========================================================================
	// 6. Upload indices
	// =========================================================================

	if (indexCount > 0) {

		ctx_->UploadDataToBuffer(
			staticVertexFmtHeap.m_staticIndexBuffer,
			data.indices.data(),
			indexBytes,

			PipelineStageBit_Vertex,
			AccessFlags::IndexRead,

			0,
			indexByteOffset
		);
	}


	// =========================================================================
	// 7. Construct mesh
	// =========================================================================

	StaticMesh& mesh = result;

	mesh.geometry.topology =
		PrimitiveTopology::TriangleList;

	mesh.geometry.vertexCount = vertexCount;
	mesh.geometry.indexCount = indexCount;

		mesh.geometry.aabbMin = data.aabbMin;
		mesh.geometry.aabbMax = data.aabbMax;


	Surface surface{};

	surface.firstIndex =
		static_cast<uint32_t>(
			indexByteOffset / sizeof(uint32_t)
			);

	surface.indexCount = indexCount;

	surface.baseVertex =
		static_cast<int32_t>(vertexBase);

	mesh.geometry.surfaces.push_back(surface);

	// The optional lightmap window's published base (0xFFFFFFFF = none)
	// — the draw-ring fill reads it to compute the per-draw delta.
	mesh.lightmapBase = alloc.lightmapAllocation
		? static_cast<uint32_t>(alloc.lightmapBase) : 0xFFFFFFFFu;

	const ClusterStaticMeshHandle handle =
		staticMeshes.create(std::move(result));

	if (handle.empty()) {
		// The UploadDataToBuffer calls above were already staged into
		// these ranges (vertex/shading/lightmap/index), so the frees must
		// retire with that GPU work - never in place (see
		// DeferFreeStaticMeshRanges).
		DeferFreeStaticMeshRanges(
			ctx_,
			alloc.vertexAllocation, alloc.indexAllocation,
			alloc.lightmapAllocation);
		return {};
	}

	alloc.vertexBase = vertexBase;
	alloc.indexByteOffset = indexByteOffset;

	auto [it, inserted] = meshAllocations_.emplace(
		handle.index(),
		new StaticMeshAllocation(std::move(alloc)));
	assert(inserted);

	if (retainUploads_)
		retainedUploads_[handle.index()] = data;

	return handle;
}

void smvk::Cluster::PatchRetainedSkinStream(
	ClusterStaticMeshHandle handle, std::vector<PackedSkinInfluences>&& stream)
{
	if (handle.empty() || stream.empty())
		return;
	// The skin stream ALWAYS lands the runtime-side map (the draw path's
	// source until the binding-2 GPU allocation), and - when uploads are
	// retained - the retained upload (the serializer's source).
	skinStreams_[handle.index()] = stream;
	auto it = retainedUploads_.find(handle.index());
	if (it == retainedUploads_.end())
		std::fprintf(stderr,
			"[Cluster] PatchRetainedSkinStream: mesh slot %u has no"
			" retained upload (the cook path loses the stream; the"
			" runtime map keeps it)\n", handle.index());
	else
		it->second.skin = stream;
	StaticMesh* mesh = staticMeshes.get(handle);
	if (mesh)
		mesh->skinned = true;
}

void smvk::Cluster::SetMeshSkinned(ClusterStaticMeshHandle handle, bool skinned)
{
	if (handle.empty())
		return;
	if (StaticMesh* mesh = staticMeshes.get(handle))
		mesh->skinned = skinned;
}

const std::vector<smvk::PackedSkinInfluences>* smvk::Cluster::GetMeshSkinStream(
	ClusterStaticMeshHandle handle) const
{
	if (handle.empty())
		return nullptr;
	auto it = skinStreams_.find(handle.index());
	return it == skinStreams_.end() ? nullptr : &it->second;
}

const std::vector<smvk::PackedSkinInfluences>* smvk::Cluster::GetRetainedSkinStream(
	ClusterStaticMeshHandle handle) const
{
	if (handle.empty())
		return nullptr;
	auto it = retainedUploads_.find(handle.index());
	if (it == retainedUploads_.end() || it->second.skin.empty())
		return nullptr;
	return &it->second.skin;
}

void smvk::Cluster::SetMeshSkinStream(ClusterStaticMeshHandle handle,
	std::vector<smvk::PackedSkinInfluences>&& stream)
{
	if (handle.empty() || stream.empty())
		return;
	skinStreams_[handle.index()] = std::move(stream);
	if (StaticMesh* mesh = staticMeshes.get(handle))
		mesh->skinned = true;
	skinnedGpuValid_ = false; // re-Ensure picks up the new stream
}

// ---------------------------------------------------------------------------
// SKINNED-MESH GPU data (the Northlight compute-skinning path). Built
// once per cluster (idempotent): the skinned-stream buffer (the
// concatenated PackedSkinInfluences rows, static) + the DEFORMED buffer (the
// skinning pass's per-frame output: [pos3, normalW, tangentW] per vertex,
// 20B) + the ranges table ((heapBase, count, skinBase) per range, static).
// The heaps stay the BIND POSE (the pass's input).
// ---------------------------------------------------------------------------
bool smvk::Cluster::EnsureSkinnedGpuData(ResourceManager& ctx) {
	if (skinnedGpuValid_)
		return !skinnedRanges_.empty();
	if (skinStreams_.empty() || !ctx_) {
		skinnedGpuValid_ = true;
		skinnedRanges_.clear();
		return false;
	}
	skinnedGpuValid_ = true; // one shot (a rebuild re-Ensures via SetMeshSkinStream)

	// Gather the per-mesh ranges: the stream (skinStreams_) + the heap
	// base (meshAllocations_, recorded by every upload path). The vertex
	// count comes from the mesh's geometry (the stream must parallel it;
	// a mismatched stream clamps to the geometry count).
	skinnedRanges_.clear();
	skinnedRanges_.reserve(skinStreams_.size());
	std::vector<smvk::PackedSkinInfluences> streamData;
	uint32_t minBase = UINT32_MAX;
	uint32_t maxEnd = 0;
	for (auto& [poolIdx, stream] : skinStreams_) {
		auto it = meshAllocations_.find(poolIdx);
		if (it == meshAllocations_.end() || !it->second)
			continue; // no heap range (offline clusters call this never)
		const StaticMesh* m = staticMeshes.get(
			ClusterStaticMeshHandle::Mint(poolIdx));
		if (!m)
			continue;
		SkinnedMeshRange r{};
		r.mesh = ClusterStaticMeshHandle::Mint(poolIdx);
		r.vertexCount = std::min(uint32_t(stream.size()),
			m->geometry.vertexCount);
		r.heapVertexBase = uint32_t(it->second->vertexBase);
		r.skinBase = uint32_t(streamData.size());
		if (r.vertexCount == 0)
			continue;
		streamData.insert(streamData.end(),
			stream.begin(), stream.begin() + r.vertexCount);
		skinnedRanges_.push_back(r);
		minBase = std::min(minBase, r.heapVertexBase);
		maxEnd = std::max(maxEnd, r.heapVertexBase + r.vertexCount);
	}
	if (skinnedRanges_.empty())
		return false;

	if (std::getenv("TOURMALINE_KD")) {
		for (const SkinnedMeshRange& r : skinnedRanges_) {
			double wsum = 0.0;
			uint32_t zeroW = 0, maxJoint = 0;
			for (uint32_t v = 0; v < r.vertexCount; ++v) {
				const smvk::PackedSkinInfluences& sk = streamData[r.skinBase + v];
				const float s = float(sk.weights & 0xFF)
					+ float((sk.weights >> 8) & 0xFF)
					+ float((sk.weights >> 16) & 0xFF)
					+ float((sk.weights >> 24) & 0xFF);
				if (s <= 1e-3f)
					++zeroW;
				wsum += s;
				maxJoint = (std::max)(maxJoint,
					(std::max)({(sk.joints) & 0xFFu,
						(sk.joints >> 8) & 0xFFu,
						(sk.joints >> 16) & 0xFFu,
						(sk.joints >> 24) & 0xFFu}));
			}
			std::fprintf(stderr,
				"[KD] skinRange heapBase=%u count=%u skinBase=%u"
				" meanW=%.1f zeroW=%u maxJoint=%u\n",
				r.heapVertexBase, r.vertexCount, r.skinBase,
				wsum / (std::max)(1u, r.vertexCount), zeroW, maxJoint);
		}
	}

	// The deformed buffer covers the skinned SPAN [minBase, maxEnd) of
	// the heap id space (a non-skinned interleave wastes 20B/id - the
	// demo's clusters are all-skinned, and the offset math stays one
	// subtract for every fetch).
	skinnedDeformedBase_ = minBase;
	const uint32_t span = maxEnd - minBase;

	// 5 words per vertex: pos (3 uint words, reinterpreted floats) +
	// normal and tangent (the packed oct words). The skinning pass writes
	// them; the draw fetches map vi - skinnedDeformedBase_ into the span.
	const size_t skinBytes = streamData.size() * sizeof(smvk::PackedSkinInfluences);
	skinnedStream_ = ctx_->CreateBuffer(
		skinBytes > 0 ? skinBytes : 4, BufferType_Storage, "skinned stream");
	// The deformed buffer is a PARITY DOUBLE BUFFER (two halves of the
	// span): the driver flips a per-frame parity and points the pass's
	// deformed BDA at the cur half (every render fetch follows the BDA -
	// no consumer arithmetic changes), while the motion vector leg reads
	// cur + PREV halves for the skinned motion vectors (this frame's +
	// last frame's animated pose - the SMAA T2x resolve / DLSS / motion
	// blur histories need TRUE mesh motion; static-heap vectors ghosted
	// every animated mesh through those temporal consumers).
	const size_t deformBytes = size_t(span) * 5 * sizeof(uint32_t);
	skinnedDeformedSpanBytes_ = deformBytes;
	skinnedDeformed_ = ctx_->CreateBuffer(deformBytes * 2, BufferType_Storage,
		"skinned deformed");
	// The ranges table: (heapBase, count, skinBase) rows (the compute
	// pass's per-range dispatch inputs; one workgroup per range).
	struct SkinnedRangeGpu {
		uint32_t heapBase;
		uint32_t count;
		uint32_t skinBase;
		uint32_t pad;
	};
	std::vector<SkinnedRangeGpu> table;
	table.reserve(skinnedRanges_.size());
	for (const SkinnedMeshRange& r : skinnedRanges_)
		table.push_back({ r.heapVertexBase, r.vertexCount, r.skinBase, 0 });
	skinnedRangesBuf_ = ctx_->CreateBuffer(
		table.size() * sizeof(SkinnedRangeGpu), BufferType_Storage,
		"skinned ranges");
	if (skinnedStream_.empty() || skinnedDeformed_.empty()
		|| skinnedRangesBuf_.empty()) {
		skinnedRanges_.clear();
		skinnedDeformedBase_ = UINT32_MAX;
		return false;
	}
	ctx_->UploadDataToBuffer(skinnedStream_, streamData.data(), skinBytes,
		PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
	ctx_->UploadDataToBuffer(skinnedRangesBuf_, table.data(),
		table.size() * sizeof(SkinnedRangeGpu),
		PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
	return true;
}

// ---------------------------------------------------------------------------
// The animation driver (CPU; the palette build). The track writes compose
// T * R * S from the current local's decomposition (the loaded locals ARE
// the bind pose; a track REPLACES its whole channel, untracked channels
// keep the loaded value - the repeated-sample round trip is exact for
// glTF TRS-authored locals).
// ---------------------------------------------------------------------------
void smvk::Cluster::SetAnimClip(touranim::Clip clip) {
	animClip_ = std::move(clip);
	animTime_ = 0.0;
}

bool smvk::Cluster::SampleAnimation(float dt,
	std::vector<glm::mat4>& outPalette) {
	if (!hasSkeleton || !animClip_ || skeleton.Count() == 0)
		return false;
	const touranim::Clip& clip = *animClip_;
	if (clip.duration > 1e-6) {
		animTime_ += double(dt);
		while (animTime_ > clip.duration)
			animTime_ -= clip.duration;
	}
	for (const touranim::Track& t : clip.tracks) {
		if (t.joint >= scene.localTransform.size())
			continue;
		const glm::vec4 v = touranim::SampleTrack(t, animTime_);
		glm::mat4 local = scene.localTransform[t.joint];
		// Decompose the current local (the bind channels for untracked
		// axes; exact round trip for TRS-authored locals).
		const glm::vec3 T(local[3]);
		const glm::mat3 R3(local);
		glm::vec3 S(
			glm::length(glm::vec3(R3[0])),
			glm::length(glm::vec3(R3[1])),
			glm::length(glm::vec3(R3[2])));
		glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
		{
			glm::mat3 Rn(1.0f);
			for (int c = 0; c < 3; ++c) {
				if (S[c] > 1e-8f)
					Rn[c] = R3[c] / S[c];
			}
			q = glm::quat_cast(Rn);
		}
		// The track REPLACES its whole channel (the .tourAnim rule).
		switch (t.channel) {
		case touranim::Channel::Translate:
			local = glm::translate(glm::mat4(1.0f),
					glm::vec3(v.x, v.y, v.z))
				* glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), S);
			break;
		case touranim::Channel::Rotate:
			q = glm::quat(v.w, v.x, v.y, v.z); // xyzw keys -> wxyz ctor
			local = glm::translate(glm::mat4(1.0f), T)
				* glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), S);
			break;
		case touranim::Channel::Scale:
			S = glm::vec3(v.x, v.y, v.z);
			local = glm::translate(glm::mat4(1.0f), T)
				* glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), S);
			break;
		}
		scene.localTransform[t.joint] = local;
		// The joint's DESCENDANTS follow (markAsChanged covers the
		// subtree; the child joints + skinned mesh nodes recalc).
		markAsChanged(scene, int(t.joint));
	}
	recalculateGlobalTransforms(scene);
	touranim::BuildSkinPalette(scene, skeleton, outPalette);
	return true;
}



std::vector<smvk::ClusterStaticMeshHandle> smvk::Cluster::UploadStaticMeshes(
	std::span<const StaticMeshUpload> uploads)
{
	std::vector<ClusterStaticMeshHandle> handles;
	if (uploads.empty())
		return handles;

	// GPU-free offline mode: CPU geometry only — per-mesh single path (the
	// batch win is GPU-side staging, which offline mode never touches).
	if (offlineMode_) {
		handles.reserve(uploads.size());
		for (const StaticMeshUpload& data : uploads)
			handles.push_back(UploadStaticMesh(data));
		return handles;
	}

	// =========================================================================
	// 1. Allocate heap ranges (per mesh; all-or-nothing — a degenerate mesh
	//    or a full heap fails the batch with NOTHING staged to the GPU)
	// =========================================================================

	struct MeshRange {
		uint64_t vertexBase = 0; // VERTEX UNITS, not bytes
		uint64_t indexByteOffset = 0;
		StaticMeshAllocation alloc{};
	};
	std::vector<MeshRange> ranges(uploads.size());
	// All-or-nothing rollback for the carve loop below: a degenerate or
	// heap-failing entry must free every range the loop already carved
	// (entries [0, i) + entry i's PARTIAL carve - a vertex alloc that
	// succeeded before its index alloc failed). The old bare `return {}`
	// LEAKED all of them (the ranges stayed allocated in the virtual
	// blocks forever - nothing tracked them without pool handles). IN
	// PLACE is safe here: nothing of this batch was staged to the GPU
	// yet (the bulk uploads run only after every range is carved + every
	// pool entry created), so an immediate retry sees the full capacity.
	auto failCarved = [&ranges](size_t upto) {
		for (size_t j = 0; j < upto; ++j)
			FreeStaticMeshRangesNow(ranges[j].alloc.vertexAllocation,
				ranges[j].alloc.indexAllocation,
				ranges[j].alloc.lightmapAllocation);
	};
	for (size_t i = 0; i < uploads.size(); ++i) {
		const StaticMeshUpload& data = uploads[i];
		const uint32_t vertexCount =
			static_cast<uint32_t>(data.vertices.size());
		const uint32_t indexCount =
			static_cast<uint32_t>(data.indices.size());
		if (vertexCount == 0) {
			failCarved(i); // degenerate entry fails the whole batch
			return {};
		}
		MeshRange& r = ranges[i];
		{
			r.alloc.vertexAllocation =
				staticVertexFmtHeap.m_staticVertexBlock.Allocate(
					vertexCount, 1, r.vertexBase);
			if (r.alloc.vertexAllocation == 0) {
				failCarved(i);
				return {};
			}
		}
		if (indexCount > 0) {
			const uint64_t indexBytes = sizeof(uint32_t)
				* static_cast<uint64_t>(indexCount);
			r.alloc.indexAllocation =
				staticVertexFmtHeap.m_staticIndexBlock.Allocate(
					indexBytes, alignof(uint32_t), r.indexByteOffset);
			if (r.alloc.indexAllocation == 0) {
				// Entry i's own vertex carve rolls back too (it succeeded
				// above and nothing references it yet).
				failCarved(i + 1);
				return {};
			}
		}
		// The OPTIONAL lightmap range: carved only when any vertex is
		// charted (independently packed — its base is not vertexBase).
		{
			bool charted = false;
			for (const ImportStaticVertex& v : data.vertices)
				if (v.chartId != 0xFFFFFFFFu) {
					charted = true;
					break;
				}
			if (charted) {
				uint64_t lmBase = 0;
				r.alloc.lightmapAllocation =
					staticVertexFmtHeap.m_staticLightmapBlock.Allocate(
						vertexCount, 1, lmBase);
				if (r.alloc.lightmapAllocation == 0) {
					failCarved(i + 1);
					return {};
				}
				r.alloc.lightmapBase = lmBase;
			}
		}
	}

	// =========================================================================
	// 2. Pool entries + bookkeeping (BEFORE any GPU staging: a failure here
	//    leaves nothing queued in the upload streamer)
	// =========================================================================

	handles.resize(uploads.size());
	for (size_t i = 0; i < uploads.size(); ++i) {
		const StaticMeshUpload& data = uploads[i];
		MeshRange& r = ranges[i];
		const uint32_t vertexCount =
			static_cast<uint32_t>(data.vertices.size());
		const uint32_t indexCount =
			static_cast<uint32_t>(data.indices.size());
		StaticMesh mesh{};
		mesh.geometry.topology = PrimitiveTopology::TriangleList;
		mesh.geometry.vertexCount = vertexCount;
		mesh.geometry.indexCount = indexCount;
		mesh.geometry.aabbMin = data.aabbMin;
		mesh.geometry.aabbMax = data.aabbMax;
		Surface surface{};
		surface.firstIndex = static_cast<uint32_t>(
			r.indexByteOffset / sizeof(uint32_t));
		surface.indexCount = indexCount;
		surface.baseVertex = static_cast<int32_t>(r.vertexBase);
		mesh.geometry.surfaces.push_back(surface);
		// The optional lightmap window's published base (0xFFFFFFFF =
		// none) — the draw-ring fill reads it for the per-draw delta.
		mesh.lightmapBase = r.alloc.lightmapAllocation
			? static_cast<uint32_t>(r.alloc.lightmapBase) : 0xFFFFFFFFu;
		handles[i] = staticMeshes.create(std::move(mesh));
		if (handles[i].empty()) {
			// Roll the created entries back (frees their heap ranges via
			// meshAllocations_), then the failed entry's own fresh ranges.
			// The rolled-back DestroyMesh frees are now RETIREMENT-ORDERED
			// (deferred - see DeferFreeStaticMeshRanges): conservative by a
			// frame or two, never wrong (nothing of this batch was staged
			// yet). The failed entry's OWN fresh ranges free IN PLACE:
			// never staged, never referenced by any GPU work (the bulk
			// uploads happen only after every pool entry succeeds), so an
			// immediate retry of the batch still sees the full capacity.
			for (size_t j = 0; j < i; ++j)
				DestroyMesh(handles[j]);
			// The failed entry's OWN fresh ranges free in place: never
			// staged, never referenced by any GPU work (the bulk uploads
			// happen only after every pool entry succeeds), so an
			// immediate retry of the batch still sees the full capacity.
			FreeStaticMeshRangesNow(r.alloc.vertexAllocation,
				r.alloc.indexAllocation, r.alloc.lightmapAllocation);
			return {};
		}
		r.alloc.vertexBase = r.vertexBase;
		r.alloc.indexByteOffset = r.indexByteOffset;
		auto [it, inserted] = meshAllocations_.emplace(
			handles[i].index(),
			new StaticMeshAllocation(std::move(r.alloc)));
		assert(inserted);
		if (retainUploads_)
			retainedUploads_[handles[i].index()] = data;
	}

	// =========================================================================
	// 3. Convert AoS -> SoA-ish into ONE packed blob per heap stream
	// =========================================================================

	size_t totalVerts = 0;
	size_t totalIdx = 0;
	for (size_t i = 0; i < uploads.size(); ++i) {
		totalVerts += uploads[i].vertices.size();
		totalIdx += uploads[i].indices.size();
	}
	std::vector<PackedVertexPosition> positions(totalVerts);
	std::vector<PackedVertexShading> shading(totalVerts);
	std::vector<PackedVertexLightmap> lightmap(totalVerts);
	std::vector<uint32_t> indices(totalIdx);
	std::vector<BufferBulkCopy> posCopies;
	std::vector<BufferBulkCopy> shadeCopies;
	std::vector<BufferBulkCopy> lmCopies;
	std::vector<BufferBulkCopy> idxCopies;
	posCopies.reserve(uploads.size());
	shadeCopies.reserve(uploads.size());
	lmCopies.reserve(uploads.size());
	idxCopies.reserve(uploads.size());
	size_t srcVert = 0;
	size_t srcIdx = 0;
	for (size_t i = 0; i < uploads.size(); ++i) {
		const StaticMeshUpload& data = uploads[i];
		MeshRange& r = ranges[i];
		const uint32_t vertexCount =
			static_cast<uint32_t>(data.vertices.size());
		const uint32_t indexCount =
			static_cast<uint32_t>(data.indices.size());
		PackedVertexPosition* posOut = positions.data() + srcVert;
		PackedVertexShading* shadeOut = shading.data() + srcVert;
		PackedVertexLightmap* lmOut = lightmap.data() + srcVert;
		for (uint32_t v = 0; v < vertexCount; ++v)
			PackStaticVertex(data.vertices[v], posOut[v], shadeOut[v],
				lmOut[v]);
		if (indexCount > 0) {
			memcpy(indices.data() + srcIdx, data.indices.data(),
				size_t(indexCount) * sizeof(uint32_t));
			idxCopies.push_back(BufferBulkCopy{
				srcIdx * sizeof(uint32_t),
				size_t(r.indexByteOffset),
				size_t(indexCount) * sizeof(uint32_t) });
		}
		posCopies.push_back(BufferBulkCopy{
			srcVert * sizeof(PackedVertexPosition),
			size_t(r.vertexBase) * sizeof(PackedVertexPosition),
			size_t(vertexCount) * sizeof(PackedVertexPosition) });
		shadeCopies.push_back(BufferBulkCopy{
			srcVert * sizeof(PackedVertexShading),
			size_t(r.vertexBase) * sizeof(PackedVertexShading),
			size_t(vertexCount) * sizeof(PackedVertexShading) });
		// The lightmap stream packs CONTIGUOUSLY in the staging blob
		// (only charted meshes contribute); the destination addresses
		// each mesh's independently-carved window. The source slice
		// rides the SAME srcVert slot as the shading words (the
		// lightmap vector is vertex-parallel).
		if (r.alloc.lightmapAllocation) {
			lmCopies.push_back(BufferBulkCopy{
				srcVert * sizeof(PackedVertexLightmap),
				size_t(r.alloc.lightmapBase)
					* sizeof(PackedVertexLightmap),
				size_t(vertexCount) * sizeof(PackedVertexLightmap) });
		}
		srcVert += vertexCount;
		srcIdx += indexCount;
	}

	// =========================================================================
	// 4. Four table-driven bulk uploads (one per heap; the RHI stages 512MB
	//    blobs — one memcpy pass + one submit + one barrier set per blob)
	// =========================================================================

	ctx_->BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexPosBuffer,
		positions.data(), posCopies,
		PipelineStageBit_Vertex,
		AccessFlags::VertexAttributeRead);
	ctx_->BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexShadingBuffer,
		shading.data(), shadeCopies,
		PipelineStageBit_Vertex,
		AccessFlags::VertexAttributeRead);
	if (!lmCopies.empty()) {
		ctx_->BulkCopyBuffer(
			staticVertexFmtHeap.m_staticVertexLightmapBuffer,
			lightmap.data(), lmCopies,
			PipelineStageBit_All,
			AccessFlags::ShaderRead);
	}
	if (!idxCopies.empty()) {
		ctx_->BulkCopyBuffer(staticVertexFmtHeap.m_staticIndexBuffer,
			indices.data(), idxCopies,
			PipelineStageBit_Vertex,
			AccessFlags::IndexRead);
	}
	return handles;
}

std::vector<smvk::ClusterStaticMeshHandle> smvk::Cluster::UploadStaticMeshesDirect(
	const void* content, std::span<const StaticMeshDirectSource> sources)
{
	std::vector<ClusterStaticMeshHandle> handles;
	if (!content || sources.empty())
		return handles;
	assert(!offlineMode_ && "UploadStaticMeshesDirect is a GPU-context fast path");

	// =========================================================================
	// 1. Allocate heap ranges (all-or-nothing — nothing staged on failure)
	// =========================================================================
	struct MeshRange {
		uint64_t vertexBase = 0; // VERTEX UNITS
		uint64_t indexByteOffset = 0;
		StaticMeshAllocation alloc{};
	};
	std::vector<MeshRange> ranges(sources.size());
	// Same all-or-nothing rollback as UploadStaticMeshes' carve loop (the
	// old bare `return {}` leaked every carved range; in place is safe -
	// nothing is staged before every range is carved + every pool entry
	// created).
	auto failCarved = [&ranges](size_t upto) {
		for (size_t j = 0; j < upto; ++j)
			FreeStaticMeshRangesNow(ranges[j].alloc.vertexAllocation,
				ranges[j].alloc.indexAllocation,
				ranges[j].alloc.lightmapAllocation);
	};
	for (size_t i = 0; i < sources.size(); ++i) {
		const StaticMeshDirectSource& src = sources[i];
		if (src.vertexCount == 0) {
			failCarved(i); // degenerate entry fails the whole batch
			return {};
		}
		MeshRange& r = ranges[i];
		{
			r.alloc.vertexAllocation =
				staticVertexFmtHeap.m_staticVertexBlock.Allocate(
					src.vertexCount, 1, r.vertexBase);
			if (r.alloc.vertexAllocation == 0) {
				failCarved(i);
				return {};
			}
		}
		if (src.indexCount > 0) {
			const uint64_t indexBytes = sizeof(uint32_t)
				* static_cast<uint64_t>(src.indexCount);
			r.alloc.indexAllocation =
				staticVertexFmtHeap.m_staticIndexBlock.Allocate(
					indexBytes, alignof(uint32_t), r.indexByteOffset);
			if (r.alloc.indexAllocation == 0) {
				failCarved(i + 1);
				return {};
			}
		}
		// The OPTIONAL lightmap range: the source blob carries the
		// stream only for charted meshes (lmSrc != 0).
		if (src.lmSrc != 0) {
			uint64_t lmBase = 0;
			r.alloc.lightmapAllocation =
				staticVertexFmtHeap.m_staticLightmapBlock.Allocate(
					src.vertexCount, 1, lmBase);
			if (r.alloc.lightmapAllocation == 0) {
				failCarved(i + 1);
				return {};
			}
			r.alloc.lightmapBase = lmBase;
		}
	}

	// =========================================================================
	// 2. Pool entries + bookkeeping (BEFORE any GPU staging)
	// =========================================================================
	handles.resize(sources.size());
	for (size_t i = 0; i < sources.size(); ++i) {
		const StaticMeshDirectSource& src = sources[i];
		MeshRange& r = ranges[i];
		StaticMesh mesh{};
		mesh.geometry.topology = PrimitiveTopology::TriangleList;
		mesh.geometry.vertexCount = src.vertexCount;
		mesh.geometry.indexCount = src.indexCount;
		mesh.geometry.aabbMin = src.aabbMin;
		mesh.geometry.aabbMax = src.aabbMax;
		Surface surface{};
		surface.firstIndex = static_cast<uint32_t>(
			r.indexByteOffset / sizeof(uint32_t));
		surface.indexCount = src.indexCount;
		surface.baseVertex = static_cast<int32_t>(r.vertexBase);
		mesh.geometry.surfaces.push_back(surface);
		// The optional lightmap window's published base (0xFFFFFFFF =
		// none) — the draw-ring fill reads it for the per-draw delta.
		mesh.lightmapBase = r.alloc.lightmapAllocation
			? static_cast<uint32_t>(r.alloc.lightmapBase) : 0xFFFFFFFFu;
		handles[i] = staticMeshes.create(std::move(mesh));
		if (handles[i].empty()) {
			// Same rollback contract as UploadStaticMeshes: the rolled-back
			// DestroyMesh frees are retirement-ordered (deferred, never
			// staged); the failed entry's OWN fresh ranges free in place
			// (nothing was staged - the bulk copies happen only after every
			// pool entry succeeds).
			for (size_t j = 0; j < i; ++j)
				DestroyMesh(handles[j]);
			// Same as UploadStaticMeshes: the failed entry's own fresh
			// ranges free in place (nothing was staged - the bulk copies
			// happen only after every pool entry succeeds).
			FreeStaticMeshRangesNow(r.alloc.vertexAllocation,
				r.alloc.indexAllocation, r.alloc.lightmapAllocation);
			return {};
		}
		r.alloc.vertexBase = r.vertexBase;
		r.alloc.indexByteOffset = r.indexByteOffset;
		auto [it, inserted] = meshAllocations_.emplace(
			handles[i].index(),
			new StaticMeshAllocation(std::move(r.alloc)));
		assert(inserted);
	}

	// =========================================================================
	// 3. THREE table-driven bulk uploads straight from `content` — the
	//    sources are packed-SoA subranges of one blob (the loaded pak
	//    image), so this is pure offset math: no AoS parse, no conversion,
	//    no intermediate vectors.
	// =========================================================================
	std::vector<BufferBulkCopy> posCopies;
	std::vector<BufferBulkCopy> shadeCopies;
	std::vector<BufferBulkCopy> idxCopies;
	std::vector<BufferBulkCopy> lmCopies;
	posCopies.reserve(sources.size());
	shadeCopies.reserve(sources.size());
	lmCopies.reserve(sources.size());
	idxCopies.reserve(sources.size());
	for (size_t i = 0; i < sources.size(); ++i) {
		const StaticMeshDirectSource& src = sources[i];
		MeshRange& r = ranges[i];
		posCopies.push_back(BufferBulkCopy{
			size_t(src.posSrc),
			size_t(r.vertexBase) * sizeof(PackedVertexPosition),
			size_t(src.vertexCount) * sizeof(PackedVertexPosition) });
		shadeCopies.push_back(BufferBulkCopy{
			size_t(src.shadeSrc),
			size_t(r.vertexBase) * sizeof(PackedVertexShading),
			size_t(src.vertexCount) * sizeof(PackedVertexShading) });
		if (src.lmSrc != 0) {
			lmCopies.push_back(BufferBulkCopy{
				size_t(src.lmSrc),
				size_t(r.alloc.lightmapBase)
					* sizeof(PackedVertexLightmap),
				size_t(src.vertexCount) * sizeof(PackedVertexLightmap) });
		}
		if (src.indexCount > 0) {
			idxCopies.push_back(BufferBulkCopy{
				size_t(src.idxSrc),
				size_t(r.indexByteOffset),
				size_t(src.indexCount) * sizeof(uint32_t) });
		}
	}
	ctx_->BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexPosBuffer,
		content, posCopies,
		PipelineStageBit_Vertex,
		AccessFlags::VertexAttributeRead);
	ctx_->BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexShadingBuffer,
		content, shadeCopies,
		PipelineStageBit_Vertex,
		AccessFlags::VertexAttributeRead);
	if (!lmCopies.empty()) {
		ctx_->BulkCopyBuffer(
			staticVertexFmtHeap.m_staticVertexLightmapBuffer,
			content, lmCopies,
			PipelineStageBit_All,
			AccessFlags::ShaderRead);
	}
	if (!idxCopies.empty()) {
		ctx_->BulkCopyBuffer(staticVertexFmtHeap.m_staticIndexBuffer,
			content, idxCopies,
			PipelineStageBit_Vertex,
			AccessFlags::IndexRead);
	}
	return handles;
}


// -----------------------------------------------------------------------------
// Geometry LOD streaming (Renderer/geometry_streamer.h): the deferred
// sibling record + the promote path (see the header block). The promote
// replays EXACTLY the deserializer's post-upload sequence - a promoted
// sibling is indistinguishable from a load-time one.
// -----------------------------------------------------------------------------

uint64_t smvk::Cluster::DeferMeshLod(DeferredMeshLod&& d) {
	// Payload accounting: the pos + shading streams are interleaved in
	// the vertex block (VERTEX units - the arena counts vertices, the
	// bytes are the physical cost) + the index bytes. Both matter to
	// the streamer's budget, so account the physical bytes: vertices
	// cost ~28-32 B each (pos3 + shading stream), indices 4 B.
	const uint64_t vertexBytes =
		uint64_t(d.src.vertexCount) * 32ull; // upper bound (pos+shade+pad)
	const uint64_t indexBytes =
		uint64_t(d.src.indexCount) * sizeof(uint32_t);
	d.payloadBytes = vertexBytes + indexBytes;
	deferredLods_.push_back(std::move(d));
	return vertexBytes + indexBytes;
}

const smvk::Cluster::DeferredMeshLod* smvk::Cluster::FindDeferredMeshLod(
	StringViewT uri) const
{
	for (const DeferredMeshLod& d : deferredLods_)
		if (uri == d.uri)
			return &d;
	return nullptr;
}

size_t smvk::Cluster::DeferredLodCount() const {
	size_t n = 0;
	for (const DeferredMeshLod& d : deferredLods_)
		if (!d.promotedHandle.valid())
			++n;
	return n;
}

uint64_t smvk::Cluster::DeferredLodBytes() const {
	uint64_t b = 0;
	for (const DeferredMeshLod& d : deferredLods_)
		if (!d.promotedHandle.valid())
			b += d.payloadBytes;
	return b;
}

std::optional<smvk::Cluster::PromotedLod>
smvk::Cluster::PromoteDeferredMeshLod(StringViewT uri)
{
	DeferredMeshLod* rec = nullptr;
	for (DeferredMeshLod& d : deferredLods_) {
		if (uri != d.uri)
			continue;
		if (!d.promotedHandle.valid())
			rec = &d;
		break; // one record per uri; skip already-promoted ones
	}
	if (rec == nullptr)
		return std::nullopt;
	if (!pakImagePin_ || pakImagePin_->empty())
		return std::nullopt; // no payload source: cannot promote

	// One direct upload from the pinned pak image (the deserializer's
	// batch path with a single source - fresh arena ranges, table-driven
	// copies through the RHI upload streamer).
	const StaticMeshDirectSource srcs[1] = { rec->src };
	const std::vector<ClusterStaticMeshHandle> handles =
		UploadStaticMeshesDirect(pakImagePin_->data(), srcs);
	if (handles.size() != 1 || !handles[0].valid())
		return std::nullopt;
	const ClusterStaticMeshHandle h = handles[0];

	// The deserializer's meta re-attach (cluster_serializer.cpp's
	// post-upload block, verbatim): sphere + lightmap chunk/xforms +
	// cooked meshlets. Skinned never defers (the v1 exclusion), so
	// skinned stays false here. The fields MOVE OUT of the record (the
	// evict path moves them back).
	if (StaticMesh* m = staticMeshes.get(h)) {
		m->geometry.sphereRadius = rec->sphere;
		m->lmChunk = rec->lmChunk;
		m->lmChartXforms = std::move(rec->lmXforms);
		if (!rec->meshlets.meshlets.empty())
			m->meshlets = std::move(rec->meshlets);
		m->skinned = false;
	}
	RegisterMeshUri(rec->uri, h);
	// LodInfo was registered at DEFER time (RebuildLodChains needed it
	// to see the full chain) - re-registering is an idempotent refresh.
	RegisterMeshLod(rec->uri, rec->lod);
	if (const StaticMesh* m = staticMeshes.get(h)) {
		if (m->meshlets && !m->meshlets->empty())
			RegisterMeshMeshlets(rec->uri, *m->meshlets);
	}
	// The record STAYS with promotedHandle set - the evict path's
	// source. Its payload fields are spent (moved into the mesh); evict
	// re-fills them from the mesh before destroying it.
	rec->promotedHandle = h;

	return PromotedLod{ h, rec->payloadBytes };
}

uint64_t smvk::Cluster::EvictMeshLod(StringViewT uri)
{
	DeferredMeshLod* rec = nullptr;
	for (DeferredMeshLod& d : deferredLods_) {
		if (uri != d.uri)
			continue;
		if (d.promotedHandle.valid())
			rec = &d;
		break;
	}
	if (rec == nullptr)
		return 0;
	const ClusterStaticMeshHandle h = rec->promotedHandle;

	// Move the mesh's runtime fields back into the record BEFORE the
	// destroy (the pool entry dies with them otherwise) - a re-promote
	// is then byte-identical to the first one.
	if (StaticMesh* m = staticMeshes.get(h)) {
		rec->sphere = m->geometry.sphereRadius;
		rec->lmChunk = m->lmChunk;
		rec->lmXforms = std::move(m->lmChartXforms);
		if (m->meshlets)
			rec->meshlets = std::move(*m->meshlets);
	}
	rec->promotedHandle = {};

	// The mesh URI must MISS again (RebuildLodChains' deferred
	// detection = GetMeshLod hit + FindMeshByUri miss). NOT
	// UnregisterUri() - that erases meshLods_/meshMeshlets_ too, and
	// the LodInfo metadata is exactly what must SURVIVE eviction (the
	// chain builder + the next promote both need it). The targeted
	// erase keeps every other registry row intact.
	meshByUri_.Erase(uri);
	meshMeshlets_.Erase(uri);

	// DestroyMesh: pool entry + bookkeeping now, the heap ranges retire
	// DEFERRED (DeferFreeStaticMeshRanges - in-flight frames may still
	// be drawing this sibling's ranges; the arena hands them back only
	// after the +MaxFramesInFlight margin).
	DestroyMesh(h);

	return rec->payloadBytes;
}

// -----------------------------------------------------------------------------
// Mesh destruction
// -----------------------------------------------------------------------------

void smvk::Cluster::DestroyMesh(
	smvk::ClusterStaticMeshHandle handle)
{
	if (handle.empty())
		return;

	retainedUploads_.erase(handle.index());

	auto it =
		meshAllocations_.find(handle.index());

	if (it != meshAllocations_.end()) {

		StaticMeshAllocation* allocation = it->second;

		// Detach the range ids FIRST: the CPU bookkeeping dies now, the
		// physical heap ranges retire below. An in-flight frame can still
		// be DRAWING these ranges - the frees must land behind the last
		// published graphics point (DeferFreeStaticMeshRanges), or the
		// virtual block hands the range to a new mesh while the old
		// frame's draws read the new mesh's data through the old draw
		// args (garbage).
		const StaticHeapArena::AllocId vertexAllocation =
			allocation->vertexAllocation;
		const StaticHeapArena::AllocId indexAllocation =
			allocation->indexAllocation;
		const StaticHeapArena::AllocId lightmapAllocation =
			allocation->lightmapAllocation;

		delete allocation;
		meshAllocations_.erase(it);

		DeferFreeStaticMeshRanges(
			ctx_,
			vertexAllocation, indexAllocation, lightmapAllocation);
	}

	staticMeshes.destroy(handle);
}



// -----------------------------------------------------------------------------
// Material
// -----------------------------------------------------------------------------

smvk::ClusterMaterialHandle smvk::Cluster::CreateMaterial(
	smvk::TextureHandle diffuse)
{
	MaterialCreateInfo ci{};
	ci.diffuse = diffuse;
	ci.baseColorTexture = diffuse;
	return CreateMaterial(ci);
}

smvk::ClusterMaterialHandle smvk::Cluster::CreateMaterial(
	const MaterialCreateInfo& info)
{
	// Resolve baseColor texture – prefer baseColorTexture, fall back to legacy diffuse
	TextureHandle baseTex = !info.baseColorTexture.empty() ? info.baseColorTexture : info.diffuse;

	MaterialValues values{};
	values.diffuse = info.baseColorFactor;
	values.textures[0] = baseTex.empty() ? -1 : static_cast<int>(baseTex.index());
	values.textures[1] = info.metallicRoughnessTexture.empty() ? -1 : static_cast<int>(info.metallicRoughnessTexture.index());
	values.textures[2] = info.normalTexture.empty() ? -1 : static_cast<int>(info.normalTexture.index());
	values.textures[3] = info.occlusionTexture.empty() ? -1 : static_cast<int>(info.occlusionTexture.index());
	values.textures2[0] = info.emissiveTexture.empty() ? -1 : static_cast<int>(info.emissiveTexture.index());
	values.textures2[1] = -1;
	values.textures2[2] = -1;
	values.textures2[3] = -1;

	values.params.x = info.alphaCutoff;
	values.params.y = info.metallicFactor;
	values.params.z = info.roughnessFactor;
	values.params.w = static_cast<float>(static_cast<uint32_t>(info.shadingModel));

	values.params2.x = info.occlusionStrength;
	values.params2.y = info.normalScale;
	values.params2.z = info.emissiveStrength;
	values.params2.w = 0.0f;

	// The w word carries the per-material flag bits (mesh.h): the
	// receive-contact-shadows bit rides the old pad.
	values.emissiveFactor = glm::vec4(info.emissiveFactor,
		MaterialFlagsToWord(info.receiveContactShadows
			? kMaterialFlagReceiveContactShadows : 0u));
	// values.ambient keeps the MaterialValues default (0,0,0,0): the legacy
	// sky-ambient floor is off by default; ambient comes from the env/IBL
	// block instead. See comment in mesh.h.
	return CreateMaterialFromValues(
		values, info.shadingModel, info.alphaMode, info.alphaCutoff,
		info.doubleSided, info.castsShadows, info.allowFarVertexLit,
		info.farQuality);
}

	smvk::ClusterMaterialHandle smvk::Cluster::CreateMaterialFromValues(
		const MaterialValues& values,
		ShadingModel shadingModel,
		AlphaMode alphaMode,
		float alphaCutoff,
		bool doubleSided,
		bool castsShadows,
		bool allowFarVertexLit,
		ShaderLodQuality farQuality)
	{
		smvk::Material material{};
		material.values = values;
	// Authoritative alphaMode mirror for the shader (params2.w): Blend
	// materials need screen-door dither in the fragment shader, but
	// shadingModel alone cannot distinguish them from Opaque (both are
	// DefaultLit). Enforced here so cooked paks (whose blob predates the
	// encoding) backfill correctly on deserialize.
	material.values.params2.w = static_cast<float>(static_cast<uint32_t>(alphaMode));
	material.shadingModel = shadingModel;
	material.alphaMode = alphaMode;
	material.alphaCutoff = alphaCutoff;
	material.doubleSided = doubleSided;
	material.castsShadows = castsShadows;
	material.shaderLod.allowFarVertexLit = allowFarVertexLit;
	material.shaderLod.farQuality = farQuality;
	material.pipeline = MakePipelineStateForMaterial(alphaMode, shadingModel);
	return materials.create(std::move(material));
}

void smvk::Cluster::DestroyMaterial(smvk::ClusterMaterialHandle handle)
{
	materials.destroy(handle);
}

