# Ocarina graphics root-cause analysis: white, fog, and black Link

## Scope and verdict

This is a read-only analysis of the PS5 Vulkan tree at the current working revision, including the applied source and the numbered patch files. The current source still has the 32-bank `SamplesStreamer` bound and still calls `ResetCounter()` when it crosses that bound; the proposed 0419 change is present only as a patch file, not in the source examined here (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:233-260`; `tools/eglantine-patches/0419-query-bound-keeps-the-count.patch:41-89`).

The strongest result is that the slot-bound hypothesis is mechanically valid and independently explains symptom C: at 8,201 slots, the current code abandons the running query, clears the carried value, and the next report can be written as zero. That zero can make the guest's render condition false, leaving the white clear visible while later HUD work still draws. The console observation quoted in 0419 is exactly above the current 8,192-slot threshold (`tools/eglantine-patches/0419-query-bound-keeps-the-count.patch:18-23`). It is not yet a console-proven causal link because the proposed 0419 behavior was not run.

For symptom A, the code and the port's own test record point first to native pointer-table reads that resolve to zero or are bound too short for Ocarina's far light-probe records. The current PS5 defaults are intended to avoid that: native tables are forced on, the explicit table-pool cap is deliberately not set, and the old visual workaround is forced off (`headless/main.cpp:694-710`). A remains reachable through an overriding environment value, an older pre-0418 build, a storage-buffer binding-budget failure, or a device mapping/range failure.

For symptom B, the most specific lead is the same pointer-table path in graphics stages. The PS5 defaults explicitly warn that `compute` mode leaves the graphics-stage pointer reads used by character lighting out and produces pause-menu Link black except for the eyes (`headless/main.cpp:699-706`). If the tested binary had `EDEN_NX2_NATIVE_TABLES=compute` or `0`, this is a high-confidence explanation.

---

## Part 1 — every direct path to a zero, stale, or too-low visibility report

### 1. Current 32-bank bound: zeroed report after `CloseCounter()`

**Condition.** `SamplesStreamer::CloseCounter()` first pauses the current Vulkan query, then checks `current_query->size_slots > SamplesQueryBank::BANK_SIZE * 32`, i.e. more than 8,192 slots (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:233-250`). A slot is added on every `StartCounter()`-driven reservation while the guest window remains open; the existing query's `size_slots` is incremented at `ReserveHostQuery()` (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:534-580`).

**What the guest sees.** The bound calls `ResetCounter()` (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:250-259`). For `SamplesStreamer`, reset clears the submission note/window state, ends the query if necessary, calls `AbandonCurrentQuery()`, and queues a callback that sets both `amend_value` and `accumulation_value` to zero (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:200-220`). `AbandonCurrentQuery()` releases the bank references and nulls `current_query` (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:592-599`). If the guest then reports, `WriteCounter()` builds a new report with `value = 0`; with no current host query it marks that report final-synced and returns it (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:425-452`). The report operation eventually copies that zero to guest memory (`Eglantine/src/video_core/query_cache/query_cache.h:307-345`).

This is not merely a memory guard. It changes a long visibility window into a zero report. The patch description explicitly states the resulting guest behavior: the game treats zero as everything occluded, skips the scene, and shows the clear colour with HUD/audio still active (`tools/eglantine-patches/0419-query-bound-keeps-the-count.patch:4-14`).

**Pause-menu relationship.** The bound is most likely to be reached after a long window with many submissions/render passes, not because the menu itself changes the bound. `Scheduler::EndPendingOperations()` invokes the ZPass close/reset decision at every submission (`Eglantine/src/video_core/renderer_vulkan/vk_scheduler.cpp:758-782`), and `CloseCounter()` grows the live query at those boundaries (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:233-250`). A pause transition can therefore make it more likely by changing submission/fence/render-pass cadence and by causing the next report to occur immediately after the abandoned query. The direct reachability evidence is the recorded 8,201-slot line in the 0419 patch description (`tools/eglantine-patches/0419-query-bound-keeps-the-count.patch:18-23`).

**PS5 reachability.** Reachable in the current PS5 Vulkan source. The current source has the 32-bank test and `ResetCounter()` call. The current title-specific scheduler default is pause for title `0400486024DEC000` unless `EDEN_NX2_ZPASS_PAUSE` is explicitly supplied (`Eglantine/src/video_core/renderer_vulkan/vk_scheduler.cpp:764-772`). Thus the path is active by default for the current Ocarina title when the query reaches 8,193 slots. The proposed 0419 fix—64 banks plus `PauseCounter()` at the bound—has not been applied in the examined source (`tools/eglantine-patches/0419-query-bound-keeps-the-count.patch:60-89`).

### 2. Submission reset: report window cut at every submission

**Condition.** If `pause_now` is false, `EndPendingOperations()` calls `CounterReset(ZPassPixelCount64)` instead of `CounterClose()` (`Eglantine/src/video_core/renderer_vulkan/vk_scheduler.cpp:773-782`). `RasterizerVulkan::ResetCounter()` routes ZPass to the query cache (`Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1640-1655`). The `SamplesStreamer::ResetCounter()` path is the abandon-and-zero path described above (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:200-220`).

