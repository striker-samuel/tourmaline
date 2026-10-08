#include <Engine/Rendergraph/spotshadow.h>
#include <Engine/Renderer/light.h>

namespace smvk {

// LiSPSM (Light Space Perspective Shadow Maps) variant for spotlights.
// Warps the spot projection to match the camera's perspective frustum,
// providing better texel density in camera/view space.
// Based on LiSPSM (Light Space Perspective Shadow Maps) by Wimmer et al.
GpuCascadeDescription SpotShadowAtlas::BuildSpotTileLiSPSM(
	const glm::vec3& position,
	const glm::vec3& direction,
	float outerAngle,
	float range,
	int32_t shadowMapIndex,
	uint32_t tileIndex,
	const glm::mat4& cameraView,
	const glm::mat4& cameraProj,
	float cameraNear,
	float cameraFar,
	float nearPlane,
	float bias,
	float lightSize,
	float softness) const
{
	assert(Valid());
	assert(tileIndex < slotCount);

	GpuCascadeDescription tile{};
	// Compute LiSPSM view-projection matrix
	const glm::mat4 vp = smvk::GetSpotLightVPLiSPSM(
		position, direction, outerAngle, range, nearPlane, 1.0f,
		cameraView, cameraProj, cameraNear, cameraFar);
	std::memcpy(tile.lightVP, glm::value_ptr(vp), sizeof(tile.lightVP));
	tile.splitEnd = range;
	tile.bias = bias;
	tile.normalBias = 0.02f;
	tile.pad_ = 0.0f;
	tile.invShadowSize = glm::vec2(
		1.0f / static_cast<float>(tileExtent.width),
		1.0f / static_cast<float>(tileExtent.height));

	const float invAtlasW = 1.0f / static_cast<float>(atlasExtent.width);
	const float invAtlasH = 1.0f / static_cast<float>(atlasExtent.height);
	const uint32_t cols = Columns();
	const float offX = static_cast<float>((tileIndex % cols) * tileExtent.width);
	const float offY = static_cast<float>((tileIndex / cols) * tileExtent.height);
	tile.uvScale = glm::vec2(
		static_cast<float>(tileExtent.width) * invAtlasW,
		static_cast<float>(tileExtent.height) * invAtlasH);
	tile.uvBias = glm::vec2(offX * invAtlasW, offY * invAtlasH);
	const float texelU = 0.5f * invAtlasW;
	const float texelV = 0.5f * invAtlasH;
	tile.uvClampMin = tile.uvBias + glm::vec2(texelU, texelV);
	tile.uvClampMax = tile.uvBias + tile.uvScale - glm::vec2(texelU, texelV);
	// Soft shadows (SampleEvsmSpotTileSoft): near plane for the
	// eye-depth inversion, emitting-sphere diameter + artistic scalar
	// for the PCSS-style penumbra.
	tile.pad_ = nearPlane;
	tile.pad2_[0] = std::max(lightSize, 0.0f);
	tile.pad2_[1] = std::max(softness, 0.0f);
	(void)shadowMapIndex; // atlas index lives in the light / push constants
	return tile;
}

} // namespace smvk