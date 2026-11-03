cmake_minimum_required(VERSION 3.20)
project(NativeGameStarter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reuse Tourmaline as an engine submodule. This option builds the native
# application host + cooking tools, not the editor or engine samples.
set(TOURMALINE_ENABLE_GAME_HOST ON CACHE BOOL
    "Build Tourmaline host/cooker for native games" FORCE)
set(TOURMALINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/tourmaline")
if(NOT EXISTS "${TOURMALINE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Missing engine submodule. Run git submodule update --init --recursive")
endif()
add_subdirectory("${TOURMALINE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/tourmaline")
include("${TOURMALINE_SOURCE_DIR}/cmake/HostSetup.cmake")

tourmaline_add_project(NativeGame
    RECIPE "${CMAKE_CURRENT_SOURCE_DIR}/project/game.tourrecipe")

add_executable(NativeGame src/main.cpp)
target_compile_features(NativeGame PRIVATE cxx_std_20)
target_link_libraries(NativeGame PRIVATE tourmaline_apphost)
tourmaline_host_setup(NativeGame)
tourmaline_attach_project(NativeGame NativeGame)

# No renderer or native graphics device required for these tests.
enable_testing()
add_executable(NativeGameTests tests/game_test.cpp)
target_compile_features(NativeGameTests PRIVATE cxx_std_20)
target_include_directories(NativeGameTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_test(NAME native_game_state COMMAND NativeGameTests)
