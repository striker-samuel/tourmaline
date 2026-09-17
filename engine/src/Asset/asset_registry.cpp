#include <Engine/App/cluster.h>
#include <Engine/Asset/asset_registry.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace smvk {

// ---------------------------------------------------------------------------
// Asset URI normalization (view-based; see asset_registry.h for semantics).
// Fast path: already-normalized input returns the caller's view unchanged.
// Slow path: mirrors vfs::Normalize (separator collapsing, '.' skipping,
// '..' root-escape rejection, drive-letter/leading-slash rejection) but
// builds the result directly into the caller's scratch arena instead of
// round-tripping std::string.
// ---------------------------------------------------------------------------

StringViewT NormalizeAssetUri(StringViewT uri, Arena& scratch) {
	if (uri.empty())
		return {};
	if (UriAlreadyNormalized(uri))
		return uri; // includes the '#' fragment verbatim (contract)

	const auto [file, frag] = SplitAssetUri(uri);
	if (file.empty())
		return {}; // "#frag" has no file part

	// Reserved //flintbase/ scheme: keep the (lowercased, canonical)
	// prefix verbatim and normalize the remainder. Regular paths reject
	// drive-letter/leading-slash inputs.
	const bool scheme =
		vfs::HasFlintbasePrefix(std::string_view(file.data(), file.size()));
	StringViewT body = file;
	if (scheme) {
		const size_t kPrefixLen = vfs::kFlintbaseScheme.size();
		body = file.size() <= kPrefixLen ? StringViewT{} : file.substr(kPrefixLen);
	}
	else {
		if (file.size() >= 2 && file[1] == ':')
			return {}; // drive-letter absolute
		if (file[0] == '/' || file[0] == '\\')
			return {}; // posix absolute
	}

	// Segment pass: collapse separators, skip '.', reject "..".
	std::vector<StringViewT> parts;
	size_t start = 0;
	for (size_t i = 0; i <= body.size(); ++i) {
		if (i == body.size() || body[i] == '/' || body[i] == '\\') {
			const StringViewT seg = body.substr(start, i - start);
			start = i + 1;
			if (seg.empty() || (seg.size() == 1 && seg[0] == '.'))
				continue;
			if (seg.size() == 2 && seg[0] == '.' && seg[1] == '.')
				return {}; // escaped the virtual root
			parts.push_back(seg);
		}
	}
	if (parts.empty())
		return {};

	constexpr std::string_view kPrefix = "//flintbase/";
	size_t total = 1; // NUL
	if (scheme)
		total += kPrefix.size();
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i)
			total += 1;
		total += parts[i].size();
	}
	if (!frag.empty())
		total += 1 + frag.size();

	char* buf = static_cast<char*>(scratch.Allocate(total));
	size_t off = 0;
	if (scheme) {
		std::memcpy(buf, kPrefix.data(), kPrefix.size());
		off += kPrefix.size();
	}
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i)
			buf[off++] = '/';
		std::memcpy(buf + off, parts[i].data(), parts[i].size());
		off += parts[i].size();
	}
	if (!frag.empty()) {
		buf[off++] = '#';
		std::memcpy(buf + off, frag.data(), frag.size());
		off += frag.size();
	}
	buf[off] = '\0';
	return StringViewT(buf, off);
}

// ---------------------------------------------------------------------------
// Cluster URI registry: registration normalizes into the cluster-owned
// arena (keys stay valid until the cluster dies); lookups normalize into
// the frame arena (fast path: zero allocation) and probe the UriMap.
// ---------------------------------------------------------------------------

bool Cluster::RegisterMeshUri(StringViewT uri, ClusterStaticMeshHandle handle) {
	const StringViewT n = NormalizeAssetUri(uri, uriArena_);
	if (n.empty() || !handle.valid())
		return false;
	meshByUri_.Insert(uriArena_.AllocateString(n), handle);
	return true;
}

bool Cluster::RegisterMaterialUri(StringViewT uri, ClusterMaterialHandle handle) {
	const StringViewT n = NormalizeAssetUri(uri, uriArena_);
	if (n.empty() || !handle.valid())
		return false;
	materialByUri_.Insert(uriArena_.AllocateString(n), handle);
	return true;
}

bool Cluster::RegisterLightUri(StringViewT uri, ClusterLightHandle handle) {
	const StringViewT n = NormalizeAssetUri(uri, uriArena_);
	if (n.empty() || !handle.valid())
		return false;
	lightByUri_.Insert(uriArena_.AllocateString(n), handle);
	return true;
}

bool Cluster::RegisterTextureUri(StringViewT uri, TextureHandle handle) {
	const StringViewT n = NormalizeAssetUri(uri, uriArena_);
	if (n.empty() || !handle.valid())
		return false;
	textureByUri_.Insert(uriArena_.AllocateString(n), handle);
	return true;
}

void Cluster::UnregisterUri(StringViewT uri) {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return;
	meshByUri_.Erase(n);
	materialByUri_.Erase(n);
	lightByUri_.Erase(n);
	textureByUri_.Erase(n);
	textureSamplers_.Erase(n);
	textureSources_.Erase(n);
	meshLods_.Erase(n);
	meshMeshlets_.Erase(n);
}

