// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Compile this file with a C compiler: no C++ or std::string_view leaks.
// LOCAL-ONLY dependency: the included project_c.h belongs to the
// untracked samples/04_project_game (EXISTS-guarded in tests/CMakeLists).
#include "../samples/04_project_game/project/project_c.h"
#include <RuntimeC.h>
#include <assert.h>
#include <string.h>

_Static_assert(TOUR_C_CATALOG_ABI_VERSION == 1, "catalog ABI");
_Static_assert(TOUR_C_KIND_SCENE == 1, "deployed kind identity");
_Static_assert(TOUR_C_KIND_NAVMESH == 10, "deployed kind identity");

int main(void) {
    char error[256] = {0};
    // The sample's real generated project_c.h must be consumable by C11.
    assert(tour_project_catalog.abi_version == TOUR_C_CATALOG_ABI_VERSION);
    assert(tour_project_catalog.asset_count == 5);
    assert(tour_project_catalog.entry_scene == TOUR_PROJECT_SCENE_game);
    assert(tour_project_catalog.fingerprint == TOUR_PROJECT_CATALOG_FINGERPRINT);
    assert(tour_project_assets[2].id == TOUR_PROJECT_MESH_game_2);
    assert(tour_project_cue_forest_storm.ordinal == 1);
    assert(TOUR_PROJECT_PAK_COUNT == 2);
    assert(tour_project_name[0] == 'G');
    // Calls live OUTSIDE assert(): NDEBUG strips the whole assert
    // statement including its expression, so an assert-embedded call
    // never runs in Release - and tour_c_bytes_free below then frees
    // the poison sentinel untouched (free((void*)1) = heap corruption
    // at exit). Assert RESULTS, never side effects.
    int ok = (tour_c_runtime_open(0, error, sizeof error) == 0);
    assert(ok);
    assert(strlen(error) > 0);
    ok = (tour_c_runtime_open("", error, sizeof error) == 0);
    assert(ok);
    assert(error[0] != 0);
    ok = (tour_c_runtime_attach_catalog(0, 0, error, sizeof error) == 0);
    assert(ok);
    assert(error[0] != 0);
    tour_c_bytes bytes = {(void*)1, 1};
    ok = (tour_c_runtime_read_asset(0, 0, &bytes, error, sizeof error) == 0);
    assert(ok);
    assert(bytes.data == 0 && bytes.size == 0);
    tour_c_bytes_free(&bytes);
    tour_c_runtime_close(0);
    return 0;
}
