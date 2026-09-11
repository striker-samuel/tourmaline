#include <Engine/Renderer/texture_streamer.h>
#include <Engine/RHI/resource_manager.h>

#include <algorithm>
#include <cmath>
#include <iostream>

#include "ktx.h"

namespace smvk {
namespace {

// Destroys the KTX object on scope exit (decode failures, upload throws).
struct KtxGuard {
	ktxTexture2* tex = nullptr;
	explicit KtxGuard(ktxTexture2* t) : tex(t) {}
	~KtxGuard() { if (tex) ktxTexture2_Destroy(tex); }
	KtxGuard(const KtxGuard&) = delete;
	KtxGuard& operator=(const KtxGuard&) = delete;
};

} // namespace

TextureStreamer::TextureStreamer(ResourceManager& ctx, TextureStreamerConfig config)
	: ctx_(ctx)
	, config_(config)
{
}

TextureStreamer::~TextureStreamer() = default;

uint64_t TextureStreamer::EstimateResidentBytes(uint32_t baseWidth, uint32_t baseHeight,
	uint32_t firstMip, uint32_t mipCount, uint32_t arrayLayers)
{
	uint64_t bytes = 0;
	for (uint32_t r = 0; r < mipCount; ++r)
	{
		const uint32_t m = firstMip + r;
		const uint32_t w = std::max(1u, baseWidth >> m);
		const uint32_t h = std::max(1u, baseHeight >> m);
		// BC7: fixed 16 bytes per 4x4 block.
		const uint64_t blocks =
			static_cast<uint64_t>((w + 3) / 4) * ((h + 3) / 4);
		bytes += blocks * 16u;
	}
	return bytes * std::max(1u, arrayLayers);
}

uint32_t TextureStreamer::maxBaseFor(uint32_t fullMips) const
{
	if (fullMips <= config_.minResidentMips)
		return 0;
	return fullMips - config_.minResidentMips;
}

uint32_t TextureStreamer::initialBaseFor(uint32_t fullMips, uint32_t baseLongSide) const
{
	uint32_t base = 0;
	const uint32_t maxBase = maxBaseFor(fullMips);
	while (base < maxBase)
	{
		const uint32_t dim =
			std::max(1u, baseLongSide >> base);
		if (dim <= config_.initialResidentDimCap)
			break;
		++base;
	}
	return base;
}

Holder<TextureHandle> TextureStreamer::StreamTexture(const void* payload, size_t payloadSize,
	const std::string& debugName, const ImageCreateInfo& imageCI)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (payload == nullptr || payloadSize == 0)
	{
		std::cerr << "TextureStreamer: empty payload for " << debugName << std::endl;
		return {};
	}

	ktxTexture2* ktxTex = nullptr;
	const KTX_error_code loadRes = ktxTexture2_CreateFromMemory(
		static_cast<const ktx_uint8_t*>(payload),
		static_cast<ktx_size_t>(payloadSize),
		KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
		&ktxTex);
	if (loadRes != KTX_SUCCESS)
	{
		std::cerr << "TextureStreamer: failed to decode " << debugName
			<< " code " << loadRes << std::endl;
		return {};
	}
	KtxGuard guard(ktxTex);

	const uint32_t fullMips = std::max(1u, ktxTex->numLevels);
	const uint32_t baseW = std::max(1u, ktxTex->baseWidth);
	const uint32_t baseH = std::max(1u, ktxTex->baseHeight);
	const uint32_t layers = std::max(1u, ktxTex->numLayers) *
		std::max(1u, ktxTex->numFaces);
	const uint32_t base = initialBaseFor(fullMips, std::max(baseW, baseH));

	try
	{
		ImageCreateInfo ci = imageCI;
		ci.hostTransfer = true;
		Holder<TextureHandle> holder = ctx_.CreateStreamingTexture(
			*ktxTex, base, fullMips - base, debugName.c_str(), ci);
		if (!holder.valid())
			return {};

		Entry e{};
		e.handle = holder;
		e.payloadData = payload;
		e.payloadSize = payloadSize;
		e.name = debugName;
		e.fullMips = fullMips;
		e.baseWidth = baseW;
		e.baseHeight = baseH;
		e.arrayLayers = layers;
		e.residentBase = base;
		e.desiredBase = base;
		e.lastRequestFrame = 0;
		e.lastChangeFrame = 0;
		entries_.emplace(EntryKey(holder), std::move(e));
		return holder;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "TextureStreamer: failed to upload " << debugName
			<< " : " << ex.what() << std::endl;
		return {};
	}
}

