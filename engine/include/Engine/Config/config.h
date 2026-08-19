#pragma once

// ============================================================================
// smvk engine compile-time feature toggles (KISS).
//
// These control whether certain permanent-lifetime resource heaps are
// allocated on the VulkanResourceManager. To disable a feature, define the
// corresponding DISABLE_* macro before the engine headers are included, e.g.
// on the compiler / command line:
//
//     -DDISABLE_STATIC_VERTEX_HEAPS
//
// or via CMake:
//
//     target_compile_definitions(engine PRIVATE DISABLE_STATIC_VERTEX_HEAPS)
//
// Disabled heaps are simply never allocated (the manager still holds empty
// handles and frees them safely as no-ops at shutdown), which is the cheapest
// way to drop the large static vertex/index geometry heaps for apps that do
// not use them.
//
// Defaults: everything ENABLED.
// ============================================================================

#ifdef DISABLE_STATIC_VERTEX_HEAPS
#define SMVK_STATIC_VERTEX_HEAPS 0
#else
#define SMVK_STATIC_VERTEX_HEAPS 1
#endif

// Reserved for future skinned-mesh static heaps.
#ifdef DISABLE_SKINNED_VERTEX_HEAPS
#define SMVK_SKINNED_VERTEX_HEAPS 0
#else
#define SMVK_SKINNED_VERTEX_HEAPS 1
#endif
