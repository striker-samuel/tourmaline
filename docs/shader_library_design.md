# Tourmaline: HLSL Shader Libraries, Material Instances, and 32-bit Userdata

**Status:** Phase A (the baseline userdata transport + the no-op
StandardLit hook family) is implemented on `master` — see AGENTS.md
"The lit-hook seam + the 32-bit userdata lane". Phases B/C below remain
proposals.  
**Target:** Tourmaline `master`, reviewed October 2026  
**Design principle:** Extend existing shaders with HLSL functions, not generated shader source.

## 1. Executive decision

Build a **compile-time HLSL library mechanism** around the existing shared lit fragment body. Tourmaline continues to implement glTF material evaluation, Forward+ decals, PBR/BRDF lighting, GI, shadows, fog, alpha handling, MRT exports, LOD, and rendering paths. A game supplies a few normal HLSL functions that edit either the evaluated surface or the final lit HDR color.

Keep **material**, **material instance**, **shader family**, and **view effects** separate:

- A **material** owns the shared authored PBR asset, a default `uint32_t userData`, and a compiled shader-family choice.
- A **material instance** references exactly one base material; it optionally overrides the 32-bit userdata. No copy of the 128-byte `MaterialValues` structure is needed.
- A **shader family** identifies a precompiled lit implementation: initially `StandardLit` and optional `ProjectLit`. It is selected by the base material and inherited by instances.
- A **view** owns full-screen damage/redout state in the post-process path, not in an arbitrary mesh material.

**Runtime data is dynamic; shader source is not.** Changing `userData` never compiles shaders or creates pipelines. A material instance must not implicitly become a unique shader program.

## 2. Goals and non-goals

### In scope

1. Reuse `default_lit_body.hlsli` rather than duplicate the entire lit shader.
2. Support normal HLSL `.hlsli` libraries with strongly typed, fixed hook signatures.
3. A standard no-op hook implementation and a project-supplied implementation.
4. A single `uint32_t` of user data on each material/instance; bit layout belongs to gameplay code.
5. Correct per-draw value forwarding for direct indexed, GPU-filtered indexed, and opaque mesh-shader lit paths.
6. Honor existing alpha-test/A2C, GI and SG permutations when binding a project shader family.
7. Keep material-instance value changes independent of material texture/resource IDs and avoid accidental draw-batch splitting.
8. Validate ABI/layout and regressions with real GPU path tests.

### Deliberately out of scope for v1

- Reflection, dynamic named properties, string replacement, material graphs, scripting languages, runtime HLSL editing or JIT compilation.
- Arbitrary descriptor/resource allocation per material instance; user data is one 32-bit word.
- Arbitrary overrides of normal, alpha, opacity/cutout, vertex displacement, tessellation, raster state, or render queue.
- Custom dissolve silhouettes without corresponding depth, seed, shadow and motion coverage integration.
- Every debug shader or the far-LOD vertex-lit shortcut automatically honoring project effects.
- Unlimited shader families or precompiling every family × GI × SG × A2C × mesh × LOD combination without an explicit need.

## 3. Existing architecture and constraints

Repository observations:

- `engine/shaders/default_lit_body.hlsli` contains the shared lit implementation. `default_lit.frag.hlsl`, `default_lit_a2c.frag.hlsl` and `gfxmesh_lit.frag.hlsl` are short entry wrappers around it. This is already the correct reuse seam.
- The body has **early returns** for unlit materials and debug modes. It writes three MRT outputs (`outColor`, `outAoTerm`, `outEnvSpec`). A post-lit hook applied only to `outColor` does not automatically modify AO/SSR companion exports or downstream composite contributions.
- `engine/shaders/material_evaluation.hlsli` already exists, but it currently serves PBR/debug evaluation and notes divergence from the lit body (notably normal mapping, occlusion, emissive and decals). Refactoring the PBR evaluator is not prerequisite to the hook system.
- Direct VS (`default.vert.hlsl`) reads a 256-byte push-constant block. A per-draw word in the existing tail at byte 244 is a promising unused slot, while byte 240 is already used by `ditherCloseup`. The scene push layout is checked against `tools/push_constants_idl/push_constants.yaml` and must not change its pre-192-byte field offsets.
- Filtered VS (`gfxfiltered.vert.hlsl`) and mesh shader (`gfxfilter.mesh.hlsl`) load the per-draw `GfxDraw` ring. `gfx::Draw` is **96 bytes**, and `GfxDraw` has `model`, `misc`, `misc2`, all already assigned. Adding a standalone uint to the existing 16-byte-aligned structure ordinarily makes the stride **112 bytes**; do not claim a free word.
- All participating stages have explicit fragment/varying locations; location 8 is occupied by `ditherCloseup`. Use a new explicit **location 9**, `nointerpolation uint userData`, after confirming each linked interface.
- Material GPU data is a 128-byte `MaterialValues`. Keep userdata outside that structure to avoid format/pak/ABI changes and to preserve per-instance independence.
- CMake already compiles `.hlsl` with DXC, supports include directories, and explicitly compiles SG/GI/`MAT_STANDARD` variants. It tracks engine `.hlsli` files broadly; project-local hook-file dependencies will need to be added for reliable rebuilds.
- Existing material ranges are keyed to material IDs and the renderer chooses different lit programs for GI, SG lobe count, layered versus standard materials, and mesh/indexed paths. A new family affects program selection but need not alter the GPU `MaterialValues` ID.

