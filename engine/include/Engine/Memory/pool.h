// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>
#include <atomic>
#include <bit> // std::countr_zero (the lease-mask bit index)

#include <Engine/smvk.h>
#include <Engine/Memory/alloc_config.h>

namespace smvk {

	// Forward declare destroy overloads (defined in RHI/resource_manager.cpp private TU)
	void destroy(ResourceManager* ctx, ImageHandle handle);
	void destroy(ResourceManager* ctx, TextureHandle handle);
	void destroy(ResourceManager* ctx, BufferHandle handle);
	void destroy(ResourceManager* ctx, SamplerHandle handle);
	void destroy(ResourceManager* ctx, BindlessImageHandle handle);
	void destroy(ResourceManager* ctx, ImageViewHandle handle);
	void destroy(ResourceManager* ctx, DescriptorPoolHandle handle);
	void destroy(ResourceManager* ctx, ShaderModuleHandle handle);

	template<typename HandleType>
	class Holder final {
	public:
		Holder() = default;
		template<typename Ctx>
		Holder(Ctx* ctx, HandleType handle) : ctx_(ctx), handle_(handle) {
			deleter_ = [](void* c, HandleType h){ destroy(static_cast<Ctx*>(c), h); };
		}
		~Holder() {
			if (ctx_ && deleter_ && !handle_.empty()) deleter_(ctx_, handle_);
		}
		Holder(const Holder&) = delete;
		Holder(Holder&& other) : ctx_(other.ctx_), deleter_(other.deleter_), handle_(other.handle_) {
			other.ctx_ = nullptr;
			other.deleter_ = nullptr;
			other.handle_ = HandleType{};
		}
		Holder& operator=(const Holder&) = delete;
		Holder& operator=(Holder&& other) {
			std::swap(ctx_, other.ctx_);
			std::swap(deleter_, other.deleter_);
			std::swap(handle_, other.handle_);
			return *this;
		}
		Holder& operator=(std::nullptr_t) {
			this->reset();
			return *this;
		}

		inline operator HandleType() const {
			return handle_;
		}

		bool valid() const {
			return handle_.valid();
		}

		bool empty() const {
			return handle_.empty();
		}

		void reset() {
			if (ctx_ && deleter_ && !handle_.empty()) deleter_(ctx_, handle_);
			ctx_ = nullptr;
			deleter_ = nullptr;
			handle_ = HandleType{};
		}

		HandleType release() {
			ctx_ = nullptr;
			deleter_ = nullptr;
			return std::exchange(handle_, HandleType{});
		}

		uint32_t gen() const {
			return handle_.gen();
		}
		uint32_t index() const {
			return handle_.index();
		}
		void* indexAsVoid() const {
			return handle_.indexAsVoid();
		}

	private:
		void* ctx_ = nullptr;
		void(*deleter_)(void*, HandleType) = nullptr;
		HandleType handle_ = {};
	};

	// =====================================================================
	// Thread-shard LEASE registry for lock-free pools.
	//
	// Multithread model: one local shard per thread. The main thread owns
	// shard 0 for the process lifetime. Every OTHER thread must hold a
	// UNIQUE shard lease: shards are claimed through a CAS bitmask, never
	// reused while leased, and released when the owning thread exits. The
	// old modulo counter (`fetch_add % kNumPoolShards`) could hand shard 0
	// (the main thread's) or a shard still owned by a LIVE worker of
	// another pool to a new thread — two threads mutating the same shard
	// with no lock anywhere is a silent data race, and sequential pools
	// (pool A destructed, pool B constructed) eventually wrapped the
	// counter into exactly that case.
	//
	// As long as live leases <= kNumPoolShards - 1 and each thread only
	// creates in its own shard, no locks are needed on the create/get/
	// destroy fast path. Cross-shard get() after the producing thread has
	// joined is safe (happens-before via std::future / thread join);
	// concurrent mutation of the SAME entry from two threads is still UB.
	//
	// Exhaustion (all shards leased): AcquirePoolShardLease returns
	// kNoShardLease. RegisterPoolShard's fallback is then shard 0 with a
	// LOUD warning — the documented "caller must serialize externally"
	// state, now visible instead of silent. ThreadPool never takes that
	// path: it pre-leases before spawning and runs fewer workers instead.
	// =====================================================================
	// Pool sharding constants live in mem::config (alloc_config.h) — the
	// single place to resize. Aliased here so existing pool.h call sites
	// (CurrentPoolShard, RegisterPoolShard, ShardedPool, ThreadPool caps)
	// keep working unchanged.
	static_assert(mem::config::kNumPoolShards >= 2
		&& mem::config::kNumPoolShards <= 32,
		"the lease mask is a 32-bit bitset over shards");
	inline constexpr uint32_t kNumPoolShards = mem::config::kNumPoolShards;
	inline constexpr uint32_t kShardCapacity = mem::config::kShardCapacity;
	inline constexpr uint32_t kShardedMaxSize = mem::config::kShardedMaxSize; // 8192, matches old Pool::kMaxSize

