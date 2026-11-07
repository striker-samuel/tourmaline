// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Standalone synthetic pager hardware baseline. No engine or content dependencies.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
void VkOk(VkResult r, const char* label) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(label) + ": VkResult=" + std::to_string(r));
}
struct Args {
    std::string mode = "all";
    std::vector<size_t> sizes{1, 4, 16, 64};
    int iterations = 24, warmup = 4;
    bool json = false;
    std::string diskPath;
};
size_t ParseNumber(const std::string& s) {
    size_t parsed = 0;
    const auto n = std::stoull(s, &parsed);
    if (parsed != s.size() || n == 0 || n > 1024) throw std::runtime_error("size must be 1..1024 MiB");
    return size_t(n);
}
Args Parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help") {
            std::cout << "Synthetic pager hardware benchmarks\n"
                << "  --mode all|cpu|disk|gpu   (default all)\n"
                << "  --sizes-mib 1,4,16,64    (1..1024 MiB each)\n"
                << "  --iterations N           (default 24)\n"
                << "  --warmup N               (default 4)\n"
                << "  --disk-path FILE         (optional synthetic temporary file)\n"
                << "  --json                   (JSON lines, one record per test/size)\n";
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
        const std::string value = argv[++i];
        if (flag == "--mode") a.mode = value;
        else if (flag == "--sizes-mib") {
            a.sizes.clear();
            size_t start = 0;
            while (start < value.size()) {
                const auto comma = value.find(',', start);
                a.sizes.push_back(ParseNumber(value.substr(start, comma == std::string::npos ? comma : comma - start)));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (a.sizes.empty()) throw std::runtime_error("empty sizes");
        } else if (flag == "--iterations" || flag == "--warmup") {
            size_t pos = 0;
            int number = std::stoi(value, &pos);
            if (pos != value.size() || number < (flag == "--iterations" ? 1 : 0) || number > 100000)
                throw std::runtime_error("invalid iteration count");
            (flag == "--iterations" ? a.iterations : a.warmup) = number;
        } else if (flag == "--disk-path") a.diskPath = value;
        else if (flag == "--json") throw std::runtime_error("--json takes no value");
        else throw std::runtime_error("unknown option: " + flag);
    }
    if (a.mode != "all" && a.mode != "cpu" && a.mode != "disk" && a.mode != "gpu")
        throw std::runtime_error("invalid --mode");
    return a;
}
struct Result {
    std::string test, method, device;
    size_t bytes = 0;
    int iterations = 0;
    double p50 = 0, p95 = 0, minimum = 0, maximum = 0, mean = 0, mibps = 0;
};
Result Summarize(const std::string& test, const std::string& method,
                 const std::string& device, size_t bytes, std::vector<double> samples) {
    if (samples.empty()) throw std::runtime_error("no samples");
    const int n = int(samples.size());
    const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](double q) {
        const auto index = (samples.size() - 1) * q;
        const auto lo = size_t(index), hi = std::min(samples.size() - 1, lo + 1);
        return samples[lo] + (samples[hi] - samples[lo]) * (index - lo);
    };
    const double mean = sum / n;
    return {test, method, device, bytes, n, percentile(0.5), percentile(0.95),
            samples.front(), samples.back(), mean, mean > 0 ? (double(bytes) / 1048576.0) / (mean / 1000.0) : 0};
}
void Print(const Result& r, bool json) {
    if (json) {
        std::cout << std::setprecision(9) << "{\"schema\":1,\"test\":\"" << r.test
            << "\",\"timing\":\"" << r.method << "\",\"device\":\"" << r.device
            << "\",\"bytes\":" << r.bytes << ",\"iterations\":" << r.iterations
            << ",\"mean_ms\":" << r.mean << ",\"p50_ms\":" << r.p50 << ",\"p95_ms\":" << r.p95
            << ",\"min_ms\":" << r.minimum << ",\"max_ms\":" << r.maximum
            << ",\"effective_mib_s\":" << r.mibps << "}\n";
    } else {
        std::cout << std::fixed << std::setprecision(3) << std::setw(16) << r.test
            << " " << std::setw(5) << r.bytes / 1048576 << " MiB  mean "
            << std::setw(9) << r.mean << " ms  p50 " << std::setw(9) << r.p50
            << " ms  p95 " << std::setw(9) << r.p95 << " ms  "
            << std::setw(10) << r.mibps << " MiB/s  [" << r.method << "]\n";
    }
}
std::vector<double> Measure(int warmup, int iterations, const auto& op) {
    std::vector<double> times;
    times.reserve(iterations);
    for (int i = -warmup; i < iterations; ++i) {
        const auto begin = Clock::now();
        op();
        const auto end = Clock::now();
        if (i >= 0) times.push_back(Ms(begin, end));
    }
    return times;
}
void Cpu(size_t bytes, const Args& args) {
    std::vector<uint8_t> src(bytes, 0x5a), dst(bytes, 0);
    volatile uint8_t sink = 0;
    auto samples = Measure(args.warmup, args.iterations, [&] {
        std::memcpy(dst.data(), src.data(), bytes);
        sink = dst[bytes - 1];
    });
    (void)sink;
    Print(Summarize("cpu_memcpy", "cpu_wall_complete", "CPU", bytes, std::move(samples)), args.json);
}
void Disk(size_t bytes, const Args& args) {
    const bool ownPath = args.diskPath.empty();
    const auto path = ownPath
        ? std::filesystem::temp_directory_path() /
          ("tourmaline-synthetic-" + std::to_string(Clock::now().time_since_epoch().count()) + ".bin")
        : std::filesystem::path(args.diskPath);
    struct Cleanup {
        std::filesystem::path path;
        bool own;
        ~Cleanup() { if (own) { std::error_code ec; std::filesystem::remove(path, ec); } }
    } cleanup{path, ownPath};
    std::vector<char> bytesIn(bytes, char(0x5a)), bytesOut(bytes, 0);
    auto write = [&] {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(bytesIn.data(), std::streamsize(bytes))) throw std::runtime_error("disk write failed");
        file.flush();
        if (!file) throw std::runtime_error("disk flush failed");
    };
    auto reads = [&] {
        std::ifstream file(path, std::ios::binary);
        if (!file || !file.read(bytesOut.data(), std::streamsize(bytes)))
            throw std::runtime_error("disk read failed");
        if (bytesOut.front() != char(0x5a) || bytesOut.back() != char(0x5a))
            throw std::runtime_error("disk data mismatch");
    };
    Print(Summarize("disk_write", "buffered_wall_flush_not_fsync", "filesystem", bytes,
          Measure(args.warmup, args.iterations, write)), args.json);
    Print(Summarize("disk_read", "buffered_wall_pagecache_possible", "filesystem", bytes,
          Measure(args.warmup, args.iterations, reads)), args.json);
}
struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t family = 0;
    std::string name;
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    Gpu() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "TourmalineSyntheticPagerBench";
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        VkOk(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        VkOk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPUs");
        if (!count) throw std::runtime_error("no Vulkan physical devices");
        std::vector<VkPhysicalDevice> devices(count);
        VkOk(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPUs");
        for (auto gpu : devices) {
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
            std::vector<VkQueueFamilyProperties> props(families);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, props.data());
            for (uint32_t f = 0; f < families; ++f) {
                if (props[f].queueCount && (props[f].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
                    physical = gpu;
                    family = f;
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("no transfer-capable Vulkan queue");
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(physical, &p);
        name = p.deviceName;
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
        VkOk(vkCreateDevice(physical, &di, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkOk(vkCreateCommandPool(device, &pi, nullptr, &pool), "vkCreateCommandPool");
    }
    ~Gpu() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    Buffer Create(size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags wanted) {
        Buffer b;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = bytes; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkOk(vkCreateBuffer(device, &ci, nullptr, &b.buffer), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        uint32_t chosen = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & wanted) == wanted) { chosen = i; break; }
        if (chosen == UINT32_MAX) {
            vkDestroyBuffer(device, b.buffer, nullptr);
            throw std::runtime_error("required Vulkan memory type unavailable");
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = chosen;
        VkOk(vkAllocateMemory(device, &ai, nullptr, &b.memory), "vkAllocateMemory");
        VkOk(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        if (wanted & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            VkOk(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
        return b;
    }
    void Destroy(Buffer& b) {
        if (b.mapped) vkUnmapMemory(device, b.memory);
        if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device, b.memory, nullptr);
        b = {};
    }
    void Run(size_t bytes, const Args& args) {
        Buffer upload{}, readback{}, local{};
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        try {
            upload = Create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            readback = Create(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            local = Create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            std::memset(upload.mapped, 0x5a, bytes);
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            VkOk(vkAllocateCommandBuffers(device, &ai, &command), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkOk(vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence");
            auto copy = [&](VkBuffer src, VkBuffer dst) {
                VkOk(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                VkOk(vkBeginCommandBuffer(command, &bi), "vkBeginCommandBuffer");
                VkBufferCopy region{}; region.size = bytes;
                vkCmdCopyBuffer(command, src, dst, 1, &region);
                VkOk(vkEndCommandBuffer(command), "vkEndCommandBuffer");
                VkOk(vkResetFences(device, 1, &fence), "vkResetFences");
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
                VkOk(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
                VkOk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
            };
            Print(Summarize("gpu_upload", "cpu_wall_submit_to_fence_inclusive", name, bytes,
                  Measure(args.warmup, args.iterations, [&] { copy(upload.buffer, local.buffer); })), args.json);
            Print(Summarize("gpu_readback", "cpu_wall_submit_to_fence_inclusive", name, bytes,
                  Measure(args.warmup, args.iterations, [&] { copy(local.buffer, readback.buffer); })), args.json);
            const auto* p = static_cast<const uint8_t*>(readback.mapped);
            if (p[0] != 0x5a || p[bytes - 1] != 0x5a) throw std::runtime_error("GPU copy data mismatch");
        } catch (...) {
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (command) vkFreeCommandBuffers(device, pool, 1, &command);
            Destroy(local); Destroy(readback); Destroy(upload);
            throw;
        }
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, pool, 1, &command);
        Destroy(local); Destroy(readback); Destroy(upload);
    }
};
} // namespace

int main(int argc, char** argv) {
    try {
        // --json is intentionally a flag; preprocess it before key/value options.
        std::vector<char*> forwarded;
        forwarded.push_back(argv[0]);
        bool json = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--json") == 0) json = true;
            else forwarded.push_back(argv[i]);
        }
        Args args = Parse(int(forwarded.size()), forwarded.data());
        args.json = json;
        if (!json) std::cout << "Synthetic benchmarks: steady-clock wall latency; MiB = 2^20 bytes\n";
        for (size_t mib : args.sizes) {
            const size_t bytes = mib * size_t(1048576);
            if (args.mode == "all" || args.mode == "cpu") Cpu(bytes, args);
            if (args.mode == "all" || args.mode == "disk") Disk(bytes, args);
        }
        if (args.mode == "all" || args.mode == "gpu") {
            Gpu gpu;
            for (size_t mib : args.sizes) gpu.Run(mib * size_t(1048576), args);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "pager_bench: " << e.what() << '\n';
        return 1;
    }
}
