// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Global linear allocator instances (see memory.h).

#include <Engine/Memory/memory.h>

#include <Engine/Memory/alloc_config.h>
#include <Engine/Memory/arena.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace tour::mem {

namespace {

// The CLI-override slot (alloc_config.h's MemCliOverrides): written once
// by the host's arg parsing, BEFORE the first Runtime() call (RunPakApp
// applies it directly above InitMemory); read exactly once by
// ParseRuntimeConfig below. Single-threaded by contract - no locking.
config::MemCliOverrides g_memCliOverrides;

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
	c.flightReserve = EnvSize("TOURMALINE_MEM_FLIGHT_MB", kMiB, c.flightReserve);
	c.transientReserve = EnvSize("TOURMALINE_MEM_TRANSIENT_MB", kMiB, c.transientReserve);
	c.streamerTotalBudget = EnvSize("TOURMALINE_MEM_STREAMER_MB", kMiB, c.streamerTotalBudget);
	c.stagingRingPerThread = EnvSize("TOURMALINE_MEM_STAGING_RING_MB", kMiB, c.stagingRingPerThread);
	c.bulkRingCapacity = EnvSize("TOURMALINE_MEM_BULK_RING_MB", kMiB, c.bulkRingCapacity);
	c.uploadFlushThreshold = EnvSize("TOURMALINE_MEM_FLUSH_MB", kMiB, c.uploadFlushThreshold);
	c.readbackStagingMin = EnvSize("TOURMALINE_MEM_READBACK_MIN_KB", kKiB, c.readbackStagingMin);
	c.readbackStagingMax = EnvSize("TOURMALINE_MEM_READBACK_MAX_MB", kMiB, c.readbackStagingMax);
	c.threadPoolQueueSlots = EnvSize("TOURMALINE_MEM_THREAD_SLOTS", 1, c.threadPoolQueueSlots);
	// The ring allocation multiplies the slot count by the slot size in
	// ThreadPool's constructor; a count beyond the ceiling either wraps
	// that multiplication (out-of-bounds ring writes) or reserves
	// gigabytes for a queue nothing needs. Reject to the default —
	// same bad-value policy as EnvSize itself.
	if (c.threadPoolQueueSlots > kMaxThreadPoolQueueSlots) {
		std::fprintf(stderr,
			"[Memory] TOURMALINE_MEM_THREAD_SLOTS=%zu above the ceiling"
			" %zu; using default %zu\n",
			c.threadPoolQueueSlots, kMaxThreadPoolQueueSlots,
			size_t(kThreadPoolQueueSlots));
		c.threadPoolQueueSlots = kThreadPoolQueueSlots;
	}
	c.commitQuantum = EnvSize("TOURMALINE_MEM_COMMIT_MB", kMiB, c.commitQuantum);
	c.staticVertexHeap = EnvSize("TOURMALINE_MEM_STATIC_VERTEX_MB", kMiB, c.staticVertexHeap);
	c.staticIndexHeap = EnvSize("TOURMALINE_MEM_STATIC_INDEX_MB", kMiB, c.staticIndexHeap);
	c.staticLightmapHeap = EnvSize("TOURMALINE_MEM_STATIC_LIGHTMAP_MB", kMiB, c.staticLightmapHeap);
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
	// CLI overrides (the App's --mem-* flags; alloc_config.h's contract):
	// applied AFTER the env pass + its clamps so an explicit launch flag
	// beats the ambient environment. The reserves carry no cross-field
	// invariants beyond positivity - the same bad-value policy the host's
	// arg parse already enforces (1 MiB..1 TiB).
	if (g_memCliOverrides.processReserveMb) {
		c.processReserve = g_memCliOverrides.processReserveMb * kMiB;
		std::fprintf(stderr,
			"[Memory] cli --mem-process-mb=%zu -> %zu bytes\n",
			g_memCliOverrides.processReserveMb, c.processReserve);
	}
	if (g_memCliOverrides.frameReserveMb) {
		c.frameReserve = g_memCliOverrides.frameReserveMb * kMiB;
		std::fprintf(stderr,
			"[Memory] cli --mem-frame-mb=%zu -> %zu bytes\n",
			g_memCliOverrides.frameReserveMb, c.frameReserve);
	}
	if (g_memCliOverrides.transientReserveMb) {
		c.transientReserve = g_memCliOverrides.transientReserveMb * kMiB;
		std::fprintf(stderr,
			"[Memory] cli --mem-transient-mb=%zu -> %zu bytes\n",
			g_memCliOverrides.transientReserveMb, c.transientReserve);
	}
	return c;
}

} // namespace

void config::ApplyMemCliOverrides(const config::MemCliOverrides& o) {
	g_memCliOverrides = o;
}

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
	const auto& arena = smvk::FrameArena();
	const auto& backing = FrameAllocator();
	std::fprintf(stderr,
		"[Memory] FrameArena lifetime: allocations=%llu, requested=%llu bytes (%.2f MiB), "
		"peak requested in a frame=%zu bytes (%.2f MiB)\\n",
		static_cast<unsigned long long>(arena.AllocationCount()),
		static_cast<unsigned long long>(arena.TotalRequestedBytes()),
		static_cast<double>(arena.TotalRequestedBytes()) / (1024.0 * 1024.0),
		arena.PeakUsedBytes(),
		static_cast<double>(arena.PeakUsedBytes()) / (1024.0 * 1024.0));
	std::fprintf(stderr,
		"[Memory] FrameAllocator lifetime: allocations=%llu, requested=%llu bytes (%.2f MiB), "
		"max offset=%zu bytes (%.2f MiB), offset at shutdown=%zu bytes\\n",
		static_cast<unsigned long long>(backing.AllocationCount()),
		static_cast<unsigned long long>(backing.TotalRequested()),
		static_cast<double>(backing.TotalRequested()) / (1024.0 * 1024.0),
		backing.PeakUsed(),
		static_cast<double>(backing.PeakUsed()) / (1024.0 * 1024.0),
		backing.Used());
	std::fflush(stderr);
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

} // namespace tour::mem