## 4. Ownership and C++ API

Illustrative API (new proposal; these APIs are **not** present in the current repository):

```cpp
// Shared material; same userdata default for all inheriting instances.
world.SetMaterialUserData(baseMaterial, 0u);
world.SetMaterialShaderFamily(baseMaterial, MaterialShaderFamily::ProjectLit);

// One instance can be shared by many meshes.
MaterialInstanceHandle a = world.CreateMaterialInstance(baseMaterial);
MaterialInstanceHandle b = world.CreateMaterialInstance(baseMaterial);
world.SetMeshMaterialInstance(enemyA, a);
world.SetMeshMaterialInstance(enemyB, b);
world.SetMaterialInstanceUserData(a, 0x000000FFu);
world.SetMaterialInstanceUserData(b, 0u);

// Explicitly clear override to inherit subsequent base-material updates.
world.ClearMaterialInstanceUserDataOverride(a);
```

Recommended representation:

```cpp
struct MaterialRuntimeState {
    uint32_t defaultUserData = 0;
    MaterialShaderFamily shaderFamily = MaterialShaderFamily::StandardLit;
};

struct MaterialInstance {
    MaterialHandle base;   // stable base-material reference
    uint32_t userData = 0;
    bool overridesUserData = false;
};

uint32_t ResolveUserData(const MaterialRuntimeState& base,
                         const MaterialInstance* instance) {
    return instance && instance->overridesUserData
        ? instance->userData : base.defaultUserData;
}
```

For Tourmaline's cross-cluster asset ownership, `MaterialHandle` above represents an engine-owned stable locator, not necessarily a plain world-vector index. Use its established owner/handle convention. Material-instance handles should use the same generational semantics as other runtime entities: stale handles fail, an unloaded base does not silently bind an unrelated material, and destroying one instance must not destroy its shared base material.

Material instances **inherit** shader family, textures, PBR factors, alpha mode and raster policy. They override only the `uint32_t` value in v1. If two mesh entities share one material instance, updating it intentionally affects both. A mesh-private helper can create a private instance lazily, but should not be the only implementation of per-mesh data.

The resolved `userData` is consumed while constructing draw requests, preferably without GPU material reupload. A change becomes visible on the next frame/build. Avoid editing a draw buffer that may still be read by an in-flight GPU frame.

## 5. Shader family versus runtime userdata

A **shader family** is code identity; **userdata** is runtime value. This separation is fundamental:

| Scope | Determines shader family? | Controls userdata? | Expensive to change per frame? |
|---|---|---|---|
| Base material | Yes | Default | Family switch can change pipeline / batching |
| Material instance | Inherits | Optional override | No, data update only |
| Mesh entity | Uses its bound instance | Through bound instance | No |
| View | Not a material family | Own post-FX word | No |

Start with two families: `StandardLit` (built-in no-op hooks) and `ProjectLit` (project-supplied hooks). This is **not** a general runtime shader plugin registry. Add further families only when materially different HLSL implementations cannot reasonably share the project library.

Do not put `shaderFamily` inside `MaterialValues` as a fragment runtime branch. It is CPU pipeline-selection state. Do not allow v1 material instances to switch families independently of the base material: then identical material IDs inside one GPU-driven range could require different fragment pipelines and invalidate the current batching assumptions.

## 6. HLSL hook contract

Introduce one stable, small interface header in `engine/shaders/`:

