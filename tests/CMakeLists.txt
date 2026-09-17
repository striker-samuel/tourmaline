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

add_executable(GfxFilterTests test_gfxfilter.cpp)

target_link_libraries(GfxFilterTests
    PRIVATE
        engine
)

set_target_properties(GfxFilterTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME gfxfilter_tests COMMAND GfxFilterTests)

add_executable(ExposureTests test_exposure.cpp)

target_link_libraries(ExposureTests
    PRIVATE
        engine
)

set_target_properties(ExposureTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME exposure_tests COMMAND ExposureTests)

add_executable(LightTests test_light.cpp)

target_link_libraries(LightTests
    PRIVATE
        engine
)

set_target_properties(LightTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME light_tests COMMAND LightTests)

add_executable(CameraTests test_camera.cpp)

target_link_libraries(CameraTests
    PRIVATE
        engine
)

set_target_properties(CameraTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME camera_tests COMMAND CameraTests)

add_executable(SmaaJitterTests test_smaajitter.cpp)

target_link_libraries(SmaaJitterTests
    PRIVATE
        engine
)

set_target_properties(SmaaJitterTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME smaajitter_tests COMMAND SmaaJitterTests)

add_executable(SsrTaaTests test_ssrtaa.cpp)

target_link_libraries(SsrTaaTests
    PRIVATE
        engine
)

set_target_properties(SsrTaaTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME ssrtaa_tests COMMAND SsrTaaTests)

add_executable(GiTests test_gi.cpp)

target_link_libraries(GiTests
    PRIVATE
        engine
)

set_target_properties(GiTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME gi_tests COMMAND GiTests)

add_executable(StreamingTests test_streaming.cpp)

target_link_libraries(StreamingTests
    PRIVATE
        engine
)

set_target_properties(StreamingTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME streaming_tests COMMAND StreamingTests)

add_executable(DeferredShadowTests test_deferredshadow.cpp)

target_link_libraries(DeferredShadowTests
    PRIVATE
        engine
)

set_target_properties(DeferredShadowTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME deferredshadow_tests COMMAND DeferredShadowTests)

add_executable(CubemapTests test_cubemap.cpp)

target_link_libraries(CubemapTests
    PRIVATE
        engine
)

set_target_properties(CubemapTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME cubemap_tests COMMAND CubemapTests)

add_executable(ParticlesTests test_particles.cpp)

target_link_libraries(ParticlesTests
    PRIVATE
        engine
)

set_target_properties(ParticlesTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME particles_tests COMMAND ParticlesTests)

project(TourTests)

add_executable(TourTests test_tour.cpp)

target_link_libraries(TourTests
    PRIVATE
        engine
)

set_target_properties(TourTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME tour_tests COMMAND TourTests)

# SDL3 is linked PUBLIC via engine: stage the DLL next to each test exe.
vulkantoy_stage_sdl3(AssetTests)
vulkantoy_stage_sdl3(SpirvReflectionTests)
vulkantoy_stage_sdl3(GfxFilterTests)
vulkantoy_stage_sdl3(ExposureTests)
vulkantoy_stage_sdl3(LightTests)
vulkantoy_stage_sdl3(CameraTests)
vulkantoy_stage_sdl3(SmaaJitterTests)
vulkantoy_stage_sdl3(SsrTaaTests)
vulkantoy_stage_sdl3(GiTests)
vulkantoy_stage_sdl3(StreamingTests)
vulkantoy_stage_sdl3(DeferredShadowTests)
vulkantoy_stage_sdl3(CubemapTests)
vulkantoy_stage_sdl3(ParticlesTests)
vulkantoy_stage_sdl3(TourTests)
