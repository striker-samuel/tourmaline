# SPDX-License-Identifier: MIT
# Independent test build: no Vulkan or third-party game dependencies.
cmake_minimum_required(VERSION 3.20)
project(TourmalinePortkitExperiment LANGUAGES C CXX)
add_library(tour_portkit INTERFACE)
target_include_directories(tour_portkit INTERFACE .)
target_compile_features(tour_portkit INTERFACE cxx_std_20)
enable_testing()
add_executable(tour_portkit_tests tests.cpp)
target_link_libraries(tour_portkit_tests PRIVATE tour_portkit)
add_test(NAME recomp_asset_free COMMAND tour_portkit_tests)

add_executable(tour_recomp_c_abi tests_abi.c)
target_include_directories(tour_recomp_c_abi PRIVATE .)
set_target_properties(tour_recomp_c_abi PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)
add_test(NAME recomp_c_abi COMMAND tour_recomp_c_abi)