void TextureStreamer::Unregister(TextureHandle handle)
{
	std::lock_guard<std::mutex> lock(mutex_);
	entries_.erase(EntryKey(handle));
}

void TextureStreamer::RequestMip(TextureHandle handle, uint32_t wantedFullMip, uint32_t frameIndex)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = entries_.find(EntryKey(handle));
	if (it == entries_.end())
		return;
	Entry& e = it->second;
	const uint32_t want = std::min(wantedFullMip, e.fullMips - 1);
	if (!e.everRequested || want < e.desiredBase)
		e.desiredBase = want;
	e.everRequested = true;
	e.lastRequestFrame = frameIndex;
}

uint32_t TextureStreamer::ResolveFullMip(uint32_t residentBase, uint32_t fullMips, uint32_t reportedLod)
{
	if (fullMips == 0)
		return 0;
	const uint64_t want = static_cast<uint64_t>(residentBase) + reportedLod;
	return static_cast<uint32_t>(std::min<uint64_t>(want, fullMips - 1));
}

void TextureStreamer::RequestMipFromLod(TextureHandle handle, uint32_t residentLod, uint32_t frameIndex)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = entries_.find(EntryKey(handle));
	if (it == entries_.end())
		return;
	Entry& e = it->second;
	const uint32_t want = ResolveFullMip(e.residentBase, e.fullMips, residentLod);
	if (!e.everRequested || want < e.desiredBase)
		e.desiredBase = want;
	e.everRequested = true;
	e.lastRequestFrame = frameIndex;
}

std::vector<TextureHandle> TextureStreamer::BuildBindlessSlotMap() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<TextureHandle> map;
	for (const auto& [key, e] : entries_)
	{
		(void)key;
		const uint32_t slot = ctx_.GetTextureBindlessIndex(e.handle);
		if (slot >= map.size())
			map.resize(static_cast<size_t>(slot) + 1);
		map[slot] = e.handle;
	}
	return map;
}

void TextureStreamer::RequestMipsByDistance(const std::unordered_map<uint32_t, float>& minDistBySlot,
	float viewportHeightPx, float fovYRad, uint32_t frameIndex, float approxWorldSize)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto& [key, e] : entries_)
	{
		(void)key;
		const uint32_t slot = ctx_.GetTextureBindlessIndex(e.handle);
		auto hit = minDistBySlot.find(slot);
		if (hit == minDistBySlot.end())
			continue;
		const uint32_t want = WantedMipForDistance(e.fullMips,
			std::max(e.baseWidth, e.baseHeight), hit->second,
			viewportHeightPx, fovYRad, approxWorldSize, config_.mipBias);
		const uint32_t clamped = std::min(want, e.fullMips - 1);
		if (!e.everRequested || clamped < e.desiredBase)
			e.desiredBase = clamped;
		e.everRequested = true;
		e.lastRequestFrame = frameIndex;
	}
}

uint32_t TextureStreamer::WantedMipForScreenCoverage(uint32_t fullMips, uint32_t baseLongSide,
	float screenCoveragePx) const
{
	return WantedMipForScreenCoverage(fullMips, baseLongSide, screenCoveragePx, config_.mipBias);
}

uint32_t TextureStreamer::WantedMipForScreenCoverage(uint32_t fullMips, uint32_t baseLongSide,
	float screenCoveragePx, float mipBias)
{
	if (fullMips == 0)
		return 0;
	const float coverage = std::max(1.0f, screenCoveragePx);
	const float ratio = static_cast<float>(std::max(1u, baseLongSide)) / coverage;
	float mip = std::ceil(std::log2(std::max(1.0f, ratio))) + mipBias;
	mip = std::clamp(mip, 0.0f, static_cast<float>(fullMips - 1));
	return static_cast<uint32_t>(mip);
}

