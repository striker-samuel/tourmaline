// Optional private NVIDIA Aftermath SDK GPU crash capture.
#pragma once
namespace tour::aftermath {
void Initialize(); // Before Vulkan instance/device creation; no-op unless linked.
void Shutdown();   // On normal shutdown; no-op unless linked.
}
