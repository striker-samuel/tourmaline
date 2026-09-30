// Global linear allocator instances (see memory.h).

#include <Engine/Memory/memory.h>

#include <Engine/Memory/alloc_config.h>

#include <cstdio>

namespace smvk::mem {

LinearAllocator& ProcessAllocator() {
	static LinearAllocator alloc("process", config::kProcessReserve);
	return alloc;
}

LinearAllocator& FrameAllocator() {
	static LinearAllocator alloc("frame", config::kFrameReserve);
	return alloc;
}

LinearAllocator& TransientAllocator() {
	static LinearAllocator alloc("transient", config::kTransientReserve);
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