uint32_t TextureStreamer::WantedMipForDistance(uint32_t fullMips, uint32_t baseLongSide,
	float distance, float viewportHeightPx, float fovYRad, float approxWorldSize) const
{
	return WantedMipForDistance(fullMips, baseLongSide, distance,
		viewportHeightPx, fovYRad, approxWorldSize, config_.mipBias);
}

uint32_t TextureStreamer::WantedMipForDistance(uint32_t fullMips, uint32_t baseLongSide,
	float distance, float viewportHeightPx, float fovYRad, float approxWorldSize, float mipBias)
{
	const float dist = std::max(0.001f, distance);
	const float height = std::max(1.0f, viewportHeightPx);
	const float halfTan = std::tan(std::max(0.001f, fovYRad) * 0.5f);
	const float coverage = height * std::max(0.001f, approxWorldSize) /
		(2.0f * dist * std::max(1e-6f, halfTan));
	return WantedMipForScreenCoverage(fullMips, baseLongSide, coverage, mipBias);
}

bool TextureStreamer::retarget(Entry& e, uint32_t newBase, uint32_t frameIndex)
{
	if (newBase == e.residentBase)
		return true;

	ktxTexture2* ktxTex = nullptr;
	const KTX_error_code loadRes = ktxTexture2_CreateFromMemory(
		static_cast<const ktx_uint8_t*>(e.payloadData),
		static_cast<ktx_size_t>(e.payloadSize),
		KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
		&ktxTex);
	if (loadRes != KTX_SUCCESS)
	{
		std::cerr << "TextureStreamer: re-decode failed for " << e.name
			<< " code " << loadRes << std::endl;
		return false;
	}
	KtxGuard guard(ktxTex);

	try
	{
		ctx_.RetargetStreamingTexture(
			e.handle, *ktxTex, newBase, e.fullMips - newBase, e.name.c_str());
	}
	catch (const std::exception& ex)
	{
		std::cerr << "TextureStreamer: retarget failed for " << e.name
			<< " (base " << e.residentBase << " -> " << newBase
			<< ") : " << ex.what() << std::endl;
		return false;
	}

	e.residentBase = newBase;
	e.lastChangeFrame = frameIndex;
	return true;
}

