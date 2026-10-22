# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(Asteroids)

# Asteroids.exe: the sponza invaders GAME host. The same pak-viewer
# pipeline as App (full WorldRenderer pass set — shadows, GI, SSR,
# volumetrics, particles, the works) plus the invaders game element
# (src/InvadersGame.cpp: fixed top-down camera over the atrium, green
# player ball vs the red invader grid, GPU particle bursts on every
# death/hit/fire) stacked over the renderer element through RunPakApp's
# extra-element hook (see app/src/Host.h). The shared host plumbing
# (CLI parsing, pak preload + world source) is compiled from app/src —
# the game IS a pak-viewer host variant, not a fork.
add_executable(Asteroids
    src/main.cpp
    src/InvadersGame.cpp
    # Shared pak-viewer host plumbing (app/src/Host.h is the contract).
    ${CMAKE_SOURCE_DIR}/app/src/PakApp.cpp
    ${CMAKE_SOURCE_DIR}/app/src/PakAppArgs.cpp
    ${CMAKE_SOURCE_DIR}/app/src/PakPreload.cpp
)

target_include_directories(Asteroids PRIVATE
    ${CMAKE_SOURCE_DIR}/app/src   # Host.h, PakPreload.h
)

tourmaline_host_setup(Asteroids)
