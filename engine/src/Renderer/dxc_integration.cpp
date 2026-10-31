// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// In-process DXC (HLSL -> SPIR-V) integration. The DxcCreateInstance ABI
// is the same COM shape on both platforms: on Windows it comes from the
// vendored external/dxc MSVC package (dxcompiler.lib + dxcompiler.dll);
// on Linux from the vendored external/dxc linux package (lib/
// libdxcompiler.so) whose dxcapi.h + WinAdapter.h emulate the COM basics
// (IUnknown, HRESULT, __uuidof, IID_PPV_ARGS) - so only the platform
// include line below differs.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Unknwn.h>
#endif
#include <dxc/dxcapi.h>

#include <string_view>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <cstring>

#include <Engine/Renderer/dxc_integration.h>

static const wchar_t* GetShaderProfile(DxcShaderStage stage)
{
    switch (stage)
    {
    case DxcShaderStage::Vertex:   return L"vs_6_6";
    case DxcShaderStage::Fragment: return L"ps_6_6";
    case DxcShaderStage::Compute:  return L"cs_6_6";
    case DxcShaderStage::Task:     return L"as_6_7";
    case DxcShaderStage::Mesh:     return L"ms_6_7";
    }

    return nullptr;
}

std::vector<char> CompileHLSLToSPIRV(
    std::string_view source,
    DxcShaderStage stage,
    const wchar_t* entryPoint
)
{
    // The legacy self-contained form: no include dir, no defines.
    return CompileHLSLToSPIRV(source, stage, entryPoint,
        std::wstring(), {}, nullptr);
}

std::vector<char> CompileHLSLToSPIRV(
    std::string_view source,
    DxcShaderStage stage,
    const wchar_t* entryPoint,
    const std::wstring& includeDir,
    const std::vector<std::wstring>& defines,
    std::string* outDiagnostics
)
{
    IDxcUtils* utils = nullptr;
    IDxcCompiler3* compiler = nullptr;
    IDxcResult* result = nullptr;
    IDxcIncludeHandler* includes = nullptr;

    auto giveUp = [&](const char* what, const std::string& diag) {
        if (includes)
            includes->Release();
        if (result)
            result->Release();
        if (compiler)
            compiler->Release();
        if (utils)
            utils->Release();
        if (outDiagnostics && !diag.empty())
            *outDiagnostics = diag;
        // The diagnostics ride the throw too — the reload service
        // surfaces them either way.
        throw std::runtime_error(diag.empty() ? what
            : std::string(what) + ": " + diag);
    };

    HRESULT hr = DxcCreateInstance(
        CLSID_DxcUtils,
        IID_PPV_ARGS(&utils)
    );

    if (FAILED(hr))
        giveUp("Failed to create IDxcUtils", {});

    hr = DxcCreateInstance(
        CLSID_DxcCompiler,
        IID_PPV_ARGS(&compiler)
    );

    if (FAILED(hr))
        giveUp("Failed to create IDxcCompiler3", {});

    // The include handler (the engine's shaders #include their .hlsli
    // headers — without a handler every include errors).
    hr = utils->CreateDefaultIncludeHandler(&includes);
    if (FAILED(hr))
        giveUp("Failed to create the include handler", {});

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source.data();
    sourceBuffer.Size = source.size();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    // The argument list (built dynamically: the -I/-D entries are
    // optional). Mirrors cmake/CompileShaders.cmake's flag set.
    std::vector<const wchar_t*> arguments;
    arguments.push_back(L"-E");
    arguments.push_back(entryPoint ? entryPoint : L"main");
    arguments.push_back(L"-T");
    arguments.push_back(GetShaderProfile(stage));
    arguments.push_back(L"-spirv");
    arguments.push_back(L"-fspv-target-env=vulkan1.3");
    // Useful with HLSL data layouts in Vulkan
    arguments.push_back(L"-fvk-use-dx-layout");
    if (!includeDir.empty()) {
        arguments.push_back(L"-I");
        arguments.push_back(includeDir.c_str());
    }
    for (const std::wstring& define : defines) {
        arguments.push_back(L"-D");
        arguments.push_back(define.c_str());
    }
#ifndef NDEBUG
    arguments.push_back(L"-Zi");
    arguments.push_back(L"-Qembed_debug");
    // NOTE: keep vulkan (not vulkan-with-source): DXC emits invalid
    // DebugLine column ends vs the embedded source for some shaders,
    // rejected by spirv-val ("Column End ... larger then Line ...
    // column length ... found in the DebugSource text").
    arguments.push_back(L"-fspv-debug=vulkan");
#else
    arguments.push_back(L"-O3");
#endif

    hr = compiler->Compile(
        &sourceBuffer,
        arguments.data(),
        static_cast<uint32_t>(arguments.size()),
        includes,
        IID_PPV_ARGS(&result)
    );

    if (FAILED(hr))
        giveUp("DXC Compile() call failed", {});

    // Compiler warnings/errors
    IDxcBlobUtf8* errors = nullptr;

    result->GetOutput(
        DXC_OUT_ERRORS,
        IID_PPV_ARGS(&errors),
        nullptr
    );

    std::string errorText;
    if (errors && errors->GetStringLength() > 0) {
        errorText = errors->GetStringPointer();
        std::fprintf(stderr, "%s\n", errors->GetStringPointer());
    }

    HRESULT compileStatus;
    result->GetStatus(&compileStatus);

    if (FAILED(compileStatus)) {
        if (errors)
            errors->Release();
        giveUp("HLSL compilation failed", errorText);
    }

    // Retrieve compiled SPIR-V
    IDxcBlob* object = nullptr;

    hr = result->GetOutput(
        DXC_OUT_OBJECT,
        IID_PPV_ARGS(&object),
        nullptr
    );

    if (FAILED(hr) || !object) {
        if (errors)
            errors->Release();
        giveUp("Failed to retrieve SPIR-V", errorText);
    }

    const char* begin =
        static_cast<const char*>(object->GetBufferPointer());

    std::vector<char> spirv(
        begin,
        begin + object->GetBufferSize()
    );

    object->Release();

    if (errors)
        errors->Release();

    result->Release();
    compiler->Release();
    utils->Release();
    includes->Release();

    return spirv;
}
