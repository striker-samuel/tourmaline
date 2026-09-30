// Global linear allocator instances (see memory.h).

#include <Engine/Memory/memory.h>

#include <Engine/Memory/alloc_config.h>

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
	// Function-local statics destroy themselves at exit in reverse order;
	// nothing to do. Kept as a named teardown hook for symmetry and for
	// future leak-check accounting.
}

} // namespace smvk::mem
