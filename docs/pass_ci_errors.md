# Passing CI: past errors and how to avoid them

Read this before pushing C++/shader changes, changing a test contract, or fixing a
red workflow. This is a troubleshooting guide, not permission to bypass a gate.
[AGENTS.md](../AGENTS.md) and the current workflow definitions remain authoritative.

## What was reviewed

The last 300 commits reachable from master at
[3b29da016c3b](https://github.com/striker-samuel/tourmaline/commit/3b29da016c3ba79f9109800ed073715f23c542da),
through [de1c28e76715](https://github.com/striker-samuel/tourmaline/commit/de1c28e76715),
inclusive, using GitHub's commit-list order (including merge ancestry, not just
first-parent history). Commit subjects were screened, and 17 relevant build,
test, formatting and fixture-fix patches were inspected. The cases below are
selected reusable lessons, not an exhaustive count of failed CI runs; historical
workflow logs were not audited. Commit links show the actual changes, not
necessarily a confirmed successful CI run.

## 1. Reproduce the correct gate

| Gate | What it checks |
| --- | --- |
| [CI](../.github/workflows/ci.yml) | Full non-test build, Release, BUILD_TESTING=OFF, GCC 14 on Ubuntu 24.04 and MSVC x64 on Windows. |
| [Tests](../.github/workflows/tests.yml) | Independent Release build with BUILD_TESTING=ON and CTest on both platforms; also DCC Python tests. |
| [Clang Tests](../.github/workflows/clang-tests.yml) | Independent Clang 18 Release build and headless CTest on Ubuntu 24.04. |
| [Perf scenario catalog](../.github/workflows/perf_scenarios.yml) | Path-filtered Python harness and catalog checks, no GPU. |
| [Agent CLI tools](../.github/workflows/agent_cli_tools.yml) | Path-filtered Python package, project, capture, pixel and input CLI tests on Linux/Windows. |
| [Text integrity](../.github/workflows/text_integrity.yml) | UTF-8 regression fixtures and tracked-tree scan. |

A green build-only CI says nothing about CTest. Start with the first failed
step and diagnostic, not the last cascade of errors. Match the compiler, build
type, dependency pins and test environment of the failing workflow. Use a
separate build directory for each compiler. After a targeted fix, build the
full configuration and run the relevant suite; a test that was never built or
registered does not count as passing.

Example on Linux, after recursive submodule checkout, installing the workflow's
system packages, running ./install_dependencies.sh and installing KTX at ~/ktx:

~~~sh
cmake -S . -B out/build/ci -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
  -DKTX_ROOT="$HOME/ktx" -DBUILD_TESTING=OFF
cmake --build out/build/ci

cmake -S . -B out/build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
  -DKTX_ROOT="$HOME/ktx" -DBUILD_TESTING=ON
cmake --build out/build/tests
mkdir -p .ci-content
export TOURMALINE_CONTENT_DIR="$PWD/.ci-content"
ctest --test-dir out/build/tests --output-on-failure \
  -E '^(retirement_fuzz_tests|world_fields_gpu_tests|placement_gpu_tests|probepremix_gpu_tests|rhi_upload_barriers_gpu_tests)$'
~~~

For Clang, use clang-18/clang++-18 and out/build/clang-tests instead.
For Windows, use the x64 MSVC developer environment and the dependency/setup
steps in tests.yml (including KTX_ROOT and EMBREE_PREBUILT_DIR); use the
PowerShell installer, not the Linux script.

The five excluded tests require a real SDL window and Vulkan device; run them
on a suitable developer machine when changing those systems. Do not extend
the exclusion list merely to get green CI. A software Vulkan failure is not
automatically a renderer portability bug. Report GPU validation separately.
The Linux workflow captures a PakHostTests backtrace on a CTest failure; check
the original failed test name before interpreting that diagnostic.

## 2. Repeated build failures

### ShaderProgram initialization: avoid braces for this specific type

**Symptom:** GCC build errors/internal compiler errors around resetting a shader
program; MSVC may compile the same code.

**Fix:** use the existing paren/default-init convention in
engine/include/Engine/smvk.h:

~~~cpp
program = smvk::ShaderProgram();
smvk::ShaderProgram localProgram;
~~~

Do not introduce ShaderProgram{}, a bare brace assignment to a ShaderProgram,
or a brace-initialized local of that type. Its array member has default member
initializers; the header documents GCC 13's gimplify ICE and the workaround.
The problem recurred in DDGI trace/blend teardown and screen-space-probe backend
switching. Do not generalize this workaround to unrelated handles or aggregates.

Evidence: [3b38753bc058](https://github.com/striker-samuel/tourmaline/commit/3b38753bc058),
[2c65a4da0d95](https://github.com/striker-samuel/tourmaline/commit/2c65a4da0d9516c94629b5bdcfff48b65a821ee6).

### Missing includes, PCH masking and standalone test targets

**Symptom:** an unknown type/function on Linux or in header_smoke, despite a
working editor build; a standalone CPU-only test cannot find Core headers.

**Fix:** each header includes what it uses, without relying on engine PCH or
include order. baked_placement.h needed <string>; sdf.h needed
<glm/common.hpp> for glm::clamp and <glm/vec2.hpp>. HeaderSmoke compiles each
public header in its own generated TU without PCH. GCC force-includes the
engine PCH rather than using the MSVC/Clang PCH mechanism; this does not make
transitive dependencies safe. For independent test executables, provide the
correct target include paths (SmvkRawTableTests and SmvkDescriptorTableTests
needed core/include in addition to engine/include and external/glm).
Do not fix this by adding a blanket include to the PCH or linking unnecessary
renderer dependencies into CPU-only tests.

~~~sh
cmake --build out/build/tests --target HeaderSmoke
ctest --test-dir out/build/tests --output-on-failure \
  -R '^(header_smoke|smvk_raw_table_tests|smvk_descriptor_table_tests)$'
~~~

Build the named test executables first if using a targeted rather than full build.

Evidence: [fc6a07d0be59](https://github.com/striker-samuel/tourmaline/commit/fc6a07d0be5918ff54d1faad1bb87db16e7617d0),
[faa39d6304ab](https://github.com/striker-samuel/tourmaline/commit/faa39d6304abe572071b6742d126720c8628729d),
[b8464e901cc3](https://github.com/striker-samuel/tourmaline/commit/b8464e901cc3cf2a7df46481096f9b204ad1095f).

### Namespace qualification and ownership types

MSVC may accept redundant qualification on a definition inside its owning
namespace; GCC rejects it. Inside namespace smvk, define ErrorName,
ErrorDescription and FromVkResult without smvk:: on their names. Qualify
callers or definitions outside the namespace as required. After a namespace
migration inspect definitions as well as call sites.

When comparing or storing a non-owning pointer to an object owned by
std::unique_ptr, use .get() where that is the intended identity; do not change
ownership to satisfy a comparison. EditorApp::UploadMaterials had to use
m_world.get() for both its comparison and assignment.

Evidence: [ef45baa337b1](https://github.com/striker-samuel/tourmaline/commit/ef45baa337b11e388f025d3ecc11f03aa8297f9a),
[adad6372df88](https://github.com/striker-samuel/tourmaline/commit/adad6372df88266d61635c4b6b0e6972430b2300).

### printf/fprintf format types must actually match

GCC -Wformat found mismatches in renderer diagnostics and the GI lightmap
repacker. ShaderProgram::shaderCount is size_t: use %zu, not %d. When using
%llu, pass an unsigned long long (the lightmap area/inflated arguments were
explicitly cast). For fixed-width integers use appropriate <cinttypes> format
macros or a matching explicit conversion. Windows and Linux differ in the
underlying types of some integer aliases. Check every argument when changing
a format string; do not silence warnings globally.

Evidence: [3b29da016c3b](https://github.com/striker-samuel/tourmaline/commit/3b29da016c3ba79f9109800ed073715f23c542da).

## 3. Repeated stale-test failures: change the contract and its tests together

The architecture, assets and rendergraph tests deliberately pin invariants.
A test-only edit is justified only when the intended contract changed and
the new value has been checked against production code. Otherwise fix the
implementation. Do not replace exact checks with loose ranges or remove a
failing assertion merely to pass CI.

| Area / test | Historical fix and prevention |
| --- | --- |
| architecture_tests | R7 compatibility exports and RHI exposure inventories are exact both ways. Placement, GI and renderer changes left stale rows; later FrameContext's World forward-declaration/re-export and SDF exclusion APIs did so again. Fix accidental boundary leakage first. For deliberate changes update only the affected counts/rows with the reason; shrink/delete rows when removing exposure. |
| rendergraph_contract_tests | RayTracingInstance grew from 80 to 112 bytes. Update the exact size assertion alongside an intentional ABI change, and independently verify offsets (transform stayed at 16 in this fix), alignment and every consumer. |
| asset_tests / serialized blobs | World manifest header expectation changed from version 1 to 2 and the SMP9 meshlet trailer from 12 to 16 bytes (maxLocalVertex). Check magic, version, offsets, exact end and writer/reader agreement together. Historical numbers are examples, not instructions to bump format versions; follow current AGENTS.md format policy. |
| asset_tests / LOD | Sticky/refine hysteresis tests assumed the old implicit default. The selector is opt-in; pass kLodSelectHysteresis explicitly when testing sticky behavior and retain the stateless default case. |
| gpu_scene_world_tests | The RegisterNativeIndexedRange output flag describes hasNativeSuffix, not the inverse of task/mesh compatibility. Initialize and assert the actual fact, including conflict/fallback cases. Rename misleading test locals. |
| retirement_fuzz_tests | A zero-vertex imported mesh is tolerated and returns a handle, not a failed upload batch. Assert the full handle-list size, skip empty meshes in the surface model, and preserve all-or-nothing rollback coverage for heap exhaustion. Run on GPU-capable hardware. |

Evidence: [982d05690bc1](https://github.com/striker-samuel/tourmaline/commit/982d05690bc14f87bfcee678b9203cb3630bad61),
[faa39d6304ab](https://github.com/striker-samuel/tourmaline/commit/faa39d6304abe572071b6742d126720c8628729d),
[2c65a4da0d95](https://github.com/striker-samuel/tourmaline/commit/2c65a4da0d9516c94629b5bdcfff48b65a821ee6),
[64425e08208b](https://github.com/striker-samuel/tourmaline/commit/64425e08208bfa0be3e528763f5af652afdc4d28),
[79f2c3f4a620](https://github.com/striker-samuel/tourmaline/commit/79f2c3f4a620e9ee3122e184b2b64d9f079efe49).

For CPU/HLSL ABI work also follow [GPU struct layout tests](gpu_struct_layout_tests.md)
and run the push-constant generator check:

~~~sh
python tools/push_constants_idl/codegen.py --check
ctest --test-dir out/build/tests --output-on-failure \
  -R '^(architecture_tests|rendergraph_contract_tests|push_constant_tests|gpu_struct_layout_tests|asset_tests|gpu_scene_world_tests)$'
~~~

## 4. Fixtures, portability and real runtime bugs

### Match platform-independent behavior, not a platform-specific message

The dependency-stamp test expected install_dependencies.ps1 on Linux, where
the diagnostic names install_dependencies.sh. Match the shared
install_dependencies stem while still checking the stale dependency and both
pins; if the extension is important, branch on the platform. Do not disable
dependency-stamp enforcement to get green CI. Refresh dependencies through the
platform installer.

Evidence: [345a89d00032](https://github.com/striker-samuel/tourmaline/commit/345a89d000324abc92b13df018da6dc0647e4d06).

### Numerical tolerances must follow the representation

The stereo doublefloat test exercises a snapped cell edge: a float32 relative
view translation near 4096 m has an ULP of roughly 0.0005 m. Its 0.0002 m
tolerance was unrealistically tight for the derived view. The fix allowed
0.002 m for float-derived eye deltas and projected coordinates but kept the
double world-position/IPD check at 0.0002 m. Preserve the stronger invariant
at its authoritative precision. Do not broadly inflate epsilon; explain the
error budget and test boundary cases.

Evidence: [345a89d00032](https://github.com/striker-samuel/tourmaline/commit/345a89d000324abc92b13df018da6dc0647e4d06).

### Keep synthetic benchmark data on the correct side of policy thresholds

GPU busy policy was changed from 5% to 15% in tools/benchmark_environment.py.
The perf-harness NVIDIA and Windows fixtures still supplied 7% and 12%
respectively while expecting a busy classification. The follow-up raised
them to 16% and 18%. Update fixtures with the policy, include below/at/above
boundary cases, and do not lower the actual busy threshold just to satisfy
the old test. Check both parsers.

Evidence: [ecb825e95240](https://github.com/striker-samuel/tourmaline/commit/ecb825e952408132090b9deaace53adb63e73594),
[fc6a07d0be59](https://github.com/striker-samuel/tourmaline/commit/fc6a07d0be5918ff54d1faad1bb87db16e7617d0).

### Check real serialized bytes and malformed-input fixtures

The lightCookieBake serializer emitted literal backslash+n instead of a
newline; malformed-record tests repeated that error. Numeric parsing also
compared the strtoul/strtof end pointer against an overescaped character
literal instead of the NUL terminator. In C++ use "\n" for the real record
separator and '\0' for end-of-string, not "\\n" and '\\0'. Verify successful
round-trips, then make malformed fixtures syntactically valid except for the
specific invalid field (e.g. traversal, zero samples, non-power-of-two
resolution). A negative test rejected for the wrong reason is a false pass.

Evidence: [ec49f204e92d](https://github.com/striker-samuel/tourmaline/commit/ec49f204e92d71c3695d1390dc84936c3d35a0ac),
[cd94b08057f2](https://github.com/striker-samuel/tourmaline/commit/cd94b08057f2e8af23ae925d961941894aa52d9c),
[982d05690bc1](https://github.com/striker-samuel/tourmaline/commit/982d05690bc14f87bfcee678b9203cb3630bad61).

### Failed writes must not leave partial artifacts

PakWriter opens the streaming output eagerly. A failed cook could leave a
partial pak, violating corrupt-source tests. The fix introduced an RAII
cleanup guard in tools/pak_cooker/main.cpp, declared BEFORE the writer so
the writer closes first (important on Windows). Remove the failed output
on uncommitted exits and disarm the guard only after successful completion.
Preserve the failure assertion; fix the producer, not the test.

Evidence: [345a89d00032](https://github.com/striker-samuel/tourmaline/commit/345a89d000324abc92b13df018da6dc0647e4d06).

### Distinguish a missing capability from a broken enabled path

A hard-error task/mesh gate incorrectly treated a device without mesh shaders
as an invalid enabled path. The fix gates on both the request and
smvk::SupportsMeshShaders(); TOURMALINE_MESH=0 exercises the intended indexed
fallback. Keep fail-closed validation for genuinely enabled but invalid paths,
and test the unavailable-capability fallback separately.

Likewise, a GPU-scene test exposed gameplay-owned nodes missing from the
page-derived draw list. RefreshRuntimeSceneCaches now appends renderable
gameplay-owned nodes under stable identities; fixing only the test expectation
would have hidden missing geometry.

Evidence: [ecb825e95240](https://github.com/striker-samuel/tourmaline/commit/ecb825e952408132090b9deaace53adb63e73594),
[64425e08208b](https://github.com/striker-samuel/tourmaline/commit/64425e08208bfa0be3e528763f5af652afdc4d28).

## 5. Before requesting review

- New C++ tests need an executable AND add_test in tests/CMakeLists.txt,
  correct target dependencies/include paths and runtime staging as required
  (follow the existing plain-executable conventions in AGENTS.md). A follow-up
  was needed to register WorldChannelsTests; verify discovery with
  ctest --test-dir out/build/tests -N.
  Evidence: [a167cae3c851](https://github.com/striker-samuel/tourmaline/commit/a167cae3c85189f5bca4b2475dbc32836f3c4e20).
- Apply repository clang-format to changed C++ files, including after merges.
  A material-publication merge needed a formatting-only follow-up
  ([a8cc2c878823](https://github.com/striker-samuel/tourmaline/commit/a8cc2c87882389157d2d43820f8b00f78ac4a7fe)).
  The local pre-commit configuration uses clang-format -i --style=file.
- Keep files UTF-8, without introducing mojibake or changing legitimate
  Unicode. Run the text integrity suite rather than a global replacement.
- For Python tools, run the corresponding suites before pushing; do not assume
  path-filtered workflows will run for unrelated changes.

Useful quick checks (repository root; use python3 if needed):

~~~sh
python -m unittest discover -s tests -p 'test_text_integrity.py' -v
python -m unittest discover -s tests -p 'test_dependency_stamps.py' -v
python -m unittest discover -s tools/tests -v
python tools/perf_harness.py list
python -m unittest discover -s tools/dcc -p 'test_*.py' -v
~~~

For focused C++ regressions after building the tests:

~~~sh
ctest --test-dir out/build/tests --output-on-failure \
  -R '^(tour_tests|doublefloat_tests|pak_host_tests|dependency_stamp_tests|perf_harness_tests|world_channels_tests)$'
~~~

Finally, record the actual compiler/configuration, commands and results in
the PR. Distinguish tests run, tests skipped for hardware/dependency reasons,
and CI checks still pending. If a fix is likely to recur, add a short entry
here with its symptom, root cause, preventive check and commit link.