	// The bindless-budget contract (kShardedMaxSize <= smvk::limits::
	// MaxTextures) is asserted at the pool/descriptor INTEGRATION site
	// (Vulkan/detail/vulkan_impl.h, next to the bindless pools): the
	// generic memory pool must not know descriptor budgets.

	inline thread_local uint32_t t_poolShard = 0; // main thread == shard 0
	// Lease bitmask: bit S set == shard S is leased by a live thread.
	// Bit 0 is pre-set (the main thread) and is NEVER claimable — the
	// mask starts at 1, and the acquire candidate set masks bit 0 out.
	inline std::atomic<uint32_t> g_poolShardLeases{1u};

	// Sentinel: every lease-able shard is owned by a live thread.
	inline constexpr uint32_t kNoShardLease = 0xFFFFFFFFu;

	inline uint32_t CurrentPoolShard() noexcept { return t_poolShard; }
	inline void SetCurrentPoolShard(uint32_t shard) noexcept {
		assert(shard < kNumPoolShards);
		t_poolShard = shard < kNumPoolShards ? shard : 0;
	}

	// Claims a UNIQUE unclaimed shard in [1, kNumPoolShards) (lowest first,
	// so lease order is deterministic under no contention). Lock-free:
	// CAS on the lease mask. kNoShardLease when every shard is leased.
	inline uint32_t AcquirePoolShardLease() noexcept {
		constexpr uint32_t kAllMask = (1u << mem::config::kNumPoolShards) - 1u;
		uint32_t leased = g_poolShardLeases.load(std::memory_order_relaxed);
		for (;;) {
			const uint32_t candidates = ~leased & kAllMask & ~1u;
			if (candidates == 0)
				return kNoShardLease;
			const uint32_t bit = candidates & (~candidates + 1u); // lowest set
			const uint32_t shard = std::countr_zero(bit);
			const uint32_t claimed = leased | bit;
			if (g_poolShardLeases.compare_exchange_weak(leased, claimed,
					std::memory_order_acq_rel, std::memory_order_relaxed))
				return shard;
			// leased was reloaded by the CAS failure: retry with it.
		}
	}

	// Returns a shard to the lease pool (a thread that never leased, the
	// main thread's shard 0, or the sentinel are all no-ops).
	inline void ReleasePoolShardLease(uint32_t shard) noexcept {
		if (shard == 0 || shard == kNoShardLease
				|| shard >= kNumPoolShards)
			return;
		g_poolShardLeases.fetch_and(~(1u << shard), std::memory_order_acq_rel);
	}

	// Assigns a FRESH UNIQUE shard to the calling thread (one lease per
	// thread; release via ReleasePoolShardLease before the thread exits,
	// or let ThreadPool do it). Loud shard-0 fallback when every shard is
	// leased: the caller must then serialize pool access externally (the
	// pre-lease ThreadPool never lands here — it sizes itself down).
	inline uint32_t RegisterPoolShard() noexcept {
		uint32_t shard = AcquirePoolShardLease();
		if (shard == kNoShardLease) {
			std::fprintf(stderr,
				"[Pool] WARNING: all %u pool shards leased (>%u worker threads"
				" live); this thread falls back to SHARD 0 — serialize pool"
				" access externally or raise mem::config::kNumPoolShards\n",
				kNumPoolShards, kNumPoolShards - 1);
			shard = 0;
		}
		t_poolShard = shard;
		return shard;
	}