**What the guest sees.** A guest window that begins before the submission loses the accumulated samples. Its subsequent report can be zero or too low. The port's own test log describes the exact prediction: a report whose window spanned a submission read too low, often zero, and the game treated it as occluded for one frame (`docs/ocarina-test-1-changes.md:23`).

**Pause-menu relationship.** High. Pause/resume is a submission-heavy transition and can place the report on the wrong side of a reset. `EDEN_NX2_ZPASS_PAUSE_AB` can deliberately alternate between pause and reset phases (`Eglantine/src/video_core/zpass_cut.h:30-53`). `EDEN_NX2_ZPASS_CUT_LOG` records the submission site, whether the window was open, and zero-after-cut reports (`Eglantine/src/video_core/zpass_cut.h:18-26`, `Eglantine/src/video_core/zpass_cut.h:214-230`).

**PS5 reachability.** Reachable whenever `EDEN_NX2_ZPASS_PAUSE=0` is supplied, for non-Ocarina titles, or during the reset phase of `EDEN_NX2_ZPASS_PAUSE_AB`. It is not the default for the current title-specific path because the current scheduler returns true for Ocarina when the variable is absent (`Eglantine/src/video_core/renderer_vulkan/vk_scheduler.cpp:764-772`). The older test-build table still describes `EDEN_NX2_ZPASS_PAUSE=1` as the switch used to fix flashes (`docs/ocarina-test-1-changes.md:23`); the current source has since made the Ocarina default title-specific.

### 3. Delayed fence flush: stale guest words before the query write lands

**Condition.** `Scheduler::DeferFlush()` batches a submission only when `EDEN_NX2_FENCE_FLUSH_US` is positive and the interval has not elapsed (`Eglantine/src/video_core/renderer_vulkan/vk_scheduler.cpp:269-285`). The current default is zero, so submissions are immediate. The 0402 patch records an A/B in which a 500-us batch caused 31 whole-screen flashes in 30 seconds and disabling it caused zero (`tools/eglantine-patches/0402-oot-query-flush-and-dma-snapshots.patch:118-131`).

**What the guest sees.** Below High accuracy, a report write is queued behind a later fence; a DMA read can consume the old guest-memory word. The 0402 split path now defers to the back thread, flushes the query cache, drains, and then reads (`Eglantine/src/video_core/engines/maxwell_3d.cpp:1321-1378`). The non-split macro path likewise flushes query results before reading (`Eglantine/src/video_core/macro.cpp:1533-1551`). If the delay or ownership/drain ordering defeats that path, the old word remains—often the previous zero/visibility value—and culling is wrong.

**Pause-menu relationship.** Medium to high. Menu transitions cause fences and drains, which are exactly where a delayed callback or deferred submission can be observed. The 0402 patch names the visible effect as one-frame white flash caused by stale zeroes (`tools/eglantine-patches/0402-oot-query-flush-and-dma-snapshots.patch:49-60`).

**PS5 reachability.** The old delayed-batch path is not reachable by default in the current PS5 source because `interval_us` defaults to zero (`Eglantine/src/video_core/renderer_vulkan/vk_scheduler.cpp:275-284`). It is reachable if the owner supplies `EDEN_NX2_FENCE_FLUSH_US=500` or another positive value. The current query-DMA flush path is reachable on PS5 Vulkan through `RasterizerVulkan::FlushRegion()` and `FlushQueryResults()` (`Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1708-1722`, `Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1750-1770`).

### 4. Low-accuracy fence report released before its producer fence

**Condition.** `CounterReport()` has a title-specific `keep_pending` default for Ocarina; an explicit `EDEN_NX2_QUERY_KEEP_PENDING` overrides it (`Eglantine/src/video_core/query_cache/query_cache.h:294-306`). If it is false, low-accuracy fence reports can be treated as already synchronized (`is_synced`) and freed immediately after `SignalFence()` (`Eglantine/src/video_core/query_cache/query_cache.h:306-369`). `FenceManager::SignalFence()` can skip an empty fence and invoke its operation immediately (`Eglantine/src/video_core/fence_manager.h:85-125`, `Eglantine/src/video_core/fence_manager.h:438-456`).

