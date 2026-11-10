# Cross-cutting audit of the 307-patch PS5 graphics series

Scope: read-only audit of `C:/Users/horac/lichii-ally/wt-gate` against the original 307 patches in `tools/eglantine-patches/`. Owner-supplied corrections 0436-0438 appeared during the audit and are excluded from findings as instructed. No build was run.

## Ranking

Findings are ordered by likely impact on the next compile, then by confidence. Already-fixed items called out by the owner are intentionally omitted.

## 1. Anchor pre-check

**No new anchor problem found.** Running every `adapt()` anchor against the fully patched tree found the same five textual misses and two custom diagnostic skips listed in `tmp-good.log`; there are zero new `[ANCHOR-SKIPPED]` entries versus the good build. The five missed `adapt()` anchors are therefore baseline, not series regressions: `vmaCalculateStatistics`, `CreateUnbackedImage`, the ordinary `vmaCreateBuffer` call, the staging-pool `CreateBuffer` call, and the scheduler `vk::Check(result)` block. The two non-`adapt()` baseline skips are `wait sites of vk_buffer_cache: 4, expected 6` and the `vk_texture_cache` probe.

## 2. Duplicate definitions

**No redefinition found.** The cross-patch duplicate candidates were resolved as overloads, replacements, or members of different classes: `PopWait` is overloaded at `src/common/host_command_queue.h:51` and `:58`; `Handshake` occurs in separate service classes in `src/core/hle/service/nx2_gsv.h:145`, `:222`, `:410`, and `:1842`; `Succeed` is in separate classes at `:1628` and `:2612`; `GpuSplit::Drain()` and `GpuSplit::Drain(DrainSite)` are overloads at `src/video_core/gpu_split.cpp:234` and `:281`; and `CalculateUnswizzledSizeBytes` is overloaded at `src/video_core/texture_cache/util.cpp:596` and `:607`. `ReleaseFencesOnBack` has one final definition at `src/video_core/engines/puller.cpp:56`, so patches 0049 and 0089 did not leave two definitions.

The apparent `NativeTablesEnabled` duplicate is a signature replacement, not a second definition: only `NativeTablesEnabled(Stage stage)` remains at `src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1137`.

## 3. Signature drift and identifier scope

**No unhandled call-site signature mismatch found.** I checked the changed declarations and their final call sites, including the generated SPIR-V dispatch path. The relevant final signatures/calls are:

- `FreeDeferred(StagingBufferRef&, bool = false)` at `src/video_core/renderer_vulkan/vk_staging_buffer_pool.h:59`; calls at `src/video_core/renderer_vulkan/vk_buffer_cache.cpp:447`, `vk_query_cache.cpp:990`, and `vk_texture_cache.cpp:1440` supply either the optional second argument or rely on the default.
- `UpdateRescaling(bool, bool)` at `src/video_core/renderer_vulkan/vk_scheduler.h:218`; the call at `src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp:947` supplies both arguments.
- The SPIR-V global atomic functions take `(EmitContext&, Id, Id)` in `src/shader_recompiler/backend/spirv/emit_spirv_instructions.h:431-465`; `src/shader_recompiler/backend/spirv/emit_spirv.cpp:102-107` invokes them through the opcode dispatcher, and `src/shader_recompiler/frontend/ir/opcodes.inc:359` defines the two IR operands. The GLASM/GLSL one-argument functions are separate backends and were not changed.
- `DefineGlobalMemoryFunctions(const IR::Program&)` is declared at `src/shader_recompiler/backend/spirv/spirv_emit_context.h:463`, defined at `src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1079`, and called with `program` at `:597`.
- `PaceToVblank(u32, const Kernel::KProcess*)` is called with both arguments at `src/core/hle/service/nx2_gsv.h:1904`.
- `GPU::MapInOrder(GPUVAddr, u64, function)` is called with all three arguments at `src/core/hle/service/nvdrv/devices/nvhost_as_gpu.cpp:245`, `:323`, and `:366`; the wrapper definition is `src/video_core/gpu.cpp:701`.
- `GetStreamBuffer(size_t, bool)` is called with two arguments at `src/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:149`, `:163`, and `:298`; its definition is at `:211`.
- `UploadInChunks(Image&, u64 = ...)` calls at `src/video_core/texture_cache/texture_cache.h:2440` and `:2452` are valid with one and two arguments.
- `Nx2SparseMarkWritten(u64, u64, bool = false)` calls at `src/video_core/texture_cache/texture_cache.h:5208` and `:5216` are valid.
- `GpuSplit::Execute(const u64*, size_t)` is declared at `src/video_core/gpu_split.h:244` and defined at `src/video_core/gpu_split.cpp:481`; there are no final call sites, so no stale one-argument call remains.
- `ContinuousSizeInternal(KeyT)` at `src/common/range_map.h:70` is reached only through `GetContinuousSizeFrom` at `:54`; it has no external call sites.
- `NativeTablesEnabled(Stage)` is called with `env.ShaderStage()` at `src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1268`.
- `DrainBack(DrainSite = Acquire)` calls at `src/video_core/engines/puller.cpp:58`, `:73`, `:338`, `:379`, and `:408` are valid.
- `Serialize/Deserialize(..., bool = false)` calls at `src/video_core/shader_environment.cpp:1098` and `:1151` pass the new argument; the default preserves other callers.
- `GetContinuousSizeFrom` is called on mutable range maps at `src/tests/common/range_map.cpp:24-62` and `src/video_core/memory_manager.cpp:689`; removing `const` therefore leaves no const-call mismatch.

The scope review of the newest mesh/cache patches did not add a second scope finding. The known `emulated_mesh` use in `UpdateDynamicStates` is intentionally omitted per the owner's correction; no file-wide grep result is being used as scope evidence here.

