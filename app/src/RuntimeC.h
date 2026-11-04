// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0
#pragma once

// First C game-runtime seam: a content session, the compiled-catalog gate,
// and identity-based reads. The implementation remains C++ (ProjectRuntime).
// This header compiles as C11; no exceptions, STL, or C++ runtime types.

#include <Engine/Asset/catalog_c.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tour_c_runtime tour_c_runtime;

typedef struct tour_c_bytes {
    void* data;
    size_t size;
} tour_c_bytes;

// Open a packaged .tapp; the returned session owns the content registry.
// On failure return NULL, copying a NUL-terminated diagnostic to error when
// error_capacity > 0. All calls are synchronous; no GPU or window is created.
tour_c_runtime* tour_c_runtime_open(const char* tapp_path,
    char* error, size_t error_capacity);
void tour_c_runtime_close(tour_c_runtime* runtime);

// Use the SAME row-by-row + fingerprint validation as C++ RunGame.
// The generator emits the tour_c_catalog constant in project_c.h.
// Return 1 on success, 0 on refusal. No C++ layouts cross this boundary.
int tour_c_runtime_attach_catalog(tour_c_runtime* runtime,
    const tour_c_catalog* catalog, char* error, size_t error_capacity);

// Read a catalog asset by its stable id through the effective content
// provider (including overrides). The returned buffer is owned by the
// runtime API; always release with tour_c_bytes_free. Empty is valid.
// Return 1 on success and 0 on failure. out_bytes is reset on entry.
int tour_c_runtime_read_asset(tour_c_runtime* runtime, uint64_t asset_id,
    tour_c_bytes* out_bytes, char* error, size_t error_capacity);
void tour_c_bytes_free(tour_c_bytes* bytes);

#ifdef __cplusplus
}
#endif
