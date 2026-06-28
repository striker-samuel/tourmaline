#define TINYGLTF_IMPLEMENTATION

#define VMA_IMPLEMENTATION
#define VMA_VULKAN_VERSION 1003800
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#define VOLK_IMPLEMENTATION

#include <iostream>
int main() {
  std::cout << "Engine stub running!" << std::endl;
  return 0;
}
