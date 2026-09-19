# Vendored xatlas chart + pack library (MIT, (c) 2018-2020 Jonathan Young).
# Only the self-contained core (source/xatlas/xatlas.{h,cpp}) is built;
# the viewer/examples/thirdparty checkout next to it is NOT part of the build.
project(XAtlas LANGUAGES C CXX)

add_library(xatlas STATIC source/xatlas/xatlas.cpp)

target_include_directories(xatlas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)

set_target_properties(xatlas PROPERTIES CXX_STANDARD 20)

if(MSVC)
  # xatlas is clean but large; keep object files alongside the rest.
  target_compile_options(xatlas PRIVATE /W1)
endif()
