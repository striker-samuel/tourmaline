project(ArtifactCache)

# Editor artifact cache (scratch-disk registry + per-class pak store):
# shared by the Editor (registry/store) and, later, PakCooker (cook
# acceleration lookups). Never linked into App/engine — the runtime
# never sees the cache.
add_library(artifact_cache STATIC
    src/artifact_cache.cpp
)

target_include_directories(artifact_cache
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(artifact_cache
    PUBLIC
        SQLiteCpp
    # AppendPakBlobs + pak constants (scratch paks are real paks: the
    # editor VFS mounts them through PakBackend in the render phase).
    PRIVATE
        engine
)
