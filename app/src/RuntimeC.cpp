// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#include "RuntimeC.h"
#include "ProjectRuntime.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct tour_c_runtime {
    explicit tour_c_runtime(app::ProjectRuntime&& session)
        : session(std::move(session)) {}
    app::ProjectRuntime session;
};

namespace {

void SetError(const std::string& message, char* buffer, size_t capacity) noexcept {
    if (!buffer || capacity == 0) return;
    const size_t n = std::min(message.size(), capacity - 1);
    std::memcpy(buffer, message.data(), n);
    buffer[n] = '\0';
}

void ClearError(char* buffer, size_t capacity) noexcept {
    if (buffer && capacity) buffer[0] = '\0';
}

} // namespace

extern "C" {

tour_c_runtime* tour_c_runtime_open(const char* tapp_path,
    char* error, size_t error_capacity) {
    ClearError(error, error_capacity);
    if (!tapp_path || !*tapp_path) {
        SetError("a non-empty .tapp path is required", error, error_capacity);
        return nullptr;
    }
    try {
        std::string diagnostic;
        auto result = app::ProjectRuntime::Load(tapp_path, diagnostic);
        if (!result) {
            SetError(diagnostic, error, error_capacity);
            return nullptr;
        }
        return new tour_c_runtime(std::move(*result));
    } catch (const std::exception& e) {
        SetError(e.what(), error, error_capacity);
    } catch (...) {
        SetError("unknown exception opening project", error, error_capacity);
    }
    return nullptr;
}

void tour_c_runtime_close(tour_c_runtime* runtime) {
    delete runtime;
}

int tour_c_runtime_attach_catalog(tour_c_runtime* runtime,
    const tour_c_catalog* catalog, char* error, size_t error_capacity) {
    ClearError(error, error_capacity);
    if (!runtime || !catalog) {
        SetError("runtime and catalog are required", error, error_capacity);
        return 0;
    }
    if (catalog->abi_version != TOUR_C_CATALOG_ABI_VERSION) {
        SetError("unsupported C catalog ABI version", error, error_capacity);
        return 0;
    }
    if (catalog->asset_count && !catalog->assets) {
        SetError("non-empty catalog has no asset records", error, error_capacity);
        return 0;
    }
    try {
        std::vector<app::ProjectRuntime::CompiledAssetDesc> records;
        records.reserve(catalog->asset_count);
        for (size_t i = 0; i < catalog->asset_count; ++i) {
            const tour_c_asset_desc& a = catalog->assets[i];
            if (!a.path) {
                SetError("catalog asset has a null path", error, error_capacity);
                return 0;
            }
            records.push_back({
                a.id, a.kind, std::string_view(a.path),
                std::string_view(a.scene_name ? a.scene_name : "")
            });
        }
        std::string diagnostic;
        if (!runtime->session.AttachCompiledCatalog(records.data(),
                records.size(), catalog->fingerprint, diagnostic)) {
            SetError(diagnostic, error, error_capacity);
            return 0;
        }
        return 1;
    } catch (const std::exception& e) {
        SetError(e.what(), error, error_capacity);
    } catch (...) {
        SetError("unknown exception validating catalog", error, error_capacity);
    }
    return 0;
}

int tour_c_runtime_read_asset(tour_c_runtime* runtime, uint64_t asset_id,
    tour_c_bytes* out_bytes, char* error, size_t error_capacity) {
    ClearError(error, error_capacity);
    if (out_bytes) *out_bytes = {};
    if (!runtime || !out_bytes) {
        SetError("runtime and output bytes are required", error, error_capacity);
        return 0;
    }
    try {
        std::string diagnostic;
        auto bytes = runtime->session.ReadAsset(asset_id, diagnostic);
        if (!bytes) {
            SetError(diagnostic, error, error_capacity);
            return 0;
        }
        if (!bytes->empty()) {
            void* memory = std::malloc(bytes->size());
            if (!memory) {
                SetError("out of memory reading asset", error, error_capacity);
                return 0;
            }
            std::memcpy(memory, bytes->data(), bytes->size());
            out_bytes->data = memory;
        }
        out_bytes->size = bytes->size();
        return 1;
    } catch (const std::exception& e) {
        SetError(e.what(), error, error_capacity);
    } catch (...) {
        SetError("unknown exception reading asset", error, error_capacity);
    }
    return 0;
}

void tour_c_bytes_free(tour_c_bytes* bytes) {
    if (!bytes) return;
    std::free(bytes->data);
    *bytes = {};
}

} // extern "C"
