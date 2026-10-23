// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Global linear allocator instances (see memory.h).

#include <Engine/Memory/memory.h>

#include <Engine/Memory/alloc_config.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace smvk::mem {

namespace {

// Program-entry size override: `<name>` in TOURMALINE_MEM_*, a positive
// integer in `unit` bytes (MB/KB/slots). Bad values warn and keep the
// compile-time default — same fallback policy as RunConfig pins.
size_t EnvSize(const char* name, size_t unit, size_t fallback) {
	const char* raw = std::getenv(name);
	if (!raw || !*raw)
		return fallback;
	errno = 0;
	char* end = nullptr;
	const unsigned long long n = std::strtoull(raw, &end, 0);
	while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
		++end;
	const bool bad = errno != 0 || end == raw || (end && *end != '\0')
		|| n == 0 || n > (std::numeric_limits<size_t>::max)() / unit;
	if (bad) {
		std::fprintf(stderr,
			"[Memory] bad %s='%s' (want a positive integer in %s); "
			"using default %zu\n",
			name, raw, unit == 1 ? "slots" : (unit == 1024 ? "KiB" : "MiB"),
			fallback);
		std::fflush(stderr);
		return fallback;
	}
	const size_t bytes = static_cast<size_t>(n) * unit;
	std::fprintf(stderr, "[Memory] %s=%llu -> %zu bytes\n", name, n, bytes);
	return bytes;
}

config::RuntimeConfig ParseRuntimeConfig() {
	using namespace config;
	RuntimeConfig c;
	c.appArenaBlock = EnvSize("TOURMALINE_MEM_APP_ARENA_MB", kMiB, c.appArenaBlock);
	c.frameArenaBlock = EnvSize("TOURMALINE_MEM_FRAME_ARENA_MB", kMiB, c.frameArenaBlock);
	c.processReserve = EnvSize("TOURMALINE_MEM_PROCESS_MB", kMiB, c.processReserve);
	c.frameReserve = EnvSize("TOURMALINE_MEM_FRAME_MB", kMiB, c.frameReserve);
	c.transientReserve = EnvSize("TOURMALINE_MEM_TRANSIENT_MB", kMiB, c.transientReserve);
	c.streamerTotalBudget = EnvSize("TOURMALINE_MEM_STREAMER_MB", kMiB, c.streamerTotalBudget);
	c.stagingRingPerThread = EnvSize("TOURMALINE_MEM_STAGING_RING_MB", kMiB, c.stagingRingPerThread);
	c.bulkRingCapacity = EnvSize("TOURMALINE_MEM_BULK_RING_MB", kMiB, c.bulkRingCapacity);
	c.uploadFlushThreshold = EnvSize("TOURMALINE_MEM_FLUSH_MB", kMiB, c.uploadFlushThreshold);
	c.readbackStagingMin = EnvSize("TOURMALINE_MEM_READBACK_MIN_KB", kKiB, c.readbackStagingMin);
	c.readbackStagingMax = EnvSize("TOURMALINE_MEM_READBACK_MAX_MB", kMiB, c.readbackStagingMax);
	c.threadPoolQueueSlots = EnvSize("TOURMALINE_MEM_THREAD_SLOTS", 1, c.threadPoolQueueSlots);
	c.commitQuantum = EnvSize("TOURMALINE_MEM_COMMIT_MB", kMiB, c.commitQuantum);
	c.staticVertexHeap = EnvSize("TOURMALINE_MEM_STATIC_VERTEX_MB", kMiB, c.staticVertexHeap);
	c.staticIndexHeap = EnvSize("TOURMALINE_MEM_STATIC_INDEX_MB", kMiB, c.staticIndexHeap);
	c.targetHeapBlock = EnvSize("TOURMALINE_MEM_TARGET_BLOCK_MB", kMiB, c.targetHeapBlock);

	// Validation: CommitUpTo uses a mask-based align, so the quantum must be
	// a power of two >= one OS page.
	const bool quantumOk = c.commitQuantum >= 4096
		&& (c.commitQuantum & (c.commitQuantum - 1)) == 0;
	if (!quantumOk) {
		std::fprintf(stderr,
			"[Memory] TOURMALINE_MEM_COMMIT_MB must be a power-of-two "
			"multiple of 4 KiB; using default %zu\n", kCommitQuantum);
		c.commitQuantum = kCommitQuantum;
	}
	// Cross-field invariants (the same ordering the config test pins).
	if (c.readbackStagingMax < c.readbackStagingMin) {
		std::fprintf(stderr,
			"[Memory] READBACK_MAX below READBACK_MIN; clamping max up\n");
		c.readbackStagingMax = c.readbackStagingMin;
	}
	if (c.streamerTotalBudget < c.stagingRingPerThread) {
		std::fprintf(stderr,
			"[Memory] STREAMER budget below STAGING_RING; clamping budget up\n");
		c.streamerTotalBudget = c.stagingRingPerThread;
	}
	if (c.uploadFlushThreshold > c.bulkRingCapacity) {
		std::fprintf(stderr,
			"[Memory] FLUSH threshold above BULK_RING; clamping flush down\n");
		c.uploadFlushThreshold = c.bulkRingCapacity;
	}
	return c;
}

} // namespace

const config::RuntimeConfig& config::Runtime() {
	// Function-local static: parsed exactly once, before the first
	// allocator/heap/ring is constructed (InitMemory is the documented
	// first call in main). Callers hold a const ref for process lifetime.
	static const RuntimeConfig cfg = ParseRuntimeConfig();
	return cfg;
}

LinearAllocator& ProcessAllocator() {
	static LinearAllocator alloc("process", config::Runtime().processReserve,
		config::Runtime().commitQuantum);
	return alloc;
}

LinearAllocator& FrameAllocator() {
	static LinearAllocator alloc("frame", config::Runtime().frameReserve,
		config::Runtime().commitQuantum);
	return alloc;
}

LinearAllocator& TransientAllocator() {
	static LinearAllocator alloc("transient", config::Runtime().transientReserve,
		config::Runtime().commitQuantum);
	return alloc;
}

void InitMemory() {
	// Reserve virtual address space up front so failure happens here —
	// before Vulkan, workers, or asset loading — with the OOM diagnostic.
	ProcessAllocator().EnsureInitialized();
	FrameAllocator().EnsureInitialized();
	TransientAllocator().EnsureInitialized();
}

void ShutdownMemory() {
	// Footprint report: the whole migration's memory cost in three lines.
	// Function-local statics destroy themselves at exit in reverse order;
	// nothing else to do. Kept as a named teardown hook for symmetry.
	DumpStats(stderr);
}

namespace {

void DumpOne(std::FILE* out, const LinearAllocator& alloc) {
	const LinearStats s = alloc.GetStats();
	std::string_view name = alloc.Name();
	std::fprintf(out,
		"[Memory] %.*s: used %.1f MB / committed %.1f MB / reserved %.1f MB\n",
		static_cast<int>(name.size()), name.data(),
		static_cast<double>(s.used) / (1024.0 * 1024.0),
		static_cast<double>(s.committed) / (1024.0 * 1024.0),
		static_cast<double>(s.reserved) / (1024.0 * 1024.0));
}

} // namespace

void DumpStats(std::FILE* out) {
	if (!out)
		return;
	DumpOne(out, ProcessAllocator());
	DumpOne(out, FrameAllocator());
	DumpOne(out, TransientAllocator());
	std::fflush(out);
}

} // namespace smvk::mem
