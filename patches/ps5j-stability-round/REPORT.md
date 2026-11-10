# Stability and crash round — report

Round covers: the `crash-20261108-184134` log (NPC-talk "crash", the most stable run yet) plus the
graphics complaints (black/flickering water, fog over Link, general flicker).

## 1. Subagent work

Seven subagents in two parallel batches, all completed. 213 tool/API calls, ~13 minutes wall clock.
Transcripts: `~/AppData/Local/hermes/cache/delegation/live/deleg_ecf63724/` (3) and
`.../deleg_76f67acc/` (4).

| # | Agent (batch) | Calls / time | Question | Verdict |
|---|---|---|---|---|
| 1 | Cross-emulator survey (A) | 20 / 363 s | Do other PS5 ports and Switch forks fix these five symptoms? | **Found the PS5-tested fog recipe** (viejunerd/Eglantine-PS5- issue 21). **No verified fix anywhere for the NPC-dialogue crash.** |
| 2 | A/B settings matrix (A) | 28 / 509 s | Every switch the port reads, with defaults and risk | 313 `getenv` names enumerated from source with file:line; A/B matrix produced; console env vs memory recipe diffed to **exactly 5 differing entries** |
| 3 | Unmapped reads + dialogue (A) | 27 / 707 s | Root cause of `Unmapped Device ReadBlock` and the dialogue crash | Reads are emulator mapping bugs, not guest; the two service calls are **network/telemetry, not dialogue**; Lichii #165 shows the reads persist in a **working** build → not the crash trigger |
| 4 | GPU memory audit (B) | 36 / 458 s | Why the thresholds walk to the floor; why the churn | **Proof by formula** (below). Identified the 10-frame aggressive eviction as the thrash. Format 74 identified. |
| 5 | Feedback loop + dialogue (B) | 45 / 637 s | Is the feedback copy causing the unmapped reads? | **Disproved** — `CopyImage` is GPU→GPU. Real cause: CPU uploads of 3D images with no CPU backing. Service calls **proven non-fatal**. |
| 6 | Mesh fallback (B) | 24 / 595 s | Why geometry is silently dropped, and the fix | **Root cause confirmed**: fallback is built on `bufferDeviceAddress`, which RADV does not report. Needs Lichii #569's descriptor rewrite. |
| 7 | Port parity (B) | 33 / 786 s | Which PC fixes are already in our tree? | Confirmed `0413`==#570 and `0391`==#778 present; **10 items missing** (listed below) |

## 2. Crash research — the honest result

**No emulator crash exists in the log.** Two agents (3, 5) reached this independently:

- `IGeneralService` cmd 101 (68.4 s) and `prepo:u` cmd 10500 (122.8 s) hit `service.cpp:178`, which is a
  **soft** assert. `AssertFailSoftImpl()` only breaks with `use_debug_asserts`, which defaults false.
  Both went to `auto stub fallback` and the session kept presenting frames.
- The log has **no** fatal, abort, device-lost, watchdog or `vk::Check` failure. It ends mid-stream at
  483.2 s after ~445 s of play, present #7800.