**What the guest sees.** The operation can run before the host GPU has completed the query-producing work, so the report operation can consume the previous/zero state. The patch description gives the exact mechanism and symptom: the report is freed at once, its fence may be skipped as empty, and the operation runs before the host GPU work; Ocarina then reads the wrong visibility report and flashes white (`tools/eglantine-patches/0244-flash-05-query-keep-pending.patch:10-18`).

**Pause-menu relationship.** High. A pause transition exercises fence creation, empty-fence skipping, and pending-operation release. It is specifically a report-lifetime race, so a menu transition that changes the fence cadence can expose it.

**PS5 reachability.** The current source defaults `keep_pending` to true for Ocarina (`Eglantine/src/video_core/query_cache/query_cache.h:301-305`), so the exact early-free path is not default in this source. It remains reachable with `EDEN_NX2_QUERY_KEEP_PENDING=0`, with a stale build that predates the title default, or if a non-title path uses the low-accuracy behavior.

### 5. Query DMA reads a report before carried accumulation is applied

**Condition.** Query-cache DMA state is marked dirty while a report is host-managed and not guest-synced (`Eglantine/src/video_core/query_cache/query_cache.h:474-480`, `Eglantine/src/video_core/query_cache/query_cache.h:619-650`). `RasterizerVulkan::FlushQueryResults()` first performs a semi-flush and then waits for an operation queued behind the report at ordinary modes (`Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1750-1770`). The code itself warns that a semi-flush alone can write the final value before the operation adds the carried count, especially for a report with no draw after sync (`Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1751-1754`).

**What the guest sees.** A DMA read can get a finalized-but-not-amended value, commonly zero for a report whose draw count is carried by `amend_value`, or an older value if the host write has not landed. `EDEN_NX2_QUERY_DMA_SYNC=1` waits for queries before the read; modes 2 and 3 explicitly flush pending reports (`Eglantine/src/video_core/query_dma.h:32-47`, `Eglantine/src/video_core/query_dma.h:242-267`). The default mode is zero (`Eglantine/src/video_core/query_dma.h:38-45`), so the diagnostic synchronization is not active unless requested.

**Pause-menu relationship.** Medium to high, because pause transitions are fence/query synchronization boundaries. This is distinct from the 32-bank bound: it can produce a low/stale report even when the slot count is small.

**PS5 reachability.** The path is reachable in the current Vulkan build, although 0402 added the query-cache flush before macro reads. It is best treated as a residual race to test with `EDEN_NX2_QUERY_DMA_SYNC=1` and `EDEN_NX2_QUERY_DMA_TRACE=1`.

### 6. Guest memory is not updated: unmapped, invalidated, rewritten, or unsynchronized report

There are four stale-word paths in the common query cache:

* If `GpuToCpuAddress(addr)` fails, `CounterReport()` returns without writing the report (`Eglantine/src/video_core/query_cache/query_cache.h:275-280`). The guest retains whatever was already at that address; an initially zero slot reads zero.
* If the report operation sees `IsInvalidated`, it returns without writing (`Eglantine/src/video_core/query_cache/query_cache.h:307-314`).
* If the report is not final-value-synced, it logs an error and returns without writing (`Eglantine/src/video_core/query_cache/query_cache.h:315-321`).
* When a newer report replaces the cache entry, the old query is marked `IsRewritten` (`Eglantine/src/video_core/query_cache/query_cache.h:371-385`); host sync skips rewritten or invalidated queries (`Eglantine/src/video_core/query_cache/query_cache.h:78-100`).

**What the guest sees.** These paths do not manufacture a new zero; they leave a previous word in guest memory. That word can be zero or a stale low visibility result and can therefore feed the same guest culling decision.

**Pause-menu relationship.** Medium. Menu transitions increase invalidation/rewrite/fence ordering pressure, but these conditions are not inherently menu-specific.

**PS5 reachability.** Reachable in the common Vulkan query path whenever the address is unmapped, a report is invalidated/rewritten, or a synchronization ordering error leaves it non-final. The unmapped branch is a defensive path; no evidence here says it is the path in the reported session.

### 7. Host conditional rendering can suppress draws from a zero/stale value

At the guest level, `ProcessQueryCondition()` can set `execute_on=false` for conditional, equal, or not-equal render-enable modes based on words read from the report/compare structure (`Eglantine/src/video_core/engines/maxwell_3d.cpp:759-851`). A false condition is consumed by the draw manager, which only calls the rasterizer when `ShouldExecute()` is true (`Eglantine/src/video_core/engines/draw_manager.cpp:317-331`).

