#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Unknwn.h>
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
    IDxcUtils* utils = nullptr;
    IDxcCompiler3* compiler = nullptr;
    IDxcResult* result = nullptr;

    HRESULT hr = DxcCreateInstance(
        CLSID_DxcUtils,
        IID_PPV_ARGS(&utils)
    );

    if (FAILED(hr))
        throw std::runtime_error("Failed to create IDxcUtils");

    hr = DxcCreateInstance(
        CLSID_DxcCompiler,
        IID_PPV_ARGS(&compiler)
    );

    if (FAILED(hr))
    {
        utils->Release();
        throw std::runtime_error("Failed to create IDxcCompiler3");
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source.data();
    sourceBuffer.Size = source.size();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    const wchar_t* arguments[] =
    {
        L"-E", entryPoint,
        L"-T", GetShaderProfile(stage),

        L"-spirv",
        L"-fspv-target-env=vulkan1.3",

        // Useful with HLSL data layouts in Vulkan
        L"-fvk-use-dx-layout",

#ifndef NDEBUG
        L"-Zi",
        L"-Qembed_debug",
        // NOTE: keep vulkan (not vulkan-with-source): DXC emits invalid
        // DebugLine column ends vs the embedded source for some shaders,
        // rejected by spirv-val ("Column End ... larger then Line ...
        // column length ... found in the DebugSource text").
        L"-fspv-debug=vulkan",
#else
        L"-O3",
#endif
    };

    hr = compiler->Compile(
        &sourceBuffer,
        arguments,
        static_cast<uint32_t>(std::size(arguments)),
        nullptr,
        IID_PPV_ARGS(&result)
    );

    if (FAILED(hr))
    {
        compiler->Release();
        utils->Release();
        throw std::runtime_error("DXC Compile() call failed");
    }

    // Compiler warnings/errors
    IDxcBlobUtf8* errors = nullptr;

    result->GetOutput(
        DXC_OUT_ERRORS,
        IID_PPV_ARGS(&errors),
        nullptr
    );

    if (errors && errors->GetStringLength() > 0)
    {
        std::fprintf(
            stderr,
            "%s\n",
            errors->GetStringPointer()
        );
    }

    HRESULT compileStatus;
    result->GetStatus(&compileStatus);

    if (FAILED(compileStatus))
    {
        if (errors)
            errors->Release();

        result->Release();
        compiler->Release();
        utils->Release();

        throw std::runtime_error("HLSL compilation failed");
    }

    // Retrieve compiled SPIR-V
    IDxcBlob* object = nullptr;

    hr = result->GetOutput(
        DXC_OUT_OBJECT,
        IID_PPV_ARGS(&object),
        nullptr
    );

    if (FAILED(hr) || !object)
    {
        if (errors)
            errors->Release();

        result->Release();
        compiler->Release();
        utils->Release();

        throw std::runtime_error("Failed to retrieve SPIR-V");
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

    return spirv;
}