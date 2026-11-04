# [EXPERIMENT / DO NOT MERGE] smvk split

Upstream: https://github.com/striker-samuel/smvk

This change introduces **only** a proposed explicit vendor-sync workflow.
It does not change production linking or silently switch the engine to a
partial extraction.

The smvk draft upstream currently contains API/RHI/rendergraph/source samples,
but is **not** a standalone buildable library. Its Vulkan implementation,
dependency decoupling and CMake are still outstanding.

## Proposed adoption when upstream is ready

1. Finish standalone `smvk` CMake, Windows/Linux builds and triangle/blur/fuzz tests.
2. Review and pin a full upstream commit SHA.
3. Run `python tools/sync_smvk.py --rev <full_sha>` for a dry run.
4. Run `python tools/sync_smvk.py --rev <full_sha> --apply` to copy the snapshot.
5. Integrate `add_subdirectory(external/smvk)` and link `engine` to `smvk`
   in a separate, reviewable commit after removing duplicate symbols/source.
6. Verify full Tourmaline build and functional renderer tests on both platforms.

No git submodule is required; vendored code is committed as ordinary files.
This PR must remain draft and unmerged until all integration steps are tested.