The Vulkan host path can instead accelerate the condition. `AccelerateHostConditionalRendering()` looks up query-backed addresses and asks the runtime to set a Vulkan conditional-rendering buffer (`Eglantine/src/video_core/query_cache/query_cache.h:442-515`). The Vulkan implementation binds either the guest report buffer or a resolve buffer (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:1502-1570`, `Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:1572-1653`). A zero/stale report therefore can suppress a large draw sequence without the CPU visibly reading the word.

`EDEN_NX2_RENDER_ALWAYS=1` bypasses guest render-enable decisions and is an explicit discriminator (`Eglantine/src/video_core/query_dma.h:60-67`, `Eglantine/src/video_core/engines/maxwell_3d.cpp:759-766`). `EDEN_NX2_COND_MODE=always` is a second CPU-side discriminator (`Eglantine/src/video_core/engines/maxwell_3d.cpp:692-710`, `Eglantine/src/video_core/engines/maxwell_3d.cpp:782-803`).

**PS5 reachability.** The normal conditional-rendering code is reachable in Vulkan when the game programs render-enable conditions. The host extension path is device-dependent; it is disabled when the Vulkan conditional-rendering extension is unavailable (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:1572-1579`). If the PS5 RADV device exposes it, it is reachable.

### 8. Why a white clear remains with HUD visible

The Vulkan clear consumes the guest's clear colour (`Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1402-1440`). A deferred full clear becomes a render-pass load operation, and the code explicitly notes that conditional rendering does not apply to load operations (`Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1384-1393`). Therefore a scene whose 3D draws are suppressed can leave the clear image untouched; later HUD work can still be recorded and shown. The renderer submits and presents the frame normally (`Eglantine/src/video_core/renderer_vulkan/renderer_vulkan.cpp:247-254`).

This is the code-level bridge from a zero/stale visibility result to symptom C. The query code does not itself select white; it suppresses guest/host-conditioned 3D draws, while the guest-provided clear colour remains the visible background.

### 9. Other streamer reset overrides are not ZPass sources

The base streamer reset/close methods are no-ops (`Eglantine/src/video_core/query_cache/query_stream.h:28-47`). The Vulkan transform-feedback streamer maps `ResetCounter()` to its own `CloseCounter()`, and the primitives-succeeded streamer delegates reset to the transform-feedback streamer (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:817-831`, `Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:1208-1222`). They do not operate on `QueryType::ZPassPixelCount64`; only the `SamplesStreamer` is returned for that type (`Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:1658-1673`). They can corrupt their own reports, but they are not alternate causes of the Ocarina visibility zero.

---

## Part 2 — symptoms A and B

### A. Blown-out white fog/haze over ground and Link

#### Data flow: probe table to fog shader

The port's own test record identifies the fog classifier's spherical-harmonic light probes as pointer-table reads and says those reads returned zero; it records the fix as native pointer tables, end-of-mapping table bindings, and the old visual hacks off (`docs/ocarina-test-1-changes.md:9-12`). The stability report gives the measured geometry of the lead: probe records sit roughly 104–122 MiB past the pointer, and a 64-MiB read-only pool cap makes far reads zero, removing the blue ambient term and producing a white haze over Link as well as the scene (`patches/ps5j-stability-round/REPORT.md:64-68`).

The current implementation has four relevant stages:

1. `EDEN_NX2_NATIVE_TABLES` controls whether pointers read through memory are tracked. Unset means on; `0` means off; `compute` enables only compute-stage reads; `compute+mesh` additionally enables task/mesh (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1124-1156`).
2. Pointer candidates are collected from constant-buffer pointers and indirect table loads. Candidates are dropped when native tables are disabled, when a table is being used by a graphics stage, or when table-budget accounting does not permit them (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1163-1172`, `Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1262-1288`).
3. The second pass only enables table candidates if storage-buffer bindings leave budget; otherwise it marks bindings full (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1746-1763`).
4. SPIR-V global loads with no routed candidate fall through to the null value, while routed loads use the bound storage buffer (`Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1132-1171`, `Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1204-1207`). A table entry whose holder pointer is zero is explicitly made a null binding (`Eglantine/src/video_core/buffer_cache/buffer_cache.h:780-798`, `Eglantine/src/video_core/buffer_cache/buffer_cache.h:892-905`; a zero base is also rejected by `StorageBufferBinding()` at `Eglantine/src/video_core/buffer_cache/buffer_cache.h:3428-3436`).

#### Where the probe can become zero on PS5

The old cap path is explicit: for a read-only table pointer, the binding length is limited by `EDEN_NX2_TABLE_POOL_MIB`; the current source still applies any non-zero override, with a maximum of the device range/1 GiB (`Eglantine/src/video_core/buffer_cache/buffer_cache.h:3466-3484`). A 64-MiB or smaller cap cannot cover the probe records described in the port report. Before 0418, the cap was the effective default/limit; 0418 changed read-only pointers to bind to the mapping end and left the environment cap as a tightening option (`tools/eglantine-patches/0418-table-pointers-bind-to-the-mapping-end.patch:8-29`).

