# SPDX-License-Identifier: MIT
# Independent test build: no Vulkan or third-party game dependencies.
cmake_minimum_required(VERSION 3.20)
project(TourmalinePortkitExperiment LANGUAGES CXX)
add_library(tour_portkit INTERFACE)
target_include_directories(tour_portkit INTERFACE .)
target_compile_features(tour_portkit INTERFACE cxx_std_20)
enable_testing()
add_executable(tour_portkit_tests tests.cpp)
target_link_libraries(tour_portkit_tests PRIVATE tour_portkit)
add_test(NAME recomp_asset_free COMMAND tour_portkit_tests)