```hlsl
// lit_hooks_api.hlsli
struct TMLitInput {
    uint userData;
    float2 uv;
    float3 worldPos;
    float3 normalWS;
    float time;
};

struct TMSurface {
    float3 baseColor; // linear, evaluated base color
    float metallic;
    float roughness; // perceptual roughness
    float3 emissive; // HDR scene-linear
};

void TM_ModifySurface(inout TMSurface surface, TMLitInput input);
void TM_ModifyLitColor(inout float3 hdrColor, TMLitInput input);
```

Two implementations, same function names/signatures:

```hlsl
// lit_hooks_default.hlsli — built-in no-op
void TM_ModifySurface(inout TMSurface s, TMLitInput i) {}
void TM_ModifyLitColor(inout float3 color, TMLitInput i) {}
```

And `game/shaders/game_lit_hooks.hlsli`, which defines the same functions using ordinary HLSL helper includes. This is **source-level library composition**; it does not imply linking an independently compiled SPIR-V library at runtime.

**Placement matters:**

- **Surface hook:** after texture/layer material evaluation and Forward+ decals, before BRDF-derived `F0`, specular AA and lighting; specifically in the lit branch after `ApplyForwardPlusDecals`. Re-clamp metallic/roughness to legal ranges after custom code and guard against non-finite values where appropriate. This v1 placement deliberately excludes the existing unlit early-return branch; no accidental promise that unlit/debug shaders honor hooks.
- **Lit-color hook:** after the lit color has been assembled and fogged, before final HDR sanitization/output. Preserve `outAoTerm`, `outEnvSpec` and alpha. It is appropriate for artistic tinting; it is **not guaranteed** to tint downstream SSR contributions or preserve all AO/SSR decomposition semantics. For effects requiring full post-composite consistency, use a dedicated later post-process pass instead.

Avoid providing raw `MaterialValues`, GPU pointer structures, direct descriptor tables, or the entire renderer push block as the hook's initial public ABI. The small structs encourage testable, portable effects.

No new `#define USE_DAMAGE`, `#define USE_WETNESS`, or `#if CUSTOM_MATERIAL_*` jungle. Functions call other functions. Existing `ALPHA_TO_COVERAGE`, mesh input and GI/SG build variants remain compile-time plumbing because they already represent materially different paths.

## 7. Building shader libraries without string replacement

Use tiny fragment **entry wrappers** and the same shared lit body:

```hlsl
// game/shaders/project_lit.frag.hlsl (schematic)
#include "lit_dependencies.hlsli"  // one-time engine helper for existing includes
#include "lit_hooks_api.hlsli"
#include "game_lit_hooks.hlsli"
#include "default_lit_body.hlsli"  // the actual shared PBR implementation
```

The engine's standard lit fragment wraps the same body but includes `lit_hooks_default.hlsli` instead. A2C and mesh equivalents have a few lines of entry-specific setup, then include the same hook library and body. Extract common dependencies and mesh PSIn declarations into small engine headers once; never duplicate the 1,500-line shader.

The game's CMake shader target compiles these wrappers with the engine and game shader include directories, using the existing DXC compile helper where suitable. Write output to a **distinct** project namespace (e.g. `content/shaders/project_lit.*.spv`) to avoid colliding with the engine shader target. The custom hook header and recursively included game headers must be build dependencies, otherwise changing a function may not rebuild the SPIR-V. Engine-only builds continue to compile the standard no-op family.

Prefer build-time SPIR-V with the existing DXC flags and ABI. Neither source-string rewriting nor runtime source compilation is necessary.

## 8. Getting the userdata to the fragment

1. CPU draw producer resolves the material's default/instance override into one `uint32_t` per draw, including cooked and dynamic scene draw paths that participate in v1.
2. **Direct VS:** place the value in the existing per-draw push scratch tail, tentatively at byte **244** (`padEnd_[1]`), while preserving the byte-240 dither flag. Audit all push users and use the `push_constants.yaml`/codegen checks; do not grow the 256-byte block or shift shared offsets.
3. **Filtered indexed VS:** propagate the value from the `gfx::Draw` ring through `gfxfiltered.vert.hlsl`. Adding a dedicated word to a 16-byte-aligned 96-byte `Draw` normally grows it to **112 bytes**; update the C++ and HLSL mirrors, buffer strides, upload/build/cull logic, and tests together.
4. **Mesh shader:** read the same `GfxDraw` word and forward it in the mesh vertex output, including correct initialization of any synthetic/default draw record.
5. **All lit fragment interfaces:** receive `[[vk::location(9)]] nointerpolation uint userData : TEXCOORD8;`. Every linked producer/consumer must agree on type, interpolation and location. Location 8 is already used.
6. Keep data as `uint32_t`/HLSL `uint`. Do not pass arbitrary bit patterns through numeric floats.

