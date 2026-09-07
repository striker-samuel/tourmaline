project(LZ4 C)

add_library(lz4 STATIC lz4.c)

target_include_directories(lz4 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(lz4 PROPERTIES C_STANDARD 99 LINKER_LANGUAGE C)

if(MSVC)
  # lz4.c is clean; silence benign warnings when built with /W3.
  target_compile_options(lz4 PRIVATE /w)
endif()
