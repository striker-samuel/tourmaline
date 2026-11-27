# DDGI correctness validation

This patch is independently written against Tourmaline's existing shader and render-graph contracts. No NVIDIA RTXGI SDK source, binaries, or dependencies are incorporated. The SDK's public repository is a conceptual reference only; its license is not an MIT license.

## History contract

Each per-probe irradiance record has 64 directional float4 values plus one float4 storing the ray-origin offset for the corresponding history slot. Distance moments keep their existing layout. Carry history only when both the lattice coordinate and the offset match; new or moved probes trace immediately and start with fresh measurements. SDF uses zero offset, and reset suppresses previous-history feedback.

## Validation

Run `python -m unittest discover -s tests -p test_ddgi_contract.py` for CPU contract checks. Build shader assets and run the normal engine tests on both supported platforms. These tests do not replace rendering on a ray-query capable GPU.

Manual GPU checks:
- Constant sky, no geometry: compare probe irradiance with pi times incident radiance; inspect the octahedral seams.
- A thin wall separating bright and dark rooms: inspect distance moments, probe validity, leakage and black speckling.
- A probe inside a solid: check relocation stays within its cell; when no nearby exit exists, verify invalid probes do not retain old light.
- Move the camera by one cell and then teleport; newly exposed probes must not inherit adjacent visibility. Check vertical and horizontal cascade boundaries.
- Change coarse spacing; SDF feedback must not sample old history with the new spacing.
- On a non-ray-query device, exercise the isolated SDF path with a covered scene; no TLAS pipeline should be created or dispatched.

Remaining work: hardware hit lighting still uses Tourmaline's existing compact material/sky approximation, not per-hit normals and shadowed direct lighting; full multi-bounce hardware RT and GPU image validation are outside this patch. Probe relocation remains a six-axis heuristic, not a full classification/relocation solver.
