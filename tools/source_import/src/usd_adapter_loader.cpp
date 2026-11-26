// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <source_import/usd_adapter.h>

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace source_import {

// The C-ABI entry point exported by usd_import.<ext>: fills a UsdAdapterApi
// table. Unmangled ("extern \"C\"") so both sides agree on the name.
using UsdGetApiFn = int (*)(UsdAdapterApi*, std::uint32_t);

bool LoadUsdAdapter(UsdAdapterApi* api, std::string* error) {
    if (!api) {
        if (error)
            *error = "null adapter table";
        return false;
    }

#if defined(_WIN32)
    HMODULE mod = LoadLibraryA("usd_import.dll");
    if (!mod) {
        if (error)
            *error = "usd_import.dll not found (build with "
                     "-DTOURMALINE_WITH_USD=ON and keep the OpenUSD runtime on PATH)";
        return false;
    }
    auto getApi = reinterpret_cast<UsdGetApiFn>(
        reinterpret_cast<void*>(GetProcAddress(mod, "TourmalineUsdGetApi")));
#else
    void* mod = dlopen("libusd_import.so", RTLD_NOW | RTLD_LOCAL);
    if (!mod) {
        if (error) {
            const char* d = dlerror();
            *error = std::string("libusd_import.so not found (")
                         .append(d ? d : "dlopen failed")
                         .append(") — build with -DTOURMALINE_WITH_USD=ON");
        }
        return false;
    }
    auto getApi = reinterpret_cast<UsdGetApiFn>(dlsym(mod, "TourmalineUsdGetApi"));
#endif

    if (!getApi || !getApi(api, static_cast<std::uint32_t>(sizeof(UsdAdapterApi)))) {
        if (error)
            *error = "usd_import adapter entry point missing or ABI mismatch";
        return false;
    }
    return true;
}

} // namespace source_import
