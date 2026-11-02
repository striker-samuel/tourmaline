// NVIDIA Nsight Aftermath SDK is externally installed; never vendored.
#include <Engine/Profile/aftermath_sdk.h>
#if defined(TOURMALINE_USE_AFTERMATH_SDK)
#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
namespace {
std::atomic<unsigned long long> serial{0};
std::atomic<bool> enabled{false};
std::mutex ioMutex;
// PRIVATE diagnostic output, NOT a shipped artefact. Callbacks can execute
// asynchronously and must not use rendering resources or Vulkan entry points.
void WriteArtifact(const char* ext, const void* data, uint32_t size) {
    if (!data || !size) return;
    std::lock_guard<std::mutex> lock(ioMutex);
    std::error_code ec;
    const std::filesystem::path dir = "out/aftermath-private";
    std::filesystem::create_directories(dir, ec);
    if (ec) return;
    const auto path = dir / (std::to_string(++serial) + ext);
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    }
}
void OnCrashDump(const void* data, uint32_t size, void*) { WriteArtifact(".nv-gpudmp", data, size); }
void OnShaderDebug(const void* data, uint32_t size, void*) { WriteArtifact(".nvdbg", data, size); }
}
void tour::aftermath::Initialize() {
    if (enabled.load()) return;
    const char* disable = std::getenv("TOURMALINE_AFTERMATH");
    if (disable && disable[0] == '0' && disable[1] == 0) return;
    const auto result = GFSDK_Aftermath_EnableGpuCrashDumps(
        GFSDK_Aftermath_Version_API,
        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
        &OnCrashDump, &OnShaderDebug, nullptr, nullptr);
    enabled.store(result == GFSDK_Aftermath_Result_Success);
    if (!enabled.load()) std::fprintf(stderr, "[Aftermath] GPU crash dump callback initialization failed: 0x%x\n", unsigned(result));
}
void tour::aftermath::Shutdown() {
    if (enabled.exchange(false)) GFSDK_Aftermath_DisableGpuCrashDumps();
}
#else
void tour::aftermath::Initialize() {}
void tour::aftermath::Shutdown() {}
#endif
