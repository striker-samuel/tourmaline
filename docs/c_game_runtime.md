# C game-runtime and catalog prototype

Tourmaline is still a C++20 engine. This is a **C ABI at the gameplay
boundary**, not a rewrite of the engine or a second asset database.

The canonical ProjectCatalog (built from assets.db / the BuildPlan) now
generates TWO bindings beside the project recipe:

- project.h: unchanged C++20 typed keys, constexpr queries, node schema
  accessors, and runtime catalog table.
- project_c.h: C11-compatible stable IDs, an asset table, entry scene,
  package list, audio cues, and the SAME CatalogFingerprint.

Project.exe ids / build write both with write-only-on-change semantics.
The source of truth for identity and the fingerprint remains the
existing ProjectCatalog and appmanifest code. Neither binding provides
a separate namespace for asset allocation.

## A minimal C consumer

```c
#include "project_c.h"
#include <RuntimeC.h>
#include <stdio.h>

int main(void) {
    char err[512];
    tour_c_runtime* rt = tour_c_runtime_open("game.tapp",
                                           err, sizeof err);
    if (!rt) { fprintf(stderr, "%s\n", err); return 1; }

    if (!tour_c_runtime_attach_catalog(rt, &tour_project_catalog,
                                       err, sizeof err)) {
        fprintf(stderr, "catalog mismatch: %s\n", err);
        tour_c_runtime_close(rt);
        return 1;
    }

    tour_c_bytes data = {0};
    if (tour_c_runtime_read_asset(rt, TOUR_PROJECT_TEXTURE_stone,
                                  &data, err, sizeof err)) {
        // Interpret the bytes using your own code.
        tour_c_bytes_free(&data);
    }
    tour_c_runtime_close(rt);
    return 0;
}
```

Link the consumer against tourmaline_apphost (a C++ static library);
the final native link must include its C++ runtime and its engine
dependencies. A game written in C does not need to include C++ headers.

## ABI details

- Asset kinds are the deployed numeric values (0=Nil, 1=Scene ...).
- Catalog strings are borrowed, NUL-terminated, UTF-8 paths; the
  generated header retains them for the executable's lifetime.
- The C API copies row data into ProjectRuntime's internal
  CompiledAssetDesc so no std::string_view layout crosses the ABI.
- The content gate checks the actual deployed registry, by fingerprint
  and row identities, using the existing C++ validation.
- Runtime-loaded bytes are owned by the C bridge; free them with
  tour_c_bytes_free, not with the caller's allocator.
- Diagnostics are caller-allocated, always NUL-terminated if capacity
  is nonzero. C functions return 1 on success, 0 on failure; open
  returns a null pointer on failure. C++ exceptions are caught.
- C ABI v1 is a prototype, not a binary-compatibility guarantee yet.

## Deliberate boundaries of this vertical slice

This exposes packaged content **loading, ID-based reads, and catalog
validation**. It does not yet expose World rendering, gameplay tick
or callbacks, typed node-schema decoding, asset queries equivalent to
C++ consteval MatchAssets, or GPU/input/audio APIs.

The next useful layers would be opaque World/Node/Resource handles
(with generational validity rules) and a C callback-based game host.
These should be designed from real consumer needs; do not translate
C++ classes into a 1:1 public ABI just because they exist.
