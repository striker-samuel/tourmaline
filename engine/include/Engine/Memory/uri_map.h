#pragma once
// UriMap: open-addressing (FNV-1a, linear probe) hash map for the Cluster
// URI registries, replacing std::unordered_map<std::string, ...>.
//
// Keys are NON-OWNING StringViewT slices — in practice the per-Cluster
// arena (Cluster::uriArena_), whose lifetime encloses the map's. Lookups
// hash the caller's view directly: no key conversion, no allocation.
// Erase uses tombstones; growth rehashes when used+dead entries exceed
// 70% of the slot count.

#include <cstdint>
#include <utility>
#include <vector>

#include <Engine/Memory/string_view.h>

namespace smvk {

template<typename V>
class UriMap {
public:
	UriMap() { slots_.resize(16); }

	// O(1) lookup, allocation-free (hashes the view bytes).
	[[nodiscard]] V* Find(StringViewT key) {
		if (key.empty())
			return nullptr;
		return FindSlot(key);
	}
	[[nodiscard]] const V* Find(StringViewT key) const {
		if (key.empty())
			return nullptr;
		return const_cast<UriMap*>(this)->FindSlot(key);
	}
	[[nodiscard]] bool Contains(StringViewT key) const {
		return Find(key) != nullptr;
	}

	// Inserts or replaces. On replace the FIRST key view stays stored
	// (arena copy from the original insert — no re-allocation).
	V* Insert(StringViewT key, const V& value) {
		if (key.empty())
			return nullptr;
		if ((size_ + dead_ + 1) * 10 >= slots_.size() * 7)
			Grow();
		const uint32_t h = Mix(key);
		size_t i = h & (slots_.size() - 1);
		size_t firstDead = npos;
		while (true) {
			Slot& s = slots_[i];
			if (s.state == kUsed) {
				if (s.hash == h && s.key == key) {
					s.value = value;
					return &s.value;
				}
			}
			else if (s.state == kDead) {
				if (firstDead == npos)
					firstDead = i;
			}
			else { // kEmpty: claim here or at the first tombstone
				const size_t target = firstDead == npos ? i : firstDead;
				Slot& t = slots_[target];
				t.state = kUsed;
				t.hash = h;
				t.key = key;
				t.value = value;
				++size_;
				return &t.value;
			}
			i = (i + 1) & (slots_.size() - 1);
		}
	}

	bool Erase(StringViewT key) {
		if (key.empty())
			return false;
		const uint32_t h = Mix(key);
		size_t i = h & (slots_.size() - 1);
		while (true) {
			Slot& s = slots_[i];
			if (s.state == kEmpty)
				return false;
			if (s.state == kUsed && s.hash == h && s.key == key) {
				s.state = kDead;
				s.key = StringViewT();
				--size_;
				++dead_;
				return true;
			}
			i = (i + 1) & (slots_.size() - 1);
		}
	}

	void Clear() {
		slots_.assign(16, Slot{});
		size_ = 0;
		dead_ = 0;
	}

	[[nodiscard]] size_t Size() const { return size_; }

	// Iterates live entries (order unspecified; insertion-independent).
	template<typename Func>
	void ForEach(Func&& func) const {
		for (const Slot& s : slots_)
			if (s.state == kUsed)
				func(s.key, s.value);
	}
	template<typename Func>
	void ForEachMutable(Func&& func) {
		for (Slot& s : slots_)
			if (s.state == kUsed)
				func(s.key, s.value);
	}

private:
	enum : uint8_t { kEmpty = 0, kUsed = 1, kDead = 2 };
	static constexpr size_t npos = static_cast<size_t>(-1);

	struct Slot {
		StringViewT key;
		uint32_t hash = 0;
		uint8_t state = kEmpty;
		V value{};
	};

	[[nodiscard]] static uint32_t Mix(StringViewT key) {
		const uint64_t h = StringViewHash{}(key);
		return static_cast<uint32_t>(h ^ (h >> 32)) | 1u; // never 0-friendly
	}

	[[nodiscard]] V* FindSlot(StringViewT key) {
		const uint32_t h = Mix(key);
		size_t i = h & (slots_.size() - 1);
		while (true) {
			Slot& s = slots_[i];
			if (s.state == kEmpty)
				return nullptr;
			if (s.state == kUsed && s.hash == h && s.key == key)
				return &s.value;
			i = (i + 1) & (slots_.size() - 1);
		}
	}

	void Grow() {
		std::vector<Slot> old(slots_.size() * 2, Slot{});
		old.swap(slots_);
		size_ = 0;
		dead_ = 0;
		for (Slot& s : old) {
			if (s.state != kUsed)
				continue;
			Insert(s.key, s.value); // fresh table: no rehash loop
		}
	}

	std::vector<Slot> slots_;
	size_t size_ = 0;
	size_t dead_ = 0;
};

} // namespace smvk
