// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Extracted from mesh.cpp: the vertex-format descriptions are backend-neutral
// data (the pipeline facade needs them for input layouts + strides on BOTH
// backends), while mesh.cpp's static-heap allocation is Vulkan-coupled. This
// TU compiles into `engine` AND `engine_d3d12` (never into one binary, so
// there is no ODR concern — the two libs are never linked together).

#include <Engine/smvk.h>
#include <Engine/Renderer/mesh.h>

namespace smvk {

const VertexFormatDescription& GetVertexFormatDescription(VertexFormat format) {
	switch (format)
	{
	case VertexFormat::Empty:
	{
		static const VertexFormatDescription emptyFormat{};
		return emptyFormat;
	}
	case VertexFormat::StaticVertexFormat:
		return StaticVertexFormat;
	case VertexFormat::LightmappedStaticVertexFormat:
		// Gated-off lightmap option (see mesh.h): nothing binds it
		// while lightmaps stay deprecated; the case keeps the format
		// reachable the moment the chart-unpack VSs return.
		return LightmappedStaticVertexFormat;
	case VertexFormat::SkinnedVertexFormat:
		return SkinnedVertexFormat;
	case VertexFormat::DebugLine:
		return DebugLineFormat;
	case VertexFormat::TerrainVertexFormat:
		return TerrainVertexFormat;
	case VertexFormat::TerrainLowVertexFormat:
		return TerrainLowVertexFormat;
	case VertexFormat::TerrainFarVertexFormat:
		return TerrainFarVertexFormat;
	}
	static const VertexFormatDescription fallbackFormat{};
	return fallbackFormat;
}

} // namespace smvk