	// Single-threaded pool: NO internal locking. The owner thread (or any
	// externally synchronized context, e.g. Cluster on the main thread)
	// may use it directly. Shared Vulkan pools use ShardedPool below.
	template<typename ObjectType, typename ImplObjectType>
	class Pool {
		static constexpr uint32_t kListEndSentinel = 0xffffffff;
		static constexpr uint32_t kMaxSize = 8192;
		struct PoolEntry {
			PoolEntry() = default;
			explicit PoolEntry(ImplObjectType& obj) : obj_(std::move(obj)) {}
			ImplObjectType obj_ = {};
			uint32_t gen_ = 1;
			uint32_t nextFree_ = kListEndSentinel;
			bool occupied_ = false;
		};
		uint32_t freeListHead_ = kListEndSentinel;
		uint32_t numObjects_ = 0;
		uint32_t nextIndex_ = 0;
		uint32_t capacity_ = kMaxSize;

	public:
		PoolEntry objects_[kMaxSize] = {};
		Handle<ObjectType> create(ImplObjectType&& obj) {
			uint32_t idx = 0;
			if (freeListHead_ != kListEndSentinel) {
				idx = freeListHead_;
				freeListHead_ = objects_[idx].nextFree_;
				objects_[idx].obj_ = std::move(obj);
			}
			else {
				assert(nextIndex_ < kMaxSize && "Pool overflow – increase MAX_SIZE");
				if (nextIndex_ >= kMaxSize) return {};
				idx = nextIndex_++;
				objects_[idx].obj_ = std::move(obj);
				objects_[idx].gen_ = 1;
				objects_[idx].nextFree_ = kListEndSentinel;
			}
			objects_[idx].occupied_ = true;
			numObjects_++;
			return Handle<ObjectType>(idx, objects_[idx].gen_);
		}
		void destroy(Handle<ObjectType> handle) {
			if (handle.empty())
				return;
			assert(numObjects_ > 0);
			const uint32_t index = handle.index();
			assert(index < kMaxSize);
			assert(index < nextIndex_ || freeListHead_ != kListEndSentinel);
			assert(handle.gen() == objects_[index].gen_);
			objects_[index].obj_ = ImplObjectType{};
			objects_[index].gen_++;
			if (objects_[index].gen_ == 0) objects_[index].gen_ = 1;
			objects_[index].nextFree_ = freeListHead_;
			objects_[index].occupied_ = false;
			freeListHead_ = index;
			numObjects_--;
		}
		const ImplObjectType* get(Handle<ObjectType> handle) const {
			if (handle.empty()) return nullptr;
			const uint32_t index = handle.index();
			assert(index < kMaxSize);
			assert(handle.gen() == objects_[index].gen_);
			return &objects_[index].obj_;
		}
		ImplObjectType* get(Handle<ObjectType> handle) {
			if (handle.empty()) return nullptr;
			const uint32_t index = handle.index();
			assert(index < kMaxSize);
			assert(handle.gen() == objects_[index].gen_);
			return &objects_[index].obj_;
		}
		Handle<ObjectType> getHandle(uint32_t index) const {
			assert(index < kMaxSize);
			if (index >= kMaxSize) return {};
			return Handle<ObjectType>(index, objects_[index].gen_);
		}
		Handle<ObjectType> findObject(const ImplObjectType* obj) {
			if (!obj) return {};
			for (uint32_t idx = 0; idx < nextIndex_; ++idx) {
				if (objects_[idx].occupied_ && objects_[idx].obj_ == *obj) {
					return Handle<ObjectType>(idx, objects_[idx].gen_);
				}
			}
			for (uint32_t idx = nextIndex_; idx < kMaxSize; ++idx) {
				if (objects_[idx].occupied_ && objects_[idx].obj_ == *obj) {
					return Handle<ObjectType>(idx, objects_[idx].gen_);
				}
			}
			return {};
		}
		void clear() {
			for (uint32_t i = 0; i < nextIndex_; ++i) {
				objects_[i] = PoolEntry{};
				objects_[i].gen_ = 1;
				objects_[i].nextFree_ = kListEndSentinel;
				objects_[i].occupied_ = false;
			}
			freeListHead_ = kListEndSentinel;
			numObjects_ = 0;
			nextIndex_ = 0;
		}
		uint32_t numObjects() const {
			return numObjects_;
		}

