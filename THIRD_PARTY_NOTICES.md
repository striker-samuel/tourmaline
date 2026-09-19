## Third-Party Libraries

This project makes use of the following open-source libraries:

- [tinygltf](https://github.com/syoyo/tinygltf) � MIT License  
- [volk](https://github.com/zeux/volk) � MIT License  
- [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) � MIT License  
- [Dear ImGui](https://github.com/ocornut/imgui) � MIT License  
- [nlohmann/json](https://github.com/nlohmann/json) � MIT License  
- [GLFW](https://www.glfw.org) � zlib/libpng License  
- [stb_image](https://github.com/nothings/stb) � Public Domain / MIT
- [GLM](https://github.com/g-truc/glm) � Happy Bunny License (MIT-like)
- [LZ4](https://github.com/lz4/lz4) (vendored `external/lz4/lz4.{h,c}`) � BSD 2-Clause License
- [zlib](https://zlib.net) (vendored `external/zlib`, CRC/reference only) � zlib License
- [https://github.com/PacktPublishing/Mastering-Graphics-Programming-with-Vulkan] - MIT License.
- [SMAA](https://github.com/iryoku/smaa) - Enhanced Subpixel Morphological Antialiasing, by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (2013) - MIT License. Used for the SMAA 1x / T2x antialiasing chain: the shader core is ported in `engine/shaders/smaa.hlsli`, and the precomputed area/search lookup tables are embedded byte-exact in `engine/include/Engine/Rendergraph/smaa_area_tex.h` and `smaa_search_tex.h`.
- [xatlas](https://github.com/jpcy/xatlas) (vendored `external/xatlas/source/xatlas/xatlas.{h,cpp}`) — MIT License, (c) 2018-2020 Jonathan Young. Used for the `--lm-charts xatlas` lightmap path (chart + brute-force pack, cook time only).
