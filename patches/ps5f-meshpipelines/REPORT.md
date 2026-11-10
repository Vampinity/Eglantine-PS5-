# PS5F mesh graphics pipelines

## Root cause

Ocarina's native task/mesh graphics path is entered by `PipelineCache::CurrentGraphicsPipelineSlowPath`. Runtime creation translates the guest programs in `CreateGraphicsPipeline`, then constructs the Vulkan graphics pipeline. Before this fix, the graphics pipeline's final build was queued on the ordinary Vulkan pipeline workers, but the mesh path was still treated like a normal runtime pipeline: the GPU thread performed the runtime creation work and the rasterizer could receive the not-yet-built object. A mesh draw then returned `nullptr` from `BuiltPipeline`/`CurrentGraphicsPipeline`; `PrepareDraw` reported `Draw skipped (no pipeline)`. With foliage driven by those mesh draws, vegetation disappeared.

Patch 0340 uses the same single `deferred_workers` queue introduced by 0330 for AMD/RADV mesh graphics pipelines. Runtime mesh builds use that worker and are marked `skip_until_built`; the GPU thread never waits for the deferred build and the rasterizer skips only until the pipeline is ready. `EDEN_NX2_DEFER_MESH_PIPELINES=0` is the rollback A/B switch; `=1` forces it on. The default is on for AMD proprietary, AMD open-source, and Mesa RADV drivers.

## Why the mesh cache was dropped

The per-game cache was already written to `vulkan_mesh.bin` with the mesh registers and pass-through geometry header. However, `LoadDiskResources` opened that file only when `VK_EXT_mesh_shader` was supported:

```cpp
if (device.IsExtMeshShaderSupported()) {
    LoadPipelines(..., mesh_pipeline_cache_filename, ..., true);
}
```

The PS5 RADV configuration uses the mesh-as-compute fallback when the native mesh extension is absent. Its runtime mesh pipeline was therefore saved, but the next launch never read `vulkan_mesh.bin`; `disk_pipelines` did not contain the entry, so the same mesh key was rebuilt again. Patch 0341 loads the mesh cache when either native mesh shaders are supported or the valid mesh-compute scratch buffer is active. The static Vulkan pipeline cache remains disabled as intended by 0010; this changes only the per-game mesh cache.

## Changes

- `0340-vulkan-defer-mesh-graphics-pipelines.patch`
  - adds the AMD/RADV mesh deferral switch;
  - routes runtime task/mesh graphics builds to 0330's deferred worker;
  - prevents an unfinished deferred mesh pipeline from being handed to the rasterizer.
- `0341-vulkan-load-mesh-cache-with-compute-fallback.patch`
  - loads `vulkan_mesh.bin` for the PS5 mesh-as-compute fallback even without `VK_EXT_mesh_shader`.
- `0342-vulkan-fix-mesh-cache-diagnostic.patch`
  - stops the runtime-pipeline report from labelling a mesh key as "not in the disk cache" when the key was actually loaded but deferred at boot.

No rasterizer fallback was added: a draw is skipped only while its deferred pipeline is unavailable, then retries normally. No static pipeline cache was re-enabled, and 0301 was not re-added.

## Predicted effect

- The long mesh graphics build no longer blocks the GPU thread; affected draws may be absent briefly while the worker compiles, rather than stalling the frame thread.
- Once ready, the same pipeline is reused in the run.
- On the next launch, the valid per-game `vulkan_mesh.bin` entry should be loaded instead of rebuilding the same mesh pipeline, removing the repeated multi-minute compile and allowing the vegetation draws to execute.

## Console measurement

No PS5 console/debug build is available in this environment, so no runtime number is claimed here. The owner should compare two launches with the same title cache and capture:

1. `Runtime pipelines in the last 10 s: ... ( ... mesh N)` and each `Graphics pipeline built on the GPU thread ... mesh, not in the disk cache` line.
2. `Draw skipped (no pipeline)` counts during the vegetation scene.
3. The cache-load `Total Pipeline Count` and the presence of the mesh entry in `vulkan_mesh.bin`.
4. A/B `EDEN_NX2_DEFER_MESH_PIPELINES=0` versus `=1`; restore the default/on setting after the comparison.

Expected result is a first-run deferred build with no GPU-thread driver stall, followed by a second-launch cache hit and no repeated `no pipeline` skips after the worker finishes. The actual wall-clock compile time, skipped-draw count, and second-launch hit must come from the console log.

## Verification

The patch stack was applied in a detached worktree at core commit `0ca2032bff`: all 257 `tools/eglantine-patches/0[0-2]*.patch` files, then 0330, then 0340, 0341, and 0342. `git apply --check` passed for the new patches in that clean verification worktree. A full build/test was not run because this Windows environment has no Linux/WSL build path, as specified by the task.
