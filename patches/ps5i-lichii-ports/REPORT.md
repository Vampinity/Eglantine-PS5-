# Lichii Ocarina ports for EglantinePS5

Base inspected: Eglantine core `0ca2032bff`, with the 257 existing `0001`–`0265` maintainer patches applied. Sources reviewed: the full Lichii PR diffs and PR discussions for #657, #551, #410, #584, #270, and #583.

## Results

| PR | Result | Port / existing state |
|---|---|---|
| #657 | Not applicable | The PS5 tree has no `VK_KHR_cooperative_matrix` or cooperative-HMMA path; its HMMA path is software SPIR-V lowering. The AMD gate from #657 would be dead code without first porting #583. No speculative replacement was written. |
| #551 | Ported | `0360-oot-lichii-port.patch` and `0363-oot-lichii-port.patch`. |
| #410 | Ported | `0361-oot-lichii-port.patch`. |
| #584 | Ported | `0362-oot-lichii-port.patch`, adapted to the PS5 loader's equivalent bundled routine. |
| #270 | Not applicable / skipped | The PS5 tree has no matching `nx2_present_pacer.h`/`fps_limit` setting integration. It was optional and did not port cleanly as a literal change. |
| #583 | Not applicable / skipped | The cooperative-matrix feature is absent from the PS5 tree. It was optional and would be a new large feature port, not a clean numbered-fix port. |

## #657 — AMD RDNA2 shader-build crash

**Lichii mechanism reviewed:** #657 adds the shader integer dot-product property query in `src/video_core/vulkan_common/vulkan_device.{h,cpp}` and treats AMD cooperative matrices as emulated when `integerDotProduct4x8BitPackedMixedSignednessAccelerated` is false. That keeps the 16x16x16 HMMA path off pre-RDNA3 AMD GPUs.

**Why no patch was added:** the PS5 tree contains no cooperative-matrix device feature or `VK_KHR_cooperative_matrix` path. Its relevant code is the software HMMA path in `src/shader_recompiler/backend/spirv/emit_spirv_tensor.cpp`; adding #657's device gate would not affect the owner's crash. The required prerequisite feature is Lichii #583, which was explicitly optional and does not port cleanly here.

**What would break if this were ported without #583:** nothing would change on the console; the gate would never control a pipeline. Porting only a guessed AMD workaround could hide or alter software-HMMA compilation without evidence. The correct future port must bring #583's cooperative path and #657's gate together.

**Evidence:** Lichii #657's discussion identifies the crash as `amdvlk64.dll` during OoT pipeline prebuild after #583 and initially says it needed an RDNA2 tester. Lichii build-117 release notes later report the fix on RX 6000/RX 5000/Vega/Polaris and credit RX 6800 XT testing. The PS5 source search found no cooperative-matrix symbols.

## #551 — Hyrule Field grass / foliage culling

**Already present before this port:**

- `0200-t39-native-storage-buffer-pointer-tracking.patch` already supplied native pointer and pointer-table tracking.
- `0247-flash-02-pr715-paired-pointers.patch` supplied a partial #715/#410-style paired-pointer path, but only behind `EDEN_NX2_PAIRED_POINTERS=1` and with the old 32-binding ceiling.
- `0259-plants-02-core-null-list-slots.patch` supplied null-slot detection/rebuild logic, but only behind `EDEN_NX2_NULL_SLOTS`.

**Ported files and mechanisms:**

- `src/shader_recompiler/shader_info.h:282` — `Info::MAX_SSBOS` is 64 instead of 32, matching #551's measured OoT foliage requirement of up to 37 bindings.
- `src/video_core/buffer_cache/buffer_cache_base.h:66,153-156` — storage-buffer arrays and enable/write masks use the 64-entry limit and `u64` masks; `ForEachEnabledBit` is mask-type generic at line 493.
- `src/video_core/buffer_cache/buffer_cache.h:760-777,846-861` — 64-bit binding tests and shifts.
- `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:785-794,1167-1218,2062` — the existing null-slot rebuild mechanism is now unconditional instead of an experiment toggle, with its existing four-rebuild cap and old-pipeline retention preserved.
- `src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1709-1717` in `0363` — the effective descriptor-set storage-buffer limit is clamped to `min(MAX_SSBOS, host_info.max_descriptor_set_storage_buffers)`; candidate, fallback, and table paths use that limit at approximately lines 1434, 1505, 1747, 1754-1756, and 1766.

