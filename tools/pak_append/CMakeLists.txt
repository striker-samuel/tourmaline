# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(PakAppend)

# pak_append: append-grow merge of one PakAssetKind from a source pak into a
# destination pak (see main.cpp). CPU-only (PakReader/PakWriter path).
add_executable(PakAppend main.cpp)

target_compile_definitions(PakAppend PRIVATE ENGINE_VULKAN)

target_link_libraries(PakAppend
    PRIVATE
        engine
        volk
)

set_target_properties(PakAppend PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