## 4. New files and build-system references

The series creates 31 files. All include/call references were checked in the patched tree; the source files are listed in the relevant CMake target, except for the explicitly generated `mesh_compute_scan.comp`, whose checked-in generated product is `mesh_compute_scan_spv.h`. The files are:

- `src/common/hang_site.h` — referenced by `src/video_core/fence_manager.h`, `gpu_split.h`, `spin_recursive_mutex.h`, and `renderer_vulkan/vk_master_semaphore.cpp`; **not listed** in `src/common/CMakeLists.txt`.
- `src/common/host_command_queue.h` — included by `src/video_core/gpu_thread.h`; **not listed** in `src/common/CMakeLists.txt`.
- `src/common/paged_scalar.h` — referenced by core/memory code and listed in `src/common/CMakeLists.txt`.
- `src/shader_recompiler/backend/spirv/emit_spirv_mesh_readback.cpp` and `.h` — included by mesh emission/pipeline code and listed in `src/shader_recompiler/CMakeLists.txt`.
- `src/shader_recompiler/backend/spirv/emit_spirv_tensor.cpp` — listed in `src/shader_recompiler/CMakeLists.txt`.
- `src/shader_recompiler/backend/spirv/mesh_compute_interface.h` — included by pipeline/tests; **not listed** in `src/shader_recompiler/CMakeLists.txt`.
- `src/shader_recompiler/backend/spirv/mesh_compute_layout.h` and `mesh_compute_scan_spv.h` — included by mesh code and listed in `src/shader_recompiler/CMakeLists.txt`.
- `src/shader_recompiler/backend/spirv/mesh_compute_scan.comp` — source for the checked-in SPIR-V header; no CMake shader rule enumerates this directory's `.comp` inputs.
- `src/shader_recompiler/frontend/maxwell/translate/impl/hmma_native.cpp` and `tensor_mul_add.cpp` — listed in `src/shader_recompiler/CMakeLists.txt`.
- `src/shader_recompiler/ir_opt/compose_guard.h` — included by pipeline code; **not listed** in `src/shader_recompiler/CMakeLists.txt`.
- `src/shader_recompiler/ir_opt/oot_visual_workaround.h` — included and listed in `src/shader_recompiler/CMakeLists.txt`.
- The six test sources `src/tests/video_core/{global_atomic_routing,mesh_compute_interface,mesh_compute_push,oot_visual_workaround,storage_buffer_control_flow,storage_buffer_pointer_tracking}.cpp` — listed by `src/tests/CMakeLists.txt`.
- `src/video_core/bananza_terrain_compat.h` — included by macro code; **not listed** in `src/video_core/CMakeLists.txt`.
- `src/video_core/host_shaders/blit_color_square.frag` — listed by `src/video_core/host_shaders/CMakeLists.txt`.
- `src/video_core/nx2_debug_control.h` — included by texture/pipeline diagnostics; **not listed** in `src/video_core/CMakeLists.txt`.
- `src/video_core/nx2_loading_probe.h` and `src/video_core/readback_dependency_diagnostics.h` — no references anywhere in the patched tree and **not listed** in `src/video_core/CMakeLists.txt`.
- `src/video_core/page_filter.h` and `query_dma.h` — referenced and listed in `src/video_core/CMakeLists.txt`.
- `src/video_core/renderer_vulkan/mesh_fallback_limits.h` — the missing CMake entry is the already-fixed item explicitly excluded by the owner.
- `src/video_core/renderer_vulkan/vk_mesh_compute.cpp`, `.h`, and `vk_scheduler.h` — referenced and listed in `src/video_core/CMakeLists.txt`.

**PROBLEM (low build likelihood) / `src/common/CMakeLists.txt:71-87`, `src/shader_recompiler/CMakeLists.txt:28-30,194`, `src/video_core/CMakeLists.txt:109-125,191-192` / no compiler diagnostic in the current build; these are target source-list completeness defects / add the six referenced-but-unlisted headers (`common/hang_site.h`, `common/host_command_queue.h`, `backend/spirv/mesh_compute_interface.h`, `ir_opt/compose_guard.h`, `video_core/bananza_terrain_compat.h`, and `video_core/nx2_debug_control.h`) to the owning target at those list sections; either remove the two unreferenced diagnostic headers created by `tools/eglantine-patches/0023-port-diagnostic-headers.patch:5,51` or add their intended consumers and CMake entries.**

## 5. Struct field additions and aggregate initialization

**No wrong aggregate arity found.** The added `StorageBufferDescriptor::indirect_offset` at `src/shader_recompiler/shader_info.h:180` has a default initializer, so the four-element initializers at `src/tests/video_core/nvn_storage_descriptor.cpp:12-18` and `src/tests/video_core/storage_buffer_pointer_tracking.cpp:337-343` legally value-initialize the fifth field. `Output` initializers at `src/core/hle/service/nx2_gsv.h:2715` and `:2889`, `MemoryBlock` returns at `src/video_core/vulkan_common/vulkan_wrapper.cpp:561-565`, `ImagePoolKey` at `src/video_core/renderer_vulkan/vk_texture_cache.cpp:595-609`, `FetchedEntry` at `src/video_core/dma_pusher.cpp:1173-1184`, and the mesh-emulation constants at `src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp:908-915` supply the required fields (the designated forms were checked by field name). No expected aggregate-initializer compiler error remains.


## Verification notes

- Anchor execution used the script's own matching logic with `--report`, with shader-tool subprocesses stubbed only in the scratch runner; no source-tree output was used as evidence.
- The audit artifact is being written incrementally so a long static scan cannot erase earlier results.