- `IGeneralService` is the network service; `prepo:u` is play-report telemetry. Neither is a dialogue
  or font path. The only font work upstream (Eglantine #646) is Super Mario Bros. Wonder.

**The service-call theory is disproved.** Named cause requires a console freeze/backtrace or a Windows
minidump. Not invented.

## 3. Stability findings that were proven

**The threshold walk to the floor (agent 4) — proven from source:**
`pressure_margin += 512 MiB` per allocation-pressure event, capped at 3 GiB; recovery is
`-= 128 MiB` per **600 frames with no new pressure event**. Any recurring pressure resets that
counter, so the margin never returns. The formula reproduces the log exactly:

| margin | expected | critical |
|---:|---:|---:|
| 0 MiB | 2560 | 3328 |
| 512 MiB | 2048 | 2816 |
| 1024 MiB | 1536 | 2304 |
| 1536 MiB | 1024 | 1792 |
| 2048 MiB | 1024 | 1280 |
| 2560+ MiB | 1024 | **1024** |

The log's last **253** reports were all `1024/1024/1024`.

**Why that is the flicker (agent 4):** at the floor `total_used_memory > minimum_memory` every frame,
so the collector runs every frame; in aggressive mode `ticks_to_destroy = 10` frames. Intermittently
used images are deleted and re-created every frame. Measured: **created 6616, deleted 6490,
downloaded 2043** images per 600 frames.

**The 3-second hitch (agent 4):** format 74 is `ASTC_2D_5X5_UNORM` at 6144×6144 (`surface.h:96`). Its
~192 MiB mip chain exceeds `GPU_TRANSCODE_MIB=128`, so it is decoded on the CPU asynchronously; the
10-frame aggressive eviction deletes it before reuse, so it is decoded again — **1280 ms each**.

**The fog (agents 1, 7):** OoT's fog classifier reads its spherical-harmonic light probes through a
pointer table; the records sit **104–122 MiB past the pointer** (Lichii #292). A read-only pool capped
at 64 MiB makes those reads return zero, the fog loses its blue ambient term, and it becomes a white
haze over the scene, Link included. `0202`'s own commit body records that the white haze ranks first
on the tables-off PS5 runs and that tables-on-for-every-stage (`=1`) "died translating mesh programs."

## 4. Changes made

Four commits, **291** patches in the corpus (287 + `0414`–`0417`):

| Commit | File | Change |
|---|---|---|
| `2468ff585b` | `headless/main.cpp` | `TABLE_POOL_MIB` 64 → **128**; `NATIVE_TABLES` default → **compute**; `OOT_VISUAL_WORKAROUND` baked **0**; `SPARSE_UNBACKED` overwrite **removed** so env.txt can A/B it |
| `de684ebfb2` | `0414-…patch` | Pressure margin recovers from the driver's free figure (half back when margin + 1 GiB free, after 120 frames) instead of one 128 MiB step per 600 event-free frames |
| `083c62b451` | `0415-…patch` | Allocator `huge` decided by the driver's real `VkMemoryRequirements`, not `2^28` texels (Lichii 75afd1a306). `EDEN_NX2_ALLOC_BYTES=0` restores the old filter |
| `083c62b451` | `0416-…patch` | `RefreshContents` skips the upload when the guest range has no CPU mapping — the source of the 17-line `Unmapped Device ReadBlock` burst |
| `083c62b451` → **removed** in `8e20332001` | `0417-…patch` | Ran the query flush through `GpuSplit::RunOnBack`. **Removed after the owner's A/B:** it hangs the game on the **second** pause-menu entry (front drains the back while the back waits on front work), and it made no visible difference to the fog it was written for. Corpus back to **290**. `EDEN_NX2_MACRO_QUERY_FLUSH` is no longer set by default. |

Every patch verified three ways before commit: `git apply --reverse --check` against the patched tree,
`--check` against the restored original, then applied.

**Correction, after CI caught it.** That verification was worthless as first done: it was run against
*this checkout's* submodule tree, which had drifted **27 files** from what a clean checkout plus the
patch series produces — `macro.cpp` was ~140 lines behind, `texture_cache.h` ~140 lines short.
`0414` and `0416` applied anyway by luck; `0417` did not, and CI died in "Fetch dependencies" with
`0417-… does not apply`. The drift's cause was `tools/apply-eglantine-patches.sh` itself: it passed
MSYS paths (`/c/Users/...`) to the native Windows git, which cannot open them, so on this host the
script applied **nothing**. Fixed with `cygpath -m`; the script now runs clean and the tree is
byte-identical to a verified one.

All three patches were then regenerated from a **detached worktree at `0ca2032bff` with `0001..0413`
applied**, and verified by applying the whole series to a second clean tree: **291 applied, 0 failed**.
`0417` was rewritten for the real code — the flush lives inside `owns_caches` in the series, not in the
`flush_queries`-gated block the first version patched.

**Lesson: generate patches against a clean base + the series, never against the working tree.**

Also confirmed already shipped: `EDEN_NX2_AMD_SHADER_FIXES=1` is baked at `headless/main.cpp:676`,
so Lichii #432's NaN-safe min/max is the PS5 default already.

Env recipe (`env-oot-memory.txt`) changed 2 lines: `NATIVE_TABLES` `1` → `compute`; added
`TC_KEEP_ASYNC=1800`. **`env.txt` wins over baked defaults**, so the file edit is the actual lever.

## 5. Measured vs guess

| Change | Status |
|---|---|
| Threshold walk-down as the cause of churn | **MEASURED** (formula reproduces the log) |
| Fog recipe (compute + 128 MiB + workaround off) | **PS5-tested by viejunerd**, not by us |
| `TC_KEEP_ASYNC=1800` stops the re-decode | **GUESS** (mechanism source-proven, value not) |
| `0414` improves flicker | **GUESS in effect.** Cause proven; recovery rate not measured |
| `SPARSE_UNBACKED=0` fixes black water | **HYPOTHESIS** — now switchable to settle it |
| Emulator crash in the log | **DISPROVED** — there is none |

## 6. Still open

Shipped in this round: the allocator byte policy (`0415`), the unmapped-read guard (`0416`), the
split-safe query flush (`0417`), and Lichii #432 as a baked default.

Not shipped, with reasons:

1. **Mesh descriptor fallback** (Lichii #569, #814). The current PS5 fallback is built on
   `bufferDeviceAddress`, which RADV does not report, so 27 work-producing mesh pipelines are
   dropped. The fix is a rewrite across `vk_mesh_compute`, `runtime_info`, `vk_pipeline_cache`,
   `vk_graphics_pipeline`, `spirv_emit_context`, `emit_spirv_mesh_readback` and
   `mesh_compute_scan.comp` — replacing the physical-storage-buffer path with a synthetic storage
   buffer binding. **This cannot be authored blind and shipped: a wrong binding index or scratch
   offset shows as wrong or missing vertices, and a missing reuse barrier reproduces frame-to-frame
   flicker — the exact symptom it is meant to fix.** It needs its own round with a build to test
   against. `janvitos/Lichii#814` is a policy change on top of #569, not a substitute.
2. **Eglantine #250 collector caps** (60% expected / 75% critical). Held: it caps after all override
   paths, so it would fight the deliberate `TC_EXPECTED_MIB`/`TC_CRITICAL_MIB` tuning.
3. **Lichii #434** (64 storage buffers + compute rebuild) — a large fresh port.
4. **Lichii #440** (nullable image allocation with retry) — allocator and texture-cache rework.
5. **Lichii #421** (counted-set table budget) and **Fritchii #778**'s staging retention, cached
   Maxwell settings, fence profiling and `speed_limit` diagnostic.
6. **Eglantine #211's scalar valid-mapped test** — log-noise only; block zero-fill is already present.

