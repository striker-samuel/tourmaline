// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Compilation test for the PRODUCTION RHI adapter (descriptor_table_rhi.h).
// The mirror-only test never includes ResourceManager/Holder/UploadDataToBuffer,
// so a typo in the adapter's RHI types, the destination stage mask, or the
// return-state shape would not be caught. This instantiates the full
// DescriptorStoragePublisher against the real RHI types WITHOUT running a GPU
// (no device, no upload, no descriptor set).
#include <Engine/RHI/descriptor_table_rhi.h>
#include <cstdint>
#include <cstdio>
#include <type_traits>

struct Payload {
    uint32_t x = 0, y = 0;
};

int main() {
    using Publisher = smvk::table::DescriptorStoragePublisher<Payload, 2>;
    // Publish returns the explicit queued/failed state, not a bare bool.
    static_assert(std::is_enum_v<Publisher::State>,
                  "publisher state must be the queued/failed enum");
    // GetBinding returns the descriptor-input shape (buffer + capacity + rows).
    static_assert(std::is_trivially_copyable_v<Publisher::Binding>,
                  "binding must stay a plain value type");
    static_assert(std::is_same_v<decltype(Publisher::Binding::buffer), smvk::BufferHandle>,
                  "binding must carry the raw BufferHandle");
    std::printf("descriptor-rhi compilation checks complete\n");
    return 0;
}