void TextureStreamer::Update(uint32_t frameIndex)
{
	std::lock_guard<std::mutex> lock(mutex_);
	// 1. Decay demand for textures nobody asked for recently. Without this
	//    the first close-up would pin full res forever.
	for (auto& [key, e] : entries_)
	{
		(void)key;
		const bool stale = !e.everRequested ||
			(frameIndex - e.lastRequestFrame) > config_.evictCooldownFrames;
		if (stale)
			e.desiredBase = maxBaseFor(e.fullMips);
	}

	// 2. Upgrades: demand is sharper than residency. Biggest deficit first
	//    (pops in where it matters most), cheapest first on ties.
	struct Candidate { Entry* e; uint32_t deficit; };
	std::vector<Candidate> upgrades;
	for (auto& [key, e] : entries_)
	{
		(void)key;
		if (e.desiredBase < e.residentBase)
			upgrades.push_back({ &e, e.residentBase - e.desiredBase });
	}
	std::sort(upgrades.begin(), upgrades.end(),
		[](const Candidate& a, const Candidate& b)
		{
			if (a.deficit != b.deficit)
				return a.deficit > b.deficit;
			const uint64_t aBytes = EstimateResidentBytes(
				a.e->baseWidth, a.e->baseHeight, a.e->desiredBase,
				a.e->fullMips - a.e->desiredBase, a.e->arrayLayers);
			const uint64_t bBytes = EstimateResidentBytes(
				b.e->baseWidth, b.e->baseHeight, b.e->desiredBase,
				b.e->fullMips - b.e->desiredBase, b.e->arrayLayers);
			return aBytes < bBytes;
		});

	uint32_t upgradesDone = 0;
	for (Candidate& c : upgrades)
	{
		if (upgradesDone >= config_.maxUpgradesPerFrame)
			break;
		// Demand may have moved while iterating; re-check.
		if (c.e->desiredBase >= c.e->residentBase)
			continue;
		if (retarget(*c.e, c.e->desiredBase, frameIndex))
			++upgradesDone;
	}

	// 3. Downgrades: over budget, or idle-decayed demand coarser than
	//    residency. Largest residents first. Never below live demand
	//    (desiredBase is the floor here by construction).
	auto residentTotal = [&]()
	{
		uint64_t total = 0;
		for (const auto& [key, e] : entries_)
		{
			(void)key;
			total += EstimateResidentBytes(e.baseWidth, e.baseHeight,
				e.residentBase, e.fullMips - e.residentBase, e.arrayLayers);
		}
		return total;
	};

	uint32_t downgradesDone = 0;
	uint64_t total = residentTotal();
	if (total > config_.budgetBytes)
	{
		std::vector<Entry*> cands;
		for (auto& [key, e] : entries_)
		{
			(void)key;
			if (e.residentBase < maxBaseFor(e.fullMips) &&
				e.desiredBase > e.residentBase)
				cands.push_back(&e);
		}
		std::sort(cands.begin(), cands.end(),
			[&](Entry* a, Entry* b)
			{
				return EstimateResidentBytes(a->baseWidth, a->baseHeight,
					a->residentBase, a->fullMips - a->residentBase, a->arrayLayers) >
					EstimateResidentBytes(b->baseWidth, b->baseHeight,
					b->residentBase, b->fullMips - b->residentBase, b->arrayLayers);
			});
		for (Entry* e : cands)
		{
			if (total <= config_.budgetBytes ||
				downgradesDone >= config_.maxDowngradesPerFrame)
				break;
			// Jump straight to the demand floor: one realloc, not N.
			const uint32_t target = std::min(e->desiredBase, maxBaseFor(e->fullMips));
			if (target <= e->residentBase)
				continue;
			if (retarget(*e, target, frameIndex))
			{
				++downgradesDone;
				total = residentTotal();
			}
		}
	}

	// 4. Reset per-frame demand. Next frame's RequestMip() calls rebuild it;
	//    textures nobody requests decay via the staleness check above.
	//    (desiredBase is intentionally NOT reset here – the decay in step 1
	//    handles it on a cooldown, giving hysteresis for free.)
	(void)frameIndex;
	statsUpgrades_ = upgradesDone;
	statsDowngrades_ = downgradesDone;
}

TextureStreamingStats TextureStreamer::GetStats() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	TextureStreamingStats s{};
	s.textureCount = static_cast<uint32_t>(entries_.size());
	s.upgradesLastUpdate = statsUpgrades_;
	s.downgradesLastUpdate = statsDowngrades_;
	for (const auto& [key, e] : entries_)
	{
		(void)key;
		s.fullBytes += EstimateResidentBytes(e.baseWidth, e.baseHeight,
			0, e.fullMips, e.arrayLayers);
		s.residentBytes += EstimateResidentBytes(e.baseWidth, e.baseHeight,
			e.residentBase, e.fullMips - e.residentBase, e.arrayLayers);
		if (e.residentBase == 0)
			++s.fullResidentCount;
	}
	return s;
}

std::vector<TextureStreamEntryDebug> TextureStreamer::GetEntriesDebug() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<TextureStreamEntryDebug> out;
	out.reserve(entries_.size());
	for (const auto& [key, e] : entries_)
	{
		(void)key;
		TextureStreamEntryDebug d{};
		d.handleIndex = e.handle.index();
		d.handleGen = e.handle.gen();
		d.name = e.name;
		d.fullMips = e.fullMips;
		d.residentBase = e.residentBase;
		d.desiredBase = e.desiredBase;
		d.residentWidth = std::max(1u, e.baseWidth >> e.residentBase);
		d.residentHeight = std::max(1u, e.baseHeight >> e.residentBase);
		d.residentBytes = EstimateResidentBytes(e.baseWidth, e.baseHeight,
			e.residentBase, e.fullMips - e.residentBase, e.arrayLayers);
		d.fullBytes = EstimateResidentBytes(e.baseWidth, e.baseHeight,
			0, e.fullMips, e.arrayLayers);
		d.lastRequestFrame = e.lastRequestFrame;
		out.push_back(std::move(d));
	}
	std::sort(out.begin(), out.end(),
		[](const TextureStreamEntryDebug& a, const TextureStreamEntryDebug& b)
		{ return a.residentBytes > b.residentBytes; });
	return out;
}

} // namespace smvk
