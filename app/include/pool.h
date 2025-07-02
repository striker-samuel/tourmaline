
#pragma once
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

template<typename T>
struct Handle {
	uint32_t index = UINT32_MAX;
	uint32_t generation = 0;

	bool valid() const { return index != UINT32_MAX; }

	bool operator==(const Handle& other) const {
		return index == other.index && generation == other.generation;
	}

	bool operator!=(const Handle& other) const {
		return !(*this == other);
	}
};

template<typename T, size_t MaxElements, size_t BlockSize>
class VirtualPool {
public:
	VirtualPool() {
		// Ensure block is large enough
		static_assert(BlockSize >= sizeof(T), "BlockSize must fit T");

		pageSize_ = getPageSize();
		totalBytes_ = MaxElements * BlockSize;

#if defined(_WIN32)
		base_ = VirtualAlloc(nullptr, totalBytes_, MEM_RESERVE, PAGE_NOACCESS);
		assert(base_ && "Failed to reserve virtual memory");
#else
		base_ = mmap(nullptr, totalBytes_, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		assert(base_ != MAP_FAILED && "Failed to reserve virtual memory");
#endif

		occupied_ = static_cast<bool*>   (std::malloc(MaxElements * sizeof(bool)));
		generation_ = static_cast<uint32_t*>(std::malloc(MaxElements * sizeof(uint32_t)));
		assert(occupied_ && generation_);

		std::fill(occupied_, occupied_ + MaxElements, false);
		std::fill(generation_, generation_ + MaxElements, 1);
	}

	~VirtualPool() {
		// Destruct live objects
		for (size_t i = 0; i < MaxElements; ++i) {
			if (occupied_[i]) {
				getPtr(i)->~T();
			}
		}

#if defined(_WIN32)
		VirtualFree(base_, 0, MEM_RELEASE);
#else
		munmap(base_, totalBytes_);
#endif

		std::free(occupied_);
		std::free(generation_);

	}

	Handle<T> create(const T& value) {
		for (size_t i = 0; i < MaxElements; ++i) {
			if (!occupied_[i]) {
				commitIfNeeded(i);
				void* slot = getRawPtr(i);
				new (slot) T(value);
				occupied_[i] = true;
				return Handle<T>{ static_cast<uint32_t>(i), generation_[i] };
			}
		}
		return {};
	}

	void destroy(Handle<T> handle) {
		if (!isValid(handle)) return;
		size_t i = handle.index;
		getPtr(i)->~T();
		occupied_[i] = false;
		generation_[i]++;
	}

	T* get(Handle<T> handle) {
		if (!isValid(handle)) return nullptr;
		return getPtr(handle.index);
	}

	const T* get(Handle<T> handle) const {
		if (!isValid(handle)) return nullptr;
		return getPtr(handle.index);
	}

private:
	void* base_;
	size_t totalBytes_;
	size_t pageSize_;
	bool* occupied_;
	uint32_t* generation_;

	T* getPtr(size_t index) {
		return reinterpret_cast<T*>(getRawPtr(index));
	}

	const T* getPtr(size_t index) const {
		return reinterpret_cast<const T*>(getRawPtr(index));
	}

	void* getRawPtr(size_t index) const {
		return static_cast<char*>(base_) + index * BlockSize;
	}

	bool isValid(const Handle<T>& handle) const {
		return handle.index < MaxElements &&
			occupied_[handle.index] &&
			generation_[handle.index] == handle.generation;
	}

	void commitIfNeeded(size_t index) {
		void* addr = getRawPtr(index);
#if defined(_WIN32)
		// Commit individual block if needed
		VirtualAlloc(addr, BlockSize, MEM_COMMIT, PAGE_READWRITE);
#else
		// POSIX systems lazily commit on access, or use mprotect
		mprotect(addr, BlockSize, PROT_READ | PROT_WRITE);
#endif
	}

	static size_t getPageSize() {
#if defined(_WIN32)
		SYSTEM_INFO info;
		GetSystemInfo(&info);
		return info.dwPageSize;
#else
		return sysconf(_SC_PAGESIZE);
#endif
	}
};


struct Enemy {
	int health;
	Enemy(int h) : health(h) { std::cout << "Enemy constructed\n"; }
	~Enemy() { std::cout << "Enemy destroyed\n"; }
};