		template<typename Func>
		void forEach(Func&& func)
		{
			for (uint32_t i = 0; i < kMaxSize; ++i) {
				if (objects_[i].occupied_) func(objects_[i].obj_);
			}
		}

		template<typename Func>
		void forEachHandle(Func&& func)
		{
			for (uint32_t i = 0; i < kMaxSize; ++i) {
				if (objects_[i].occupied_) {
					func(Handle<ObjectType>(i, objects_[i].gen_));
				}
			}
		}

	};

	// Lock-free sharded pool for resources shared across threads.
	// Global handle index = shard * kShardCapacity + localIndex, so every
	// handle is globally unique and routes to its owning shard without a
	// lookup. create() allocates in the CALLING thread's shard (see
	// RegisterPoolShard); get()/destroy() decode the shard from the handle.
	// No mutexes on any path. forEach/clear/numObjects/findObject iterate
	// all shards and must only run when no other thread is mutating
	// (teardown / render-thread update after worker join).
	template<typename ObjectType, typename ImplObjectType>
	class ShardedPool {
		static constexpr uint32_t kListEndSentinel = 0xffffffff;
		struct ShardEntry {
			ImplObjectType obj_ = {};
			uint32_t gen_ = 1;
			uint32_t nextFree_ = kListEndSentinel;
			bool occupied_ = false;
		};
		struct Shard {
			ShardEntry objects_[kShardCapacity] = {};
			uint32_t freeListHead_ = kListEndSentinel;
			uint32_t numObjects_ = 0;
			uint32_t nextIndex_ = 0;
		};
		Shard shards_[kNumPoolShards];

		static uint32_t shardOf(uint32_t globalIndex) noexcept { return globalIndex / kShardCapacity; }
		static uint32_t localOf(uint32_t globalIndex) noexcept { return globalIndex % kShardCapacity; }
		static uint32_t globalOf(uint32_t shard, uint32_t local) noexcept { return shard * kShardCapacity + local; }

	public:
		static constexpr uint32_t kMaxSize = kShardedMaxSize;