std::optional<ClusterStaticMeshHandle> Cluster::FindMeshByUri(StringViewT uri) const {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const ClusterStaticMeshHandle* hit = meshByUri_.Find(n);
	if (!hit)
		return std::nullopt;
	return *hit;
}

std::optional<ClusterMaterialHandle> Cluster::FindMaterialByUri(StringViewT uri) const {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const ClusterMaterialHandle* hit = materialByUri_.Find(n);
	if (!hit)
		return std::nullopt;
	return *hit;
}

std::optional<ClusterLightHandle> Cluster::FindLightByUri(StringViewT uri) const {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const ClusterLightHandle* hit = lightByUri_.Find(n);
	if (!hit)
		return std::nullopt;
	return *hit;
}

std::optional<TextureHandle> Cluster::FindTextureByUri(StringViewT uri) const {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const TextureHandle* hit = textureByUri_.Find(n);
	if (!hit)
		return std::nullopt;
	return *hit;
}

StringViewT Cluster::GetMeshUri(ClusterStaticMeshHandle handle) const {
	if (!handle.valid())
		return {};
	StringViewT found;
	meshByUri_.ForEach([&](StringViewT uri, const ClusterStaticMeshHandle& h) {
		if (h == handle)
			found = uri;
	});
	return found;
}

StringViewT Cluster::GetMaterialUri(ClusterMaterialHandle handle) const {
	if (!handle.valid())
		return {};
	StringViewT found;
	materialByUri_.ForEach([&](StringViewT uri, const ClusterMaterialHandle& h) {
		if (h == handle)
			found = uri;
	});
	return found;
}

StringViewT Cluster::GetLightUri(ClusterLightHandle handle) const {
	if (!handle.valid())
		return {};
	StringViewT found;
	lightByUri_.ForEach([&](StringViewT uri, const ClusterLightHandle& h) {
		if (h == handle)
			found = uri;
	});
	return found;
}

ClusterLightHandle Cluster::CreateLight(SceneLight def) {
	return lights.create(std::move(def));
}

void Cluster::RegisterTextureSampler(StringViewT uri, const TextureSamplerDesc& desc) {
	const StringViewT n = NormalizeAssetUri(uri, uriArena_);
	if (n.empty())
		return;
	textureSamplers_.Insert(uriArena_.AllocateString(n), desc);
}

std::optional<TextureSamplerDesc> Cluster::GetTextureSampler(StringViewT uri) const {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const TextureSamplerDesc* hit = textureSamplers_.Find(n);
	if (!hit)
		return std::nullopt;
	return *hit;
}

void Cluster::SetTextureSource(StringViewT uri, std::vector<uint8_t> bytes) {
	const StringViewT n = NormalizeAssetUri(uri, uriArena_);
	if (n.empty())
		return;
	textureSources_.Insert(uriArena_.AllocateString(n), std::move(bytes));
}

const std::vector<uint8_t>* Cluster::GetTextureSource(StringViewT uri) const {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return nullptr;
	return textureSources_.Find(n);
}

void Cluster::ReleaseTextureSources() {
	textureSources_.Clear();
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

void Cluster::RegisterMeshLod(StringViewT uri, const LodInfo& lod) {
	const StringViewT n = NormalizeAssetUri(uri, uriArena_);
	if (n.empty())
		return;
	meshLods_.Insert(uriArena_.AllocateString(n), lod);
}

std::optional<LodInfo> Cluster::GetMeshLod(StringViewT uri) const {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const LodInfo* hit = meshLods_.Find(n);
	if (!hit)
		return std::nullopt;
	return *hit;
}

std::vector<std::pair<StringViewT, LodInfo>> Cluster::ListMeshLods() const {
	std::vector<std::pair<StringViewT, LodInfo>> out;
	out.reserve(meshLods_.Size());
	meshLods_.ForEach([&](StringViewT uri, const LodInfo& lod) {
		out.emplace_back(uri, lod);
	});
	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
		return a.first < b.first;
	});
	return out;
}

void Cluster::RegisterMeshMeshlets(StringViewT uri, const CookedMeshlets& meshlets) {
	const StringViewT n = NormalizeAssetUri(uri, uriArena_);
	if (n.empty() || meshlets.empty())
		return;
	meshMeshlets_.Insert(uriArena_.AllocateString(n), meshlets);
}

std::optional<CookedMeshlets> Cluster::GetMeshMeshlets(StringViewT uri) const {
	const StringViewT n = NormalizeAssetUri(uri);
	if (n.empty())
		return std::nullopt;
	const CookedMeshlets* hit = meshMeshlets_.Find(n);
	if (!hit)
		return std::nullopt;
	return *hit;
}

std::vector<std::pair<StringViewT, TextureHandle>> Cluster::ListTextureUris() const {
	std::vector<std::pair<StringViewT, TextureHandle>> out;
	out.reserve(textureByUri_.Size());
	textureByUri_.ForEach([&](StringViewT uri, const TextureHandle& h) {
		out.emplace_back(uri, h);
	});
	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
		return a.first < b.first;
	});
	return out;
}

} // namespace smvk
