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