		Handle<ObjectType> create(ImplObjectType&& obj) {
			uint32_t shard = CurrentPoolShard();
			assert(shard < kNumPoolShards);
			if (shard >= kNumPoolShards) shard = 0;
			Shard& s = shards_[shard];
			uint32_t local = 0;
			if (s.freeListHead_ != kListEndSentinel) {
				local = s.freeListHead_;
				s.freeListHead_ = s.objects_[local].nextFree_;
				s.objects_[local].obj_ = std::move(obj);
			}
			else {
				assert(s.nextIndex_ < kShardCapacity && "Shard overflow – too many objects on one thread; rebalance or raise kShardCapacity");
				if (s.nextIndex_ >= kShardCapacity) return {};
				local = s.nextIndex_++;
				s.objects_[local].obj_ = std::move(obj);
				s.objects_[local].gen_ = 1;
				s.objects_[local].nextFree_ = kListEndSentinel;
			}
			s.objects_[local].occupied_ = true;
			s.numObjects_++;
			return Handle<ObjectType>(globalOf(shard, local), s.objects_[local].gen_);
		}
		void destroy(Handle<ObjectType> handle) {
			if (handle.empty()) return;
			uint32_t global = handle.index();
			assert(global < kMaxSize);
			uint32_t shard = shardOf(global);
			uint32_t local = localOf(global);
			assert(shard < kNumPoolShards && local < kShardCapacity);
			Shard& s = shards_[shard];
			assert(s.numObjects_ > 0);
			assert(handle.gen() == s.objects_[local].gen_);
			s.objects_[local].obj_ = ImplObjectType{};
			s.objects_[local].gen_++;
			if (s.objects_[local].gen_ == 0) s.objects_[local].gen_ = 1;
			s.objects_[local].nextFree_ = s.freeListHead_;
			s.objects_[local].occupied_ = false;
			s.freeListHead_ = local;
			s.numObjects_--;
		}
		const ImplObjectType* get(Handle<ObjectType> handle) const {
			if (handle.empty()) return nullptr;
			uint32_t global = handle.index();
			assert(global < kMaxSize);
			uint32_t shard = shardOf(global);
			uint32_t local = localOf(global);
			assert(shard < kNumPoolShards && local < kShardCapacity);
			const Shard& s = shards_[shard];
			assert(handle.gen() == s.objects_[local].gen_);
			return &s.objects_[local].obj_;
		}
		ImplObjectType* get(Handle<ObjectType> handle) {
			if (handle.empty()) return nullptr;
			uint32_t global = handle.index();
			assert(global < kMaxSize);
			uint32_t shard = shardOf(global);
			uint32_t local = localOf(global);
			assert(shard < kNumPoolShards && local < kShardCapacity);
			Shard& s = shards_[shard];
			assert(handle.gen() == s.objects_[local].gen_);
			return &s.objects_[local].obj_;
		}
		Handle<ObjectType> getHandle(uint32_t globalIndex) const {
			assert(globalIndex < kMaxSize);
			if (globalIndex >= kMaxSize) return {};
			uint32_t shard = shardOf(globalIndex);
			uint32_t local = localOf(globalIndex);
			return Handle<ObjectType>(globalIndex, shards_[shard].objects_[local].gen_);
		}
		Handle<ObjectType> findObject(const ImplObjectType* obj) {
			if (!obj) return {};
			for (uint32_t shard = 0; shard < kNumPoolShards; ++shard) {
				Shard& s = shards_[shard];
				for (uint32_t local = 0; local < s.nextIndex_; ++local) {
					if (s.objects_[local].occupied_ && s.objects_[local].obj_ == *obj) {
						return Handle<ObjectType>(globalOf(shard, local), s.objects_[local].gen_);
					}
				}
			}
			return {};
		}
		void clear() {
			for (uint32_t shard = 0; shard < kNumPoolShards; ++shard) {
				Shard& s = shards_[shard];
				for (uint32_t i = 0; i < s.nextIndex_; ++i) {
					s.objects_[i] = ShardEntry{};
					s.objects_[i].gen_ = 1;
					s.objects_[i].nextFree_ = kListEndSentinel;
					s.objects_[i].occupied_ = false;
				}
				s.freeListHead_ = kListEndSentinel;
				s.numObjects_ = 0;
				s.nextIndex_ = 0;
			}
		}
		uint32_t numObjects() const {
			uint32_t n = 0;
			for (uint32_t shard = 0; shard < kNumPoolShards; ++shard) n += shards_[shard].numObjects_;
			return n;
		}
		template<typename Func>
		void forEach(Func&& func) {
			for (uint32_t shard = 0; shard < kNumPoolShards; ++shard) {
				Shard& s = shards_[shard];
				for (uint32_t i = 0; i < kShardCapacity; ++i) {
					if (s.objects_[i].occupied_) func(s.objects_[i].obj_);
				}
			}
		}
		template<typename Func>
		void forEachHandle(Func&& func) {
			for (uint32_t shard = 0; shard < kNumPoolShards; ++shard) {
				Shard& s = shards_[shard];
				for (uint32_t i = 0; i < kShardCapacity; ++i) {
					if (s.objects_[i].occupied_) {
						func(Handle<ObjectType>(globalOf(shard, i), s.objects_[i].gen_));
					}
				}
			}
		}
	};

}; // namespace smvk