The current PS5 launcher deliberately does not set `EDEN_NX2_TABLE_POOL_MIB`, sets native tables to `1`, and sets the visual workaround to `0` (`headless/main.cpp:694-710`). Therefore the intended current build should not take the known 64-MiB zero-probe path. A is nevertheless reachable if:

* the tested environment overrides `EDEN_NX2_NATIVE_TABLES` to `0` or `compute`, causing the relevant graphics/fog pointer reads to remain global (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1124-1156`);
* the environment sets `EDEN_NX2_TABLE_POOL_MIB=64` or another value below the probe reach (`Eglantine/src/video_core/buffer_cache/buffer_cache.h:3470-3483`);
* the device mapping/range is shorter than the probe address or the table holder is zero, producing a null binding (`Eglantine/src/video_core/buffer_cache/buffer_cache.h:3432-3436`, `Eglantine/src/video_core/buffer_cache/buffer_cache.h:3536-3544`);
* the storage-buffer binding budget is full, so table candidates are not enabled and loads fall through to null (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1746-1763`, `Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1152-1171`); or
* the binary predates 0418 or the tested package did not contain the current launcher defaults (`tools/eglantine-patches/0418-table-pointers-bind-to-the-mapping-end.patch:16-29`, `headless/main.cpp:694-706`).

#### Other A candidates

* The old Ocarina visual workaround scales the matched fog scalar to 0.25 and exposure to 0.65 only when enabled; the current launcher forces it off (`Eglantine/src/shader_recompiler/ir_opt/oot_visual_workaround.h:26-38`, `Eglantine/src/shader_recompiler/ir_opt/oot_visual_workaround.h:75-90`, `headless/main.cpp:707-710`). Enabling it should reduce, not cause, the white haze, so it is a masking/discriminator path rather than the lead root cause.
* `EDEN_NX2_OOT_FOG_SUN` can scale the depth compares in two froxel programs; unset does nothing, and the current launcher does not set it (`Eglantine/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2042-2063`). It is a possible overbright-fog experiment, not a current default cause.
* The native 64-KiB-bank reupload path documents that once-per-frame upload can leave Ocarina per-draw material data stale for a frame and cause white flashes (`Eglantine/src/video_core/buffer_cache/buffer_cache.h:1552-1562`). It is a temporal material-flash candidate, not a good explanation for a stable centre haze that overlays Link.

### B. Equipment-menu Link black except emissive eyes

#### Data flow: graphics-stage character lighting

The launcher comments identify the exact distinction: `EDEN_NX2_NATIVE_TABLES=compute` takes the no-table path for vertex/fragment/task/mesh stages and leaves graphics stages without the pointer reads used by Ocarina's character lighting; the documented result is pause-menu Link black except for the eyes (`headless/main.cpp:699-706`). This is consistent with emissive eyes surviving while diffuse/probe-driven lighting is zero.