**What it would break:** memory use and descriptor bookkeeping increase for programs that actually bind more than 32 buffers. On a Vulkan device with a lower storage-buffer descriptor limit, the added limit handling leaves excess accesses global rather than binding an invalid descriptor set. A regression would show as pipeline translation failure, missing vegetation, or black/incorrect foliage rather than silent descriptor shifting.

**Evidence / measured status:** Lichii #551's discussion explains the exact failure: foliage culling programs need 33–37 storage buffers and cached translations made in another area lose pointer-table bindings. It reports a clean Windows clang-cl build, Linux tests, and an empty-cache verification plan; the PR author did not claim a completed gameplay run. Lichii build-116 release notes report Hyrule Field grass fixed. The PS5 port is source-verified only here; no console run or build was available.

## #410 — near foliage fades out

**Already present before this port:** `0247-flash-02-pr715-paired-pointers.patch` had the core candidate search, but made it opt-in with `EDEN_NX2_PAIRED_POINTERS=1`, limited it to the existing PS5 candidate policy, and did not include all of #410's backend safety guards.

**Ported files and mechanisms:**

- `src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:86-92` — records whether competing loaded pointers may be bound and whether they were bound.
- `src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:162-179` — adds `IsRuntimeRoutable`, restricting global routing to operations the SPIR-V backend can actually select among buffers.
- `src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1387-1447` — removes the environment toggle, searches paired accesses unconditionally, and retains the pair for unsupported opcodes or hosts without shader int64.
- `src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp:1712-1737` — retries with only the paired pointer when competing loaded pointers exceed the effective binding limit.

**What it would break:** affected native programs keep global accesses and pay the runtime buffer-selection compare chain; their SPIR-V changes and cached translations rebuild. A bad port would make the backend route unsupported subword/atomic operations and show as shader translation failure, or would bind past the descriptor limit and show as missing/incorrect foliage. The guards specifically prevent those console-visible failures.

**Evidence / measured status:** Lichii #410's discussion cites Eglantine #715 and says the owner observed that the fix removes close-up plant/leaf/object fading. The PR also reports 774/774 test cases and explains the exact `c0[12]` versus `*c0[12]` out-of-bounds-zero mechanism. That is measured upstream evidence; this PS5 port has not been gameplay-tested here.

## #584 — Dynamic FPS down to 10 FPS

**Existing equivalent:** the PS5 does not use Lichii's `src/core/file_sys/patch_manager.cpp` built-in patch table. Its equivalent bundled routine is in `src/core/loader/nso.cpp:69-77`, gated by `EDEN_NX2_OOT_DYNAMIC_FPS=1` and protected by the OoT build/layout checks.

**Ported change:** `0362-oot-lichii-port.patch` changes the routine's `movz x14, #20` bytes from `8E 02 80 D2` to `4E 01 80 D2` at `src/core/loader/nso.cpp:73`, and updates the nearby explanation at line 57 to `CNTFRQ / 10` (100 ms).

**What it would break:** at 10–19 FPS, the game advances at real time instead of slowing below the old 50 ms cap. At 20 FPS and above behavior is unchanged. The risk is incorrect routine bytes or an unexpected OoT build; the existing build-id, bounds, and opt-in guards limit that to skipping the patch rather than modifying another title.

**Evidence / measured status:** Lichii #584 reports real-time behavior at 10–16 FPS on an RX 6650 XT in Kokiri Forest and the sword cave, with no visible downside; the PR's build/tests were green but gameplay was not independently re-run by the author. The PS5 change is an equivalent byte change in the different loader location.

## Optional PRs skipped

- **#270:** Lichii adds `fps_limit` settings, `PacedInterval`, UI translation, tests, and changes `nx2_gsv.h`. The PS5 tree has its own vblank pacing implementation but lacks the matching header/test/settings architecture. No literal or guessed port was added. If the owner wants a configurable present cap later, it needs a separate PS5 design review.
- **#583:** Lichii adds the cooperative HMMA feature across nine shader/Vulkan files and tests. The PS5 tree currently uses software HMMA and has no corresponding feature to merge. It was not silently replaced with an original implementation.

## Verification

No local build or console test was possible in this environment. Patch application was verified instead:

```text
clean base: 0ca2032bff
applied: all 257 tools/eglantine-patches/0[0-2]*.patch files
then: 0360, 0361, 0362, 0363 with absolute paths
git diff --check: passed
current Eglantine applied-tree checks for 0360–0362: passed
0363: passed sequentially after 0360–0362 on the clean verification worktree
```

The clean-base run used a detached worktree under `C:/Users/horac/lichii-ally/eglantine-ps5/`, so no path outside the required repository component was written. No patches `0001`–`0265` and no reverted `0301` were modified.
