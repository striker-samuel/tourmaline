# Experimental descriptor-backed table publisher (NO BDA)

**Status: opt-in experimental / not a renderer backend switch.**

This stack adds a buffer-table publication seam which does not request
`GetBufferDeviceAddress` and does not change bindless-texture behavior.

- `smvk::table::DescriptorTableMirror<T,N,Buffer>` holds one buffer per
  flight; it grows in 64-KiB row-aligned quanta, replays all rows on
  reallocation, and uploads only dirty payload runs otherwise.
- Failed writes retain the table's pending state. An uninitialized or
  failed flight exposes no binding, and a flight with UNPUBLISHED edits
  also exposes no binding (`GetBinding` rejects while `PendingRows(flight)`
  is nonzero) — the binding is the GPU-published snapshot, never a mix of
  stale and current rows. All flights must converge independently.
- `DescriptorStoragePublisher<T,N>` adapts that to
  `ResourceManager::CreateBuffer(BufferType_Storage)` and
  `UploadDataToBuffer`. `Publish` returns an explicit `State` (`Queued` /
  `Failed`) and takes the destination stage/access mask (default
  compute + vertex + fragment — lighting/decal/material tables are
  fragment-consumed); `Flush` submits the staged copies. The application
  supplies the descriptor-set binding and appropriate render-graph
  storage-buffer hazards.
- Buffer retirement is through existing `Holder<BufferHandle>` lifetime.
  Publication must run only on a fenced, acquired flight, before consumers.

## What this deliberately does NOT do

- No no-bindless-texture fallback, sampler array rewrite or descriptor
  indexing replacement.
- No shader conversion: `GPU_PTR(T)` is still `vk::BufferPointer<T>`
  and `.Get()` dereferences require BDA in the production shader path.
- No Vulkan descriptor set layout, pool, writes, rebinds, or pipeline
  variants are created here. The returned binding is a **descriptor input**,
  not a complete descriptor backend.
- No change to the existing GPU Scene uploader, materials, light/decal/probe
  producers, or shaders.
- While the publication API does not require BDA, the existing Vulkan RHI
  might still enable the BDA device feature globally. This PR does not
  demonstrate operation on a device without that feature.

## Follow-up acceptance for a true non-BDA renderer

1. Add descriptor-set layouts, allocation and lifecycle in the RHI.
2. Introduce shader load/store accessors for a selected consumer instead
   of only the existing `GPU_PTR` declaration macros.
3. Compile and validate a descriptor-bound shader variant without BDA,
   then run A/B output and lifetime tests on a supported Vulkan device.
4. Preserve the existing texture bindings unchanged throughout.