The compute-only patch makes the mechanism explicit: `compute` preserves table pointers only for compute programs and takes the zero path for vertex, fragment, task, and mesh (`tools/eglantine-patches/0202-t39-white-02-native-tables-compute-only.patch:9-18`, `tools/eglantine-patches/0202-t39-white-02-native-tables-compute-only.patch:36-76`). In the current source, `NativeTablesEnabled(stage)` implements that stage gate (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1124-1156`). If the graphics-stage access is not routed to an SSBO, the SPIR-V global-load function returns a null value (`Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1152-1171`, `Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1204-1207`).

#### Where character lighting can become zero

The most direct causes are:

1. `EDEN_NX2_NATIVE_TABLES=compute` or `0` on the tested binary, leaving the graphics-stage pointer read global and therefore zero (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1124-1156`, `headless/main.cpp:702-706`).
2. A graphics pointer/table binding is present but its holder is zero, so binding returns `NULL_BINDING` (`Eglantine/src/video_core/buffer_cache/buffer_cache.h:780-798`).
3. The read-only table binding is capped before the character's probe/material record, or its mapping/range ends before that record (`Eglantine/src/video_core/buffer_cache/buffer_cache.h:3470-3484`, `Eglantine/src/video_core/buffer_cache/buffer_cache.h:3525-3544`).
4. The graphics program exhausts its SSBO budget; table candidates are not enabled and the unbound load returns null (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1746-1763`, `Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1152-1171`).

A separate AMD-specific failure can damage lighting components rather than make every diffuse term zero. With `EDEN_NX2_AMD_SHADER_FIXES=1`, 8/16-byte loads are split into word loads when RADV's 4-byte binding alignment makes vector indexing inexact; the source specifically cites Ocarina light colours losing red/green when this fix is absent (`Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:22-26`, `Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1173-1181`, `Eglantine/src/shader_recompiler/backend/spirv/emit_spirv_memory.cpp:213-227`). The current PS5 launcher enables this fix (`headless/main.cpp:677-679`), so it is reachable only if an explicit environment override disables it or the tested binary predates the default.

---

## Part 3 — ranked candidates and cheapest confirmation

The confidence values below are confidence in the hypothesis as the cause of the named symptom on the current PS5 tree, not confidence that the symptom has already been reproduced by this source-only investigation.

### C — whole-screen white after pause/resume

1. **Current 32-bank reset/abandon — 0.80 confidence, reachable.**
   * **Location:** `Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:233-260`, `:200-220`, `:592-599`; report write at `Eglantine/src/video_core/query_cache/query_cache.h:323-345`.
   * **Mechanism:** 8,201 exceeds the live 8,192 limit; the current code resets instead of pausing, abandons the accumulated query, and the next report can be zero. The 0419 description records exactly one 8,201-slot drop in the relevant 145-second session (`tools/eglantine-patches/0419-query-bound-keeps-the-count.patch:18-23`). A zero guest visibility result can suppress the 3D draw sequence (`Eglantine/src/video_core/engines/maxwell_3d.cpp:822-851`) while the clear and HUD still present (`Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1384-1440`, `Eglantine/src/video_core/renderer_vulkan/renderer_vulkan.cpp:247-254`).
   * **Cheapest confirmation:** run the same scenario with the 0419 hunk applied (64 banks and `PauseCounter()` at the bound) and `EDEN_NX2_ZPASS_CUT_LOG=1`; the confirming result is no white transition at the logged bound and no `cut zero`/`to zero` report tied to that event (`tools/eglantine-patches/0419-query-bound-keeps-the-count.patch:76-89`, `Eglantine/src/video_core/zpass_cut.h:214-230`). `EDEN_NX2_ZPASS_FORCE=1` is a cheaper broad discriminator, but it proves only that a zero visibility report is involved, not that the 32-bank event caused it (`Eglantine/src/video_core/query_cache/query_cache.h:329-333`).

2. **Submission reset cutting the guest window — 0.75 confidence, reachable when pause is disabled.**
   * **Location:** `Eglantine/src/video_core/renderer_vulkan/vk_scheduler.cpp:758-782`; `SamplesStreamer::ResetCounter()` at `Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:200-220`.
   * **Mechanism:** every submission resets rather than pauses, producing too-low/zero reports; this was directly associated with white flashes in the port test table (`docs/ocarina-test-1-changes.md:23`).
   * **Cheapest confirmation:** A/B `EDEN_NX2_ZPASS_PAUSE=0` against `EDEN_NX2_ZPASS_PAUSE=1` on one fixed pause/resume route. If the whole-screen white event tracks the reset setting, the submission path is confirmed. `EDEN_NX2_ZPASS_CUT_LOG=1` supplies the per-site cut/zero evidence (`Eglantine/src/video_core/zpass_cut.h:18-26`, `Eglantine/src/video_core/zpass_cut.h:214-230`).

3. **Stale/too-low report at DMA/fence handoff — 0.55 confidence, reachable but partly mitigated.**
   * **Location:** `Eglantine/src/video_core/engines/maxwell_3d.cpp:1345-1378`, `Eglantine/src/video_core/macro.cpp:1533-1551`, and `Eglantine/src/video_core/renderer_vulkan/vk_rasterizer.cpp:1750-1770`.
   * **Mechanism:** the guest reads before a report write or before carried accumulation is applied; 0402 was written specifically for this class and records 31 flashes with a 500-us fence batch versus zero with batching disabled (`tools/eglantine-patches/0402-oot-query-flush-and-dma-snapshots.patch:49-60`, `tools/eglantine-patches/0402-oot-query-flush-and-dma-snapshots.patch:118-131`).
   * **Cheapest confirmation:** set `EDEN_NX2_QUERY_DMA_SYNC=1` for one run and enable `EDEN_NX2_QUERY_DMA_TRACE=1`; disappearance of the transition with pending-read traces confirms the handoff path (`Eglantine/src/video_core/query_dma.h:21-47`, `Eglantine/src/video_core/query_dma.h:242-257`).

4. **Early fence report release — 0.35 confidence in the current tree, reachable only if overridden.**
   * **Location:** `Eglantine/src/video_core/query_cache/query_cache.h:294-369`; `Eglantine/src/video_core/fence_manager.h:98-125`, `:438-456`.
   * **Mechanism:** low-accuracy fence reports can be freed and executed before the producer work unless `keep_pending` is true. Current Ocarina defaults it on (`Eglantine/src/video_core/query_cache/query_cache.h:301-305`).
   * **Cheapest confirmation:** A/B `EDEN_NX2_QUERY_KEEP_PENDING=0` versus `1` with query-DMA tracing. A change in the transition count, especially a regression with `0`, confirms that report lifetime is involved; it does not explain the current default run unless the default was overridden.

5. **Guest conditional-rendering state or host HCR consuming a stale word — 0.30 confidence, reachable.**
   * **Location:** `Eglantine/src/video_core/engines/maxwell_3d.cpp:759-851`; `Eglantine/src/video_core/query_cache/query_cache.h:442-515`; Vulkan HCR at `Eglantine/src/video_core/renderer_vulkan/vk_query_cache.cpp:1572-1653`.
   * **Mechanism:** a false conditional suppresses draws; the clear still executes and HUD can be recorded later. This is an effect layer shared by the higher-ranked report causes.
   * **Cheapest confirmation:** `EDEN_NX2_RENDER_ALWAYS=1` for one run. If the white transition disappears but hidden objects appear, the render-enable decision is confirmed (`Eglantine/src/video_core/query_dma.h:60-67`).

6. **Final compose NaN/infinity pack — 0.10 confidence, reachable only as an explicit guard experiment.**
   * **Location:** `Eglantine/src/shader_recompiler/ir_opt/compose_guard.h:20-38`, pipeline hook at `Eglantine/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2065-2073`.
   * **Mechanism:** a bad float can make RGB9E5 channels become white; the guard documents three bad channels as white (`Eglantine/src/shader_recompiler/ir_opt/compose_guard.h:20-28`).
   * **Cheapest confirmation:** `EDEN_NX2_COMPOSE_GUARD=mark` and inspect the guard log. This was already tried once in the port notes without finding the flash cause, lowering its rank (`docs/ocarina-test-1-changes.md:30`).

7. **Stale native material bank — 0.10 confidence for whole-screen white, reachable.**
   * **Location:** `Eglantine/src/video_core/buffer_cache/buffer_cache.h:1552-1605`.
   * **Mechanism:** once-per-frame reupload can leave per-draw material data stale for a frame and is documented to cause white flashes, but it is not a natural explanation for a whole-screen clear plus HUD after a pause.
   * **Cheapest confirmation:** set `EDEN_NX2_BANK_REUPLOAD=0` versus `2` and correlate the event with the bank path; the source describes the default mode and the stale-material failure (`Eglantine/src/video_core/buffer_cache/buffer_cache.h:1555-1562`).

### A — centre-frame blown-out white haze over ground and Link

1. **Native pointer-table/probe read becomes zero or is outside the bound — 0.85 confidence as the root mechanism, reachable only under the wrong runtime/default.**
   * **Location:** `docs/ocarina-test-1-changes.md:11`; `patches/ps5j-stability-round/REPORT.md:64-68`; pointer routing at `Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1124-1156`, `:1262-1288`, and null-load fallback at `Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1152-1171`, `:1204-1207`.
   * **Mechanism:** zero probe coefficients remove the intended blue ambient/scattering term; the fog becomes overbright and overlays the scene. This is the port's documented cause, not a guess invented from the screenshot.
   * **Cheapest confirmation:** A/B `EDEN_NX2_NATIVE_TABLES=0` versus `1` while leaving `EDEN_NX2_TABLE_POOL_MIB` unset. The zero-table run should reproduce the haze; the table-on run should remove it. Add a shader dump or a one-line log around `All ... storage buffer bindings taken` to catch a binding-budget failure (`Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1760-1763`).

2. **Probe binding capped at 64 MiB or an older pre-0418 binary — 0.75 confidence if the tested package has the old recipe, not reachable under current launcher defaults.**
   * **Location:** `Eglantine/src/video_core/buffer_cache/buffer_cache.h:3470-3483`; 0418 change at `tools/eglantine-patches/0418-table-pointers-bind-to-the-mapping-end.patch:8-29`; current defaults at `headless/main.cpp:694-706`.
   * **Mechanism:** the far probe records are around 104–130 MiB out; a 64-MiB binding makes the reads zero/out-of-range.
   * **Cheapest confirmation:** force `EDEN_NX2_TABLE_POOL_MIB=64` for one run; recurrence of A confirms the range mechanism. Then unset it and verify the boot log/package is actually the 0418+ binary.

3. **Fog/exposure workaround or fog-sun shader mode — 0.20 confidence, reachable only by explicit environment.**
   * **Location:** `Eglantine/src/shader_recompiler/ir_opt/oot_visual_workaround.h:75-90`; fog-sun hook at `Eglantine/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2042-2063`.
   * **Mechanism:** these change fog/exposure or froxel shadow-compare factors, but the current launcher forces the main workaround off and leaves fog-sun unset (`headless/main.cpp:707-710`).
   * **Cheapest confirmation:** inspect the boot/pipeline log for `TEMPORARY OoT visual workaround` or `TEMPORARY OoT fog sun`; no such line means these are not active (`Eglantine/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2036-2063`).

4. **Stale native material/uniform bank — 0.15 confidence for the persistent haze, reachable.**
   * **Location:** `Eglantine/src/video_core/buffer_cache/buffer_cache.h:1552-1605`.
   * **Mechanism:** stale per-draw material values can cause brief white flashes, not a stable fog volume over Link.
   * **Cheapest confirmation:** `EDEN_NX2_BANK_REUPLOAD=2` and a fixed camera; if only isolated flashes change while the haze remains, reject it as A's root cause.

### B — equipment-menu Link black except emissive eyes

1. **Graphics-stage native tables disabled (`compute` or `0`) — 0.90 confidence, reachable by environment override.**
   * **Location:** `headless/main.cpp:699-706`; stage gate at `Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1124-1156`; null global load at `Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1152-1171`, `:1204-1207`.
   * **Mechanism:** character diffuse/probe reads are graphics-stage pointer loads; `compute` intentionally does not route them, so the lighting inputs are zero while emissive eyes can remain.
   * **Cheapest confirmation:** A/B `EDEN_NX2_NATIVE_TABLES=compute` versus `1` on the Equipment screen. The current launcher comment already states the expected black-Link result for the wrong mode (`headless/main.cpp:702-706`).

2. **Graphics pointer range/table holder/binding budget fails — 0.65 confidence, reachable.**
   * **Location:** holder-zero/null binding at `Eglantine/src/video_core/buffer_cache/buffer_cache.h:780-798`; range cap at `Eglantine/src/video_core/buffer_cache/buffer_cache.h:3470-3484`; table budget at `Eglantine/src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1746-1763`.
   * **Mechanism:** the character shader is compiled with a pointer load, but the bound address is null, too short, or omitted because all storage-buffer slots are occupied; the SPIR-V fallback returns zero.
   * **Cheapest confirmation:** enable shader/pipeline diagnostics and search for `All ... storage buffer bindings taken` or `Failed to find storage buffer`; independently set `EDEN_NX2_TABLE_POOL_MIB=64` for one Equipment-screen A/B.

3. **AMD misaligned vector storage loads with AMD fixes disabled — 0.30 confidence, reachable by override.**
   * **Location:** `Eglantine/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:1173-1181`; `Eglantine/src/shader_recompiler/backend/spirv/emit_spirv_memory.cpp:213-227`; PS5 default at `headless/main.cpp:677-679`.
   * **Mechanism:** without the fix, vector views can read the preceding word and shift lighting components; the source explicitly calls out Ocarina lighting. This predicts wrong/coloured lighting more directly than an all-zero result.
   * **Cheapest confirmation:** compare `EDEN_NX2_AMD_SHADER_FIXES=0` and `1` on the Equipment screen while confirming the environment value in the boot log. A change in Link's diffuse output confirms this path; an unchanged all-black model rejects it.

4. **Fog/visual workaround or final-composite shader miscompile — 0.15 confidence, not specifically supported by the symptom.**
   * **Location:** workaround at `Eglantine/src/shader_recompiler/ir_opt/oot_visual_workaround.h:26-90`; compose guard hook at `Eglantine/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:2065-2073`.
   * **Mechanism:** these affect fog/exposure or final compute output, not the character's diffuse-only lighting path. They are lower-ranked unless a shader dump identifies the Equipment model program.
   * **Cheapest confirmation:** capture the Equipment frame with `EDEN_NX2_SHADER_DUMP`/frame diagnostics and compare the character graphics program's pointer descriptors and shader hash against the working gameplay shader; no pointer/lighting difference means reject this class.

---

## Final ranked conclusion

* **C:** The current 32-bank bound is the leading explanation for the specifically reported whole-screen-white transition because the 8,201-slot event is above the active threshold and the source proves that the bound abandons and zeroes the count. Submission resets and stale DMA/fence reads are nearly as credible and share the same downstream culling mechanism. The single clean test is the 0419 `PauseCounter()`/64-bank A/B with `ZPASS_CUT_LOG=1`; `ZPASS_FORCE=1` is the fast broad discriminator.
* **A:** The pointer-table/light-probe zero path is the leading explanation and is directly recorded by the port author. It should be fixed in the current intended PS5 defaults, so first verify the actual binary and environment: native tables `1`, no explicit table cap, and workaround `0`. If those are true, the next suspect is a binding-budget or device-range failure, not the old 64-MiB cap.
* **B:** `EDEN_NX2_NATIVE_TABLES=compute`/`0` is the leading explanation for black diffuse character lighting with emissive eyes. The current launcher comments name the exact result. If tables are already `1`, inspect the graphics-stage pointer binding/range and then the AMD misaligned-load fix; do not start with fog or occlusion queries.

No repository files were modified except this report.