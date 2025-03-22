project(App)

file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/App/*.h
)

add_executable(app ${APP_SRC})

target_include_directories(app
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(app
    PRIVATE
        engine
)
