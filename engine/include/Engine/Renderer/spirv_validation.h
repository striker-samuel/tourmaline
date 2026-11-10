// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace smvk {

// Lightweight structural check for loaded, potentially corrupted binaries.
// This is NOT SPIR-V semantic validation. Build-time spirv-val checks the
// Vulkan environment and ID/type/layout rules; this gate runs on every
// shader-module creation, including hot-reloaded or externally loaded files.
inline bool CheckSpirvStructure(std::span<const char> code, std::string* error = nullptr) {
    const auto fail = [&](const char* why) {
        if (error)
            *error = why;
        return false;
    };
    if (code.size() < 20 || (code.size() % 4) != 0)
        return fail("SPIR-V must contain a complete 20-byte header and aligned words");

    const auto word = [&](size_t index) {
        uint32_t value = 0;
        std::memcpy(&value, code.data() + index * 4, sizeof(value));
        return value;
    };
    if (word(0) != 0x07230203u)
        return fail("invalid SPIR-V magic");
    // Vulkan 1.3 accepts SPIR-V up to 1.6; reject future/invalid headers.
    const uint32_t version = word(1);
    if ((version & 0xff0000ffu) != 0 || ((version >> 16) & 0xffu) != 1 ||
        ((version >> 8) & 0xffu) > 6)
        return fail("unsupported SPIR-V version");
    if (word(3) == 0)
        return fail("SPIR-V ID bound must not be zero");
    if (word(4) != 0)
        return fail("SPIR-V reserved schema word must be zero");

    // Instructions start at word 5; each header encodes its length in
    // the upper 16 bits. Prevent zero-length instructions and overruns.
    const size_t count = code.size() / 4;
    for (size_t i = 5; i < count;) {
        const size_t length = word(i) >> 16;
        if (length == 0 || length > count - i)
            return fail("invalid SPIR-V instruction word count");
        i += length;
    }
    if (error)
        error->clear();
    return true;
}

} // namespace smvk