A separate packed `uint` draw-data buffer is a possible later optimization if the 16-byte padded increase becomes measurable; it is not automatically faster once indexing and additional buffer reads are counted. Benchmark before adding that complexity.

## 9. Program selection and permutations

`shaderFamily` enters CPU lit-program selection alongside the *existing* GI, SG-lobe, layered/standard, raster and A2C choices. Reuse the exact current program-selection logic where possible, but index module families first. Keep family selection at the same base-material granularity as the existing material ranges so instances with distinct userdata can still batch together.

Important requirements:

- **Direct, filtered and mesh paths:** use the same project's hook functions; do not render `ProjectLit` materials with standard code simply because one path was missed.
- **GI/SG permutations:** do not assume replacing a missing custom GI module with the stock module is visually correct. Ensure a semantically equivalent family-local fallback or reject that configuration at load/build time. Present implementation already has multiple GI/SG and `MAT_STANDARD` specialized modules; account for compile and pipeline cost rather than blindly multiplying everything.
- **A2C:** a project-lit masked material must use the correct A2C fragment implementation if the frame chooses A2C. The hook cannot simply `discard` in that variant and retain its early-Z advantage.
- **Mesh-path availability:** if a custom mesh fragment variant is unavailable, use the correct **custom indexed** variant when that fallback is supported; do not silently lose the project effect.
- **LOD:** the existing far vertex-lit shortcut does not execute the lit fragment body. Opt effect-bearing materials out of the shortcut initially, or explicitly document the missing effect. Quality specialization for materials staying lit may work unchanged.
- **Debug views:** preserve their intentionally distinct behavior. They are not guaranteed to call gameplay hooks.

For v1, cap the number of families to `StandardLit` and `ProjectLit`, and compile only variant combinations the engine supports and the project actually needs. Track pipeline counts and shader build time.

## 10. Coverage, shadows and other passes

A **color-only** hook changes shading, not visibility. That is the safe initial contract. A real dissolve/cutout or vertex deformation is more extensive:

- Main pass, masked alpha testing, and A2C must agree on coverage.
- Reversed-Z prepass and normal-Z shadow passes use separate alpha-tested shaders (`shadow_body.hlsli` and seed wrappers). A silhouette visible only in Main produces incorrect depth, culling, shadows, SSR and occlusion.
- Motion vectors and temporal reprojection may also need visibility parity.
- Mesh shader and indexed draw paths must select the same coverage definition.

Therefore **do not expose `TM_ModifyCoverage` in v1**. Add it only with a coherent cross-pass contract and test matrix. For the same reason, do not expose a general vertex deformation hook before handling bounds, culling, shadows and motion vectors.

Full-screen redout should be a separate, view-scoped post-processing feature. It cannot correctly be modeled as a material instance because there is no single mesh covering everything. Prefer a suitable composition point in the final view pipeline (taking AA, UI and HDR/LDR order into account).

## 11. Worked example A — independent enemy damage flash

Two enemies use the same base material and `ProjectLit` shader. Each has an independent material instance; only instance A flashes.

```cpp
uint32_t PackDamage(float amount) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    return static_cast<uint32_t>(std::lround(a * 255.0f));
}

world.SetMaterialShaderFamily(enemyMaterial, MaterialShaderFamily::ProjectLit);
auto matA = world.CreateMaterialInstance(enemyMaterial);
auto matB = world.CreateMaterialInstance(enemyMaterial);
world.SetMeshMaterialInstance(enemyA, matA);
world.SetMeshMaterialInstance(enemyB, matB);

world.SetMaterialInstanceUserData(matA, PackDamage(1.0f));
world.SetMaterialInstanceUserData(matB, PackDamage(0.0f));
// Each frame, fade A: 1 -> 0. No shader recompilation or PBR material copy.
```

```hlsl
// game/shaders/effects/damage.hlsli
void ApplyDamageFlash(inout float3 hdrColor, TMLitInput input)
{
    const float damage = float(input.userData & 0xFFu) / 255.0f;
    // Relative to HDR scene lighting, instead of a fixed low HDR value.
    const float3 flashScale = float3(3.0f, 0.15f, 0.15f);
    hdrColor *= lerp(float3(1, 1, 1), flashScale, damage);
}
```

