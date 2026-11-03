# Project-local shader compilation + constexpr SPIR-V reflection

The shader output is still the normal DXC-generated \`.spv\` file. This opt-in
build step reads that compiled SPIR-V using the already-vendored SPIRV-Reflect
library and generates a C++20 header with **no runtime reflection requirement**.

## Example: GameProject

\`samples/04_project_game/CMakeLists.txt\` enables:

\`\`\`cmake
compile_shaders(
    "\${CMAKE_CURRENT_SOURCE_DIR}/project/content/shaders"
    "\${CMAKE_BINARY_DIR}/content/shaders"
    GameProjectLit
    INCLUDE_DIRS
        "\${CMAKE_CURRENT_SOURCE_DIR}/project/content/shaders"
        "\${TOURMALINE_SOURCE_DIR}/engine/shaders"
    HEADER_DIRS
        "\${CMAKE_CURRENT_SOURCE_DIR}/project/content/shaders"
        "\${TOURMALINE_SOURCE_DIR}/engine/shaders"
    VARIANTS \${GAME_PROJECT_LIT_VARIANTS}
    OUTPUT_HEADER "\${CMAKE_CURRENT_BINARY_DIR}/generated/project_compiled_shaders.h"
    RUNTIME_ROOT "\${CMAKE_BINARY_DIR}"
)
add_dependencies(GameProject GameProjectLit_shaders)
target_include_directories(GameProject PRIVATE
    "\${CMAKE_CURRENT_BINARY_DIR}/generated")
\`\`\`

The generated \`project.h\` includes \`project_compiled_shaders.h\` when available.
The compiler then sees:

\`\`\`cpp
static_assert(project::compiled_shaders::project_lit_frag_hlsl.stage ==
              project::compiled_shaders::Stage::Fragment);

constexpr auto& shader = project::compiled_shaders::project_lit_frag_hlsl;
// shader.path: "content/shaders/project_lit.frag.hlsl.spv"
// shader.entryPoint: "mainPS"
// shader.stableId: FNV-1a64 of exe-relative compiled shader path
// shader.descriptors: set/binding/count/SPIRV-Reflect enum value
// shader.pushConstants: offset/size
// shader.specializationConstants: spec ID/default scalar byte size
// shader.vertexInputs: location/component count for a vertex module
\`\`\`

The generator emits all compiled variants separately, including SG and GI
permutations. It rejects invalid SPIR-V and duplicate output symbols rather
than silently changing an identifier.

## Example: Asteroids PR #49

PR #49 (https://github.com/striker-samuel/tourmaline/pull/49) adds
\`samples/05_asteroids/shaders/asteroids_gameui.frag.hlsl\`, compiled through
\`AsteroidsUi_shaders\`. When that PR is integrated, use the same optional
keyword arguments in its existing \`compile_shaders\` call:

\`\`\`cmake
compile_shaders(
    "\${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "\${CMAKE_BINARY_DIR}/content/shaders"
    AsteroidsUi
    INCLUDE_DIRS
        "\${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "\${TOURMALINE_SOURCE_DIR}/engine/shaders"
    HEADER_DIRS
        "\${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "\${TOURMALINE_SOURCE_DIR}/engine/shaders"
    OUTPUT_HEADER "\${CMAKE_CURRENT_BINARY_DIR}/generated/project_compiled_shaders.h"
    RUNTIME_ROOT "\${CMAKE_BINARY_DIR}"
)
target_include_directories(Asteroids PRIVATE
    "\${CMAKE_CURRENT_BINARY_DIR}/generated")
\`\`\`

Its \`AsteroidsUi_shaders\` dependency already ensures the generated header
exists before compiling \`Asteroids\`. For this standalone game, include
\`"project_compiled_shaders.h"\` directly (it has no generated \`project.h\`):

\`\`\`cpp
#include "project_compiled_shaders.h"

constexpr auto& ui = project::compiled_shaders::asteroids_gameui_frag_hlsl;
static_assert(ui.stage == project::compiled_shaders::Stage::Fragment);

// Current renderer API accepts a shader path. No hard-coded string needed:
ctx.renderer().SetGameUiFragmentShaderPath(std::string(ui.path));
\`\`\`

This PR targets \`master\` independently of the unmerged Asteroids PR. It does
not rewrite or rebase #49.

## Important distinction: shader reflection vs asset identity

\`project::shaders::*\` remains the 0x5xxx **asset catalog** for compiled
\`.spv\` files owned by a project recipe and packaged in a .pak.
\`project::compiled_shaders::*\` represents build-generated shader *interface
metadata* for project-local \`.spv\` outputs under the runtime directory.
The latter uses deterministic path hashes, NOT assets.db IDs; this deliberately
does not change .tapp catalog fingerprints, asset ownership, or pak recipes.

Reflection is extracted from the **compiled** SPIR-V (not parsed HLSL).
The current generator includes descriptors, push-constant blocks, spec constants,
and vertex inputs; descriptor \`type\` is the numerical SpvReflectDescriptorType
value, and runtime validation of shader interfaces is left to the renderer.

## Validation

Run a configured build with DXC and the pinned SPIRV-Reflect dependency:

\`\`\`sh
cmake --build out/build/linux-Release --target GameProject
\`\`\`

Expected generated header:

\`out/build/linux-Release/samples/04_project_game/generated/project_compiled_shaders.h\`

Check incremental builds: changing one shader recompiles that SPIR-V and
regenerates metadata; unchanged metadata is written only when content differs.
Verify the Asteroids path against PR #49 after its integration.
