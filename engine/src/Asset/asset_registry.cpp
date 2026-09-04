#include <Engine/App/cluster.h>
#include <Engine/Asset/asset_registry.h>

#include <algorithm>

namespace smvk {

bool Cluster::RegisterMeshUri(const std::string& uri, ClusterStaticMeshHandle handle) {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty() || !handle.valid())
		return false;
	meshByUri_[n] = handle;
	return true;
}

bool Cluster::RegisterMaterialUri(const std::string& uri, ClusterMaterialHandle handle) {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty() || !handle.valid())
		return false;
	materialByUri_[n] = handle;
	return true;
}

bool Cluster::RegisterTextureUri(const std::string& uri, TextureHandle handle) {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty() || !handle.valid())
		return false;
	textureByUri_[n] = handle;
	return true;
}

void Cluster::UnregisterUri(const std::string& uri) {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return;
	meshByUri_.erase(n);
	materialByUri_.erase(n);
	textureByUri_.erase(n);
	textureSamplers_.erase(n);
	textureSources_.erase(n);
	meshLods_.erase(n);
	meshMeshlets_.erase(n);
}

std::optional<ClusterStaticMeshHandle> Cluster::FindMeshByUri(const std::string& uri) const {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const auto it = meshByUri_.find(n);
	if (it == meshByUri_.end())
		return std::nullopt;
	return it->second;
}

std::optional<ClusterMaterialHandle> Cluster::FindMaterialByUri(const std::string& uri) const {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const auto it = materialByUri_.find(n);
	if (it == materialByUri_.end())
		return std::nullopt;
	return it->second;
}

std::optional<TextureHandle> Cluster::FindTextureByUri(const std::string& uri) const {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const auto it = textureByUri_.find(n);
	if (it == textureByUri_.end())
		return std::nullopt;
	return it->second;
}

std::string Cluster::GetMeshUri(ClusterStaticMeshHandle handle) const {
	if (!handle.valid())
		return {};
	for (const auto& [uri, h] : meshByUri_) {
		if (h == handle)
			return uri;
	}
	return {};
}

std::string Cluster::GetMaterialUri(ClusterMaterialHandle handle) const {
	if (!handle.valid())
		return {};
	for (const auto& [uri, h] : materialByUri_) {
		if (h == handle)
			return uri;
	}
	return {};
}

void Cluster::RegisterTextureSampler(const std::string& uri, const TextureSamplerDesc& desc) {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return;
	textureSamplers_[n] = desc;
}

std::optional<TextureSamplerDesc> Cluster::GetTextureSampler(const std::string& uri) const {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const auto it = textureSamplers_.find(n);
	if (it == textureSamplers_.end())
		return std::nullopt;
	return it->second;
}

void Cluster::SetTextureSource(const std::string& uri, std::vector<uint8_t> bytes) {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return;
	textureSources_[n] = std::move(bytes);
}

const std::vector<uint8_t>* Cluster::GetTextureSource(const std::string& uri) const {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return nullptr;
	const auto it = textureSources_.find(n);
	if (it == textureSources_.end())
		return nullptr;
	return &it->second;
}

void Cluster::ReleaseTextureSources() {
	textureSources_.clear();
}

void Cluster::SetRetainUploads(bool retain) {
	retainUploads_ = retain;
	if (!retain)
		retainedUploads_.clear();
}

void Cluster::SetCookMode(bool cook) {
	cookMode_ = cook;
	if (cook)
		SetRetainUploads(true);
}

uint32_t Cluster::AllocCookTextureId() {
	return cookNextTextureId_++;
}

const StaticMeshUpload* Cluster::GetRetainedUpload(ClusterStaticMeshHandle handle) const {
	if (!handle.valid())
		return nullptr;
	const auto it = retainedUploads_.find(handle.index());
	if (it == retainedUploads_.end())
		return nullptr;
	return &it->second;
}

void Cluster::RegisterMeshLod(const std::string& uri, const LodInfo& lod) {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return;
	meshLods_[n] = lod;
}

std::optional<LodInfo> Cluster::GetMeshLod(const std::string& uri) const {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const auto it = meshLods_.find(n);
	if (it == meshLods_.end())
		return std::nullopt;
	return it->second;
}

void Cluster::RegisterMeshMeshlets(const std::string& uri, const CookedMeshlets& meshlets) {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty() || meshlets.empty())
		return;
	meshMeshlets_[n] = meshlets;
}

std::optional<CookedMeshlets> Cluster::GetMeshMeshlets(const std::string& uri) const {
	const std::string n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const auto it = meshMeshlets_.find(n);
	if (it == meshMeshlets_.end())
		return std::nullopt;
	return it->second;
}

std::vector<std::pair<std::string, TextureHandle>> Cluster::ListTextureUris() const {
	std::vector<std::pair<std::string, TextureHandle>> out;
	out.reserve(textureByUri_.size());
	for (const auto& [uri, h] : textureByUri_)
		out.emplace_back(uri, h);
	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
		return a.first < b.first;
	});
	return out;
}

} // namespace smvk