Called by `TM_ModifyLitColor`. The effect is *surface-local*, including object shading and fog, but downstream SSR additions need not receive the same tint. For a deliberately physical result, a different version can modify `TMSurface.baseColor` and `TMSurface.emissive` in the surface hook. **The player's full-screen damage redout is not this example; use the separate view post-FX userdata word.**

Bit assignment here: `0..7 = damage`, `8..15 = wetness`, `16..23 = available`, `24..31 = available`. The engine never knows these names.

## 12. Worked example B — runtime wetness without new material assets

The same shader library can make one instance of a stone or metal material look wet, changing physically meaningful roughness and base color rather than simply recoloring the final pixel.

```cpp
uint32_t WithWetness(uint32_t bits, float amount) {
    const auto wet = static_cast<uint32_t>(
        std::lround(std::clamp(amount, 0.0f, 1.0f) * 255.0f));
    return (bits & ~(0xFFu << 8)) | (wet << 8);
}

uint32_t bits = 0;
bits = WithWetness(bits, 0.85f);
world.SetMaterialInstanceUserData(wetWallInstance, bits);
```

```hlsl
// game/shaders/effects/wetness.hlsli
void ApplyWetness(inout TMSurface surface, TMLitInput input)
{
    const float wet = float((input.userData >> 8) & 0xFFu) / 255.0f;
    surface.roughness = lerp(surface.roughness, 0.08f, wet);
    surface.baseColor *= lerp(1.0f, 0.72f, wet);
}
```

Called by `TM_ModifySurface` **after** Forward+ decals and **before** PBR lighting. The modified roughness affects highlights and environment response through the normal BRDF path. It does not add an instance-specific texture or change the base material's alpha/raster state. The effect is uniform across the entire mesh unless the HLSL function uses other pixel inputs (UV/world position) to create spatial variation.

Example composition:

```hlsl
#include "effects/damage.hlsli"
#include "effects/wetness.hlsli"

void TM_ModifySurface(inout TMSurface s, TMLitInput i) {
    ApplyWetness(s, i);
}
void TM_ModifyLitColor(inout float3 color, TMLitInput i) {
    ApplyDamageFlash(color, i);
}
```

Both effects run within one compiled `ProjectLit` family; the low 8 and next 8 userdata bits control them independently. No effect macros, source rewriting, or string lookup.

## 13. Implementation plan and affected code

**Phase A — baseline userdata and hooks**

1. Define runtime `MaterialInstanceHandle`, ownership and base/override resolution in the World/material layer. Start with runtime mesh entities; explicitly decide whether baked/static scene nodes can attach instances in this release.
2. Add `userData` to the draw request and CPU-to-GPU draw path. Update `gfx_filter.h`, `gfxfilter.hlsli` and any code that initializes/copies draw records. Preserve all existing fields and asserts.
3. Add the flat varyings to `default.vert.hlsl`, `gfxfiltered.vert.hlsl`, `gfxfilter.mesh.hlsl`, the lit body's default `PSIn`, and the mesh lit `PSIn`.
4. Audit the direct-path push scratch; add the CPU push of byte 244 alongside the existing byte-240 dither flag without breaking the generated push ABI.
5. Add typed hook structs, default no-op functions, and the two hook calls in the shared lit body. Ensure standard rendering still matches baseline.

**Phase B — project shader library/family**

6. Extract the small include glue needed for entry wrappers; do not refactor the PBR implementation at the same time.
7. Add project shader compilation with explicit engine+project `-I` paths and dependency tracking. Produce uniquely named SPIR-V files.
8. Add a bounded `MaterialShaderFamily` selector on base materials and module loading/program lookup in `WorldRenderer` / `GfxRenderer`, including indexed, mesh, A2C, GI and SG variants.
9. Verify family-by-material-range program binding, fallback behavior, material load/unload, and LOD selection. Ship the two examples above as small demonstration materials.

**Phase C — optional later functionality**

10. Extend families past `ProjectLit` only if real project use cases require distinct code.
11. Add coverage hooks only as a coordinated Main/depth/seed/shadow/motion change.
12. Introduce richer material-instance PBR overrides or custom texture bindings only after measuring a demonstrated need.

## 14. Verification and acceptance criteria

