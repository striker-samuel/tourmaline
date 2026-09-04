project(AssetTests)

add_executable(AssetTests test_assets.cpp)

target_link_libraries(AssetTests
    PRIVATE
        engine
)

set_target_properties(AssetTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME asset_tests COMMAND AssetTests)

add_executable(SpirvReflectionTests test_spirv_reflection.cpp)

target_link_libraries(SpirvReflectionTests
    PRIVATE
        engine
)

set_target_properties(SpirvReflectionTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME spirv_reflection_tests COMMAND SpirvReflectionTests)
