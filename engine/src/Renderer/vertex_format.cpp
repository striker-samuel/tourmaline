// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Extracted from mesh.cpp: the vertex-format descriptions are backend-neutral
// data (the pipeline facade needs them for input layouts + strides on BOTH
// backends), while mesh.cpp's static-heap allocation is Vulkan-coupled. This
// TU compiles into `engine` AND `engine_d3d12` (never into one binary, so
// there is no ODR concern — the two libs are never linked together).

#include <Engine/smvk.h>
#include <Engine/Renderer/mesh.h>

#include <cassert>

namespace smvk {

const VertexLayoutDesc& GetVertexLayoutDesc(VertexLayoutId format) {
	switch (format)
	{
	case VertexLayoutId::Empty:
	{
		static const VertexLayoutDesc emptyFormat{};
		return emptyFormat;
	}
	case VertexLayoutId::Static:
		return StaticLayout;
	case VertexLayoutId::StaticLightmapped:
		// Gated-off lightmap option (see mesh.h): nothing binds it
		// while lightmaps stay deprecated; the case keeps the format
		// reachable the moment the chart-unpack VSs return.
		return StaticLightmappedLayout;
	case VertexLayoutId::Skinned:
		return SkinnedLayout;
	case VertexLayoutId::DebugLine:
		return DebugLineLayout;
	case VertexLayoutId::TerrainFull:
		return TerrainFullLayout;
	case VertexLayoutId::TerrainReduced:
		return TerrainReducedLayout;
	case VertexLayoutId::TerrainFarGrid:
		return TerrainFarLayout;
	}
	// An out-of-enum value can only come from memory corruption or a
	// garbage cast — an INVALID layout id must never masquerade as an
	// intentionally vertex-free pipeline (Empty above is the ONLY
	// legitimate empty layout). Loud in every build; the returned
	// description is unreachable fallback data.
	assert(false && "GetVertexLayoutDesc: unknown VertexLayoutId id");
	static const VertexLayoutDesc invalidFormat{};
	return invalidFormat;
}

} // namespace smvk
