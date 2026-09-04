#include <Engine/Asset/meshlet.h>

#include <algorithm>
#include <cstring>

#include <meshoptimizer.h>

#include <Engine/Renderer/mesh.h>

namespace smvk {

size_t CookedMeshlets::byteSize() const
{
	return meshlets.size() * sizeof(Meshlet) +
		vertices.size() * sizeof(uint32_t) +
		triangles.size();
}

bool BuildMeshletsForUpload(
	const StaticMeshUpload& upload,
	const MeshletConfig& config,
	CookedMeshlets* out,
	MeshletStats* outStats,
	std::string* err)
{
	auto fail = [&](const std::string& msg) {
		if (out) {
			out->meshlets.clear();
			out->vertices.clear();
			out->triangles.clear();
		}
		if (err)
			*err = msg;
		return false;
	};

	if (!out)
		return fail("null output");
	out->meshlets.clear();
	out->vertices.clear();
	out->triangles.clear();
	if (outStats)
		*outStats = MeshletStats{};

	if (!config.enabled)
		return fail("meshlets disabled");
	const size_t vcount = upload.vertices.size();
	const size_t icount = upload.indices.size();
	if (vcount == 0 || icount < 3 || (icount % 3) != 0)
		return fail("degenerate mesh");

	uint32_t maxVerts = std::clamp(config.maxVertices, 3u, 255u);
	uint32_t maxTris = std::clamp(config.maxTriangles, 1u, 512u);

	const size_t bound = meshopt_buildMeshletsBound(icount, maxVerts, maxTris);
	if (bound == 0)
		return fail("meshopt_buildMeshletsBound returned 0");

	std::vector<meshopt_Meshlet> raw(bound);
	std::vector<unsigned int> rawVerts(bound * maxVerts);
	std::vector<unsigned char> rawTris(bound * maxTris * 3);

	const float* positions = vcount > 0 ? &upload.vertices[0].pos.x : nullptr;
	const size_t count = meshopt_buildMeshlets(
		raw.data(), rawVerts.data(), rawTris.data(),
		upload.indices.data(), icount,
		positions, vcount, sizeof(StaticVertex),
		maxVerts, maxTris, config.coneWeight);
	if (count == 0)
		return fail("meshopt_buildMeshlets returned 0");

	CookedMeshlets cooked;
	cooked.meshlets.reserve(count);

	size_t vertCursor = 0;
	size_t triCursor = 0;
	// First pass: optimize + measure so we can reserve exact storage.
	for (size_t i = 0; i < count; ++i) {
		meshopt_Meshlet& m = raw[i];
		meshopt_optimizeMeshlet(
			&rawVerts[m.vertex_offset], &rawTris[m.triangle_offset],
			m.triangle_count, m.vertex_count);
		vertCursor += m.vertex_count;
		triCursor += (size_t)m.triangle_count * 3;
	}
	cooked.vertices.reserve(vertCursor);
	cooked.triangles.reserve(triCursor);

	MeshletStats stats;
	stats.meshletCount = (uint32_t)count;

	vertCursor = 0;
	triCursor = 0;
	for (size_t i = 0; i < count; ++i) {
		const meshopt_Meshlet& m = raw[i];
		// Copy the meshlet's vertex window + triangle bytes.
		for (unsigned int k = 0; k < m.vertex_count; ++k)
			cooked.vertices.push_back(rawVerts[m.vertex_offset + k]);
		for (unsigned int k = 0; k < m.triangle_count * 3; ++k)
			cooked.triangles.push_back(rawTris[m.triangle_offset + k]);

		const meshopt_Bounds b = meshopt_computeMeshletBounds(
			&rawVerts[m.vertex_offset], &rawTris[m.triangle_offset],
			m.triangle_count, positions, vcount, sizeof(StaticVertex));

		Meshlet ml;
		ml.vertexOffset = (uint32_t)vertCursor;
		ml.vertexCount = m.vertex_count;
		ml.triangleOffset = (uint32_t)triCursor;
		ml.triangleCount = m.triangle_count;
		ml.boundCenter = glm::vec3(b.center[0], b.center[1], b.center[2]);
		ml.boundRadius = b.radius;
		ml.coneApex = glm::vec3(b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]);
		ml.coneAxis = glm::vec3(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
		ml.coneCutoff = b.cone_cutoff;
		cooked.meshlets.push_back(ml);

		vertCursor += m.vertex_count;
		triCursor += (size_t)m.triangle_count * 3;

		stats.totalTriangles += m.triangle_count;
		stats.maxVertsUsed = std::max(stats.maxVertsUsed, m.vertex_count);
		stats.maxTrisUsed = std::max(stats.maxTrisUsed, m.triangle_count);
	}

	*out = std::move(cooked);
	if (outStats)
		*outStats = stats;
	return true;
}

void BuildMeshletIndirectCommands(
	const CookedMeshlets& cooked,
	std::vector<uint32_t>& outIndices,
	std::vector<MeshletDrawCommand>& outCommands)
{
	outIndices.clear();
	outCommands.clear();
	outCommands.reserve(cooked.meshlets.size());
	size_t total = 0;
	for (const Meshlet& m : cooked.meshlets)
		total += (size_t)m.triangleCount * 3;
	outIndices.reserve(total);

	for (uint32_t i = 0; i < (uint32_t)cooked.meshlets.size(); ++i) {
		const Meshlet& m = cooked.meshlets[i];
		MeshletDrawCommand cmd;
		cmd.firstIndex = (uint32_t)outIndices.size();
		cmd.indexCount = m.triangleCount * 3;
		cmd.instanceCount = 1;
		cmd.baseVertex = 0;
		cmd.meshletIndex = i;
		for (uint32_t k = 0; k < m.triangleCount * 3; ++k) {
			const uint8_t local = cooked.triangles[m.triangleOffset + k];
			outIndices.push_back(cooked.vertices[m.vertexOffset + local]);
		}
		outCommands.push_back(cmd);
	}
}

} // namespace smvk