- **No regression:** ordinary `StandardLit` looks and profiles the same with no-op hooks; debug views, `MAT_STANDARD`/layered materials and unlit materials remain unchanged.
- **Isolation:** two enemies with the same base material and shader family can have different userdata; two meshes sharing one material instance update together; clearing the override re-enables inheritance.
- **Stage correctness:** direct, filtered indexed and opaque mesh paths display the same userdata value; location 9 is explicitly declared in every stage; shader linkage validates cleanly.
- **GPU ABI:** `sizeof(gfx::Draw)` and HLSL stride agree (expect 112 bytes with simple dedicated-word design); C++/HLSL draw data, upload lengths, readback/tests and synthetic initializers are coherent. Push constants remain 256 bytes and generated pre-192 offsets remain unchanged.
- **Effect stability:** flash and wetness work under GI variants, alpha-tested/A2C where supported, and different lighting/exposure conditions; no NaN/Inf pollution.
- **Batching:** material instances do not cause different base `MaterialValues` IDs or pipelines; same-family instances remain batchable.
- **Failure handling:** missing custom modules are reported visibly rather than silently dropping custom effects; a mesh path can use the custom indexed fallback if compatible.
- **Performance:** compare shader compile time, variant/pipeline count, CPU draw-build time, GfxDraw upload bytes, draw calls, and Main pixel cost to baseline. The 96-to-112-byte draw stride is a documented tradeoff, not a negligible assumption.

## 15. Decisions and alternatives

| Decision | Adopt | Reject for v1 | Rationale |
|---|---|---|---|
| Shader composition | HLSL headers + fixed ordinary functions | Reflection, replacement, runtime linking | Stable, DXC-native, simple |
| Implementation scope | Small typed surface/color hooks | A second lit shader body | Preserves one PBR implementation |
| Runtime data | Single `uint32_t` per resolved draw | Named dynamic parameters | Predictable CPU/GPU ABI |
| Material-instance sharing | Base material + value override | 128-byte GPU material duplication | Cheap and batch-friendly |
| Family selection | Base material, compile-time modules | Per-instance dynamic shader identity | Respects material-range pipeline grouping |
| Early coverage | Not extensible in v1 | Main-only dissolve discard | Prevents depth/shadow inconsistency |
| View damage | Separate full-screen post effect | Material-instance hack | Scope matches pixels affected |
| GPU transport | Flat varying, dedicated draw word | Float-encoding arbitrary bits | Exact bit preservation |

## 16. Bottom line

The useful primitive is **one 32-bit runtime value** delivered reliably to an **ordinary HLSL library** included into the existing shared lit implementation. The shader library does not own rendering; it changes a small set of typed inputs at deliberate points. Materials choose the compiled family, material instances choose the runtime data, and the view handles screen-space effects. The first release should ship with a no-op standard family, one project lit family, `TM_ModifySurface`, `TM_ModifyLitColor`, and the damage/wetness demos. Do **not** turn this into a general material framework before a real project needs one.

### Code reviewed

- [Shared lit body](https://github.com/striker-samuel/tourmaline/blob/master/engine/shaders/default_lit_body.hlsli)
- [Existing shader entry wrapper](https://github.com/striker-samuel/tourmaline/blob/master/engine/shaders/default_lit.frag.hlsl)
- [Mesh fragment wrapper](https://github.com/striker-samuel/tourmaline/blob/master/engine/shaders/gfxmesh_lit.frag.hlsl)
- [Material definition](https://github.com/striker-samuel/tourmaline/blob/master/engine/include/Engine/Renderer/mesh.h)
- [GPU draw structure](https://github.com/striker-samuel/tourmaline/blob/master/engine/include/Engine/Renderer/gfx_filter.h)
- [GPU draw HLSL mirror](https://github.com/striker-samuel/tourmaline/blob/master/engine/shaders/gfxfilter.hlsli)
- [Direct vertex shader](https://github.com/striker-samuel/tourmaline/blob/master/engine/shaders/default.vert.hlsl)
- [Filtered vertex shader](https://github.com/striker-samuel/tourmaline/blob/master/engine/shaders/gfxfiltered.vert.hlsl)
- [Mesh shader](https://github.com/striker-samuel/tourmaline/blob/master/engine/shaders/gfxfilter.mesh.hlsl)
- [Shadow/depth shader body](https://github.com/striker-samuel/tourmaline/blob/master/engine/shaders/shadow_body.hlsli)
- [Shader build helper](https://github.com/striker-samuel/tourmaline/blob/master/cmake/CompileShaders.cmake)
- [Shader variants](https://github.com/striker-samuel/tourmaline/blob/master/engine/CMakeLists.txt)
- [Push constants IDL](https://github.com/striker-samuel/tourmaline/blob/master/tools/push_constants_idl/push_constants.yaml)
