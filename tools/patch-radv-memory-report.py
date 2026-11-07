#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the console's RADV holds of the shared pool, by purpose (diagnostic, opt-in).

Adapted from tools/patch-radv-memory-report.py of heydemoura/EglantinePS5, commit
f25ee25ffee1a7ab3a7c3768a42a959d8e398c80 (Heyde Moura, 2026-11-05). That report tallies by RADV's
buffer priority. Priority 30 is descriptor pools, command buffers' upload buffers, fences and geometry
shader heaps together, and priority 31 shaders, scratch and the rings, so this one tells them apart by
domain and flags,
and adds what the priorities leave out: peaks, largest buffers, how many were ever made, memory held
for the GPU after a free, and sparse ranges. Unlike the original it starts from the pinned source
each time, so a change here reaches the driver.

With RADV_PS5_MEMORY_REPORT=1 (env.txt) a line goes to stderr at most every 10 s and with every
EDEN_POOL_LEDGER, so at every refusal too:

  RADV_PS5_MEMORY t_s=<since the first line> pool_mib= available_mib=<largest free> driver_mib=
      retired_mib=<freed, waiting for the GPU> sparse_va_mib=<address space>/<ranges>
      <purpose>=<KiB>/<buffers>/<peak KiB>/<largest KiB>/<ever made> ...

Purposes: app (VkDeviceMemory, so VMA's blocks), query, descriptor (descriptor pools), upload
(command buffers' upload buffers), fence, gs (a command buffer's heap for geometry shaders run as
compute: 64 MiB, kept until the command buffer goes), shader (arenas), mesh (the emulated task and
publish memory, 32-bit and 16 MiB or more), rings (scratch and the queue's rings). The submission
ring and the display's memory are not buffers and are in none of these. Without the variable it
only keeps the counters. Patches the build export, never the Mesa source checkout. Runs after
tools/patch-radv-wsi.py, whose adaptation of radv_physical_device.c it builds on.
"""
from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[1]
fingerprint = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
if sys.argv[1:2] == ['--check']:
    stamp = Path(sys.argv[2])
    raise SystemExit(0 if stamp.is_file() and stamp.read_text().strip() == fingerprint else 1)
if sys.argv[1:2] == ['--digest']:
    print(fingerprint)
    raise SystemExit(0)
source = root.parent/'mihawk-vulkan-review/.deps/work/radv-src'
pristine = root.parent/'mihawk-mesa-review'
series = root.parent/'mihawk-vulkan-review/.deps/work/radv-series'  # tools/patch-radv-series.py
MARK = 'EglantinePS5 memory report'
# tools/patch-radv-wsi.py rewrites these from the pinned source and adapts them: taken from the export.
ADAPTED_FIRST = {'src/amd/vulkan/radv_physical_device.c'}

COUNTERS = r'''uint64_t radv_ps5_allocated_bytes;

/* ''' + MARK + r''' (tools/patch-radv-memory-report.py): what the driver holds of the pool by
 * purpose. Priority 30 is descriptor pools, upload buffers, fences and geometry shader heaps;
 * priority 31 shaders, scratch and rings. */
enum {
   RADV_PS5_REPORT_APP,
   RADV_PS5_REPORT_QUERY,
   RADV_PS5_REPORT_DESCRIPTOR,
   RADV_PS5_REPORT_UPLOAD,
   RADV_PS5_REPORT_FENCE,
   RADV_PS5_REPORT_GS,
   RADV_PS5_REPORT_SHADER,
   RADV_PS5_REPORT_MESH,
   RADV_PS5_REPORT_RINGS,
   RADV_PS5_REPORT_PURPOSES,
};
static const char *const radv_ps5_report_names[RADV_PS5_REPORT_PURPOSES] = {
   "app", "query", "descriptor", "upload", "fence", "gs", "shader", "mesh", "rings",
};
static uint64_t radv_ps5_report_bytes[RADV_PS5_REPORT_PURPOSES];
static int32_t radv_ps5_report_buffers[RADV_PS5_REPORT_PURPOSES];
static uint64_t radv_ps5_report_peak[RADV_PS5_REPORT_PURPOSES];
static uint64_t radv_ps5_report_largest[RADV_PS5_REPORT_PURPOSES];
static uint64_t radv_ps5_report_made[RADV_PS5_REPORT_PURPOSES];
static uint64_t radv_ps5_retired_bytes;
static uint64_t radv_ps5_sparse_bytes;
static int32_t radv_ps5_sparse_ranges;

static unsigned
radv_ps5_report_purpose(unsigned priority, enum radeon_bo_domain domain, enum radeon_bo_flag flags, uint64_t bytes)
{
   if (priority <= RADV_BO_PRIORITY_APPLICATION_MAX)
      return RADV_PS5_REPORT_APP;
   if (priority == RADV_BO_PRIORITY_QUERY_POOL)
      return RADV_PS5_REPORT_QUERY;
   if (priority == RADV_BO_PRIORITY_DESCRIPTOR) {
      if ((domain & RADEON_DOMAIN_VRAM) && (flags & RADEON_FLAG_READ_ONLY))
         return RADV_PS5_REPORT_DESCRIPTOR;
      if (flags & RADEON_FLAG_GTT_WC)
         return RADV_PS5_REPORT_UPLOAD;
      /* RADV_GS_COMPUTE_HEAP_SIZE, 64 MiB; the others without write combining are a few bytes. */
      return bytes >= (UINT64_C(16) << 20) ? RADV_PS5_REPORT_GS : RADV_PS5_REPORT_FENCE;
   }
   if (!(flags & RADEON_FLAG_32BIT))
      return RADV_PS5_REPORT_RINGS;
   /* A shader arena tops out at 8 MiB. */
   return bytes >= (UINT64_C(16) << 20) ? RADV_PS5_REPORT_MESH : RADV_PS5_REPORT_SHADER;
}

static void
radv_ps5_report_add(unsigned purpose, uint64_t bytes)
{
   const uint64_t held = p_atomic_add_return(&radv_ps5_report_bytes[purpose], bytes);
   p_atomic_inc(&radv_ps5_report_buffers[purpose]);
   p_atomic_inc(&radv_ps5_report_made[purpose]);
   if (held > p_atomic_read(&radv_ps5_report_peak[purpose]))
      p_atomic_set(&radv_ps5_report_peak[purpose], held);
   if (bytes > p_atomic_read(&radv_ps5_report_largest[purpose]))
      p_atomic_set(&radv_ps5_report_largest[purpose], bytes);
}

static void
radv_ps5_report_remove(unsigned purpose, uint64_t bytes)
{
   p_atomic_add(&radv_ps5_report_bytes[purpose], -(int64_t)bytes);
   p_atomic_dec(&radv_ps5_report_buffers[purpose]);
}

static bool
radv_ps5_report_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *const value = getenv("RADV_PS5_MEMORY_REPORT");
      enabled = value && value[0] == '1';
   }
   return enabled;
}

static void
radv_ps5_report_print(uint64_t pool, uint64_t available, uint64_t driver, int64_t now)
{
   static int64_t first_ns;
   if (!p_atomic_read(&first_ns))
      p_atomic_set(&first_ns, now);
   char line[1024];
   int length = snprintf(line, sizeof(line),
                         "RADV_PS5_MEMORY t_s=%lld pool_mib=%llu available_mib=%llu driver_mib=%llu retired_mib=%llu "
                         "sparse_va_mib=%llu/%d",
                         (long long)((now - p_atomic_read(&first_ns)) / INT64_C(1000000000)),
                         (unsigned long long)(pool >> 20), (unsigned long long)(available >> 20),
                         (unsigned long long)(driver >> 20),
                         (unsigned long long)(p_atomic_read(&radv_ps5_retired_bytes) >> 20),
                         (unsigned long long)(p_atomic_read(&radv_ps5_sparse_bytes) >> 20),
                         (int)p_atomic_read(&radv_ps5_sparse_ranges));
   for (unsigned i = 0; i < RADV_PS5_REPORT_PURPOSES && length > 0 && length < (int)sizeof(line) - 80; ++i) {
      if (!p_atomic_read(&radv_ps5_report_made[i]))
         continue;
      length += snprintf(line + length, sizeof(line) - length, " %s=%llu/%d/%llu/%llu/%llu", radv_ps5_report_names[i],
                         (unsigned long long)(p_atomic_read(&radv_ps5_report_bytes[i]) >> 10),
                         (int)p_atomic_read(&radv_ps5_report_buffers[i]),
                         (unsigned long long)(p_atomic_read(&radv_ps5_report_peak[i]) >> 10),
                         (unsigned long long)(p_atomic_read(&radv_ps5_report_largest[i]) >> 10),
                         (unsigned long long)p_atomic_read(&radv_ps5_report_made[i]));
   }
   fprintf(stderr, "%s\n", line);
}

/* From the memory budget query, at most every 10 s. */
void
radv_ps5_memory_report(uint64_t pool, uint64_t available, uint64_t driver)
{
   static int64_t last_ns;
   if (!radv_ps5_report_enabled())
      return;
   const int64_t now = os_time_get_nano();
   const int64_t previous = p_atomic_read(&last_ns);
   if (previous && now - previous < INT64_C(10000000000))
      return;
   if (p_atomic_cmpxchg(&last_ns, previous, now) != previous)
      return;
   radv_ps5_report_print(pool, available, driver, now);
}

/* With the app's pool ledger (headless/performance.cpp), beside what the other owners hold. */
void
radv_ps5_memory_report_now(void)
{
   if (radv_ps5_report_enabled())
      radv_ps5_report_print(radv_ps5_memory_pool_bytes(), radv_ps5_memory_available_bytes(),
                            p_atomic_read(&radv_ps5_allocated_bytes), os_time_get_nano());
}
'''

edits = {
    'src/amd/vulkan/winsys/ps5/radv_ps5_winsys.h': [(
        '   bool imported;\n};\n',
        '   bool imported;\n   /* What the buffer is for (' + MARK + '). */\n   uint8_t report_purpose;\n};\n')],
    'src/amd/vulkan/winsys/ps5/radv_ps5_winsys_public.h': [(
        'void radv_ps5_winsys_heap_usage(uint64_t *pool_bytes, uint64_t *available_bytes, uint64_t *driver_bytes);\n',
        'void radv_ps5_winsys_heap_usage(uint64_t *pool_bytes, uint64_t *available_bytes, uint64_t *driver_bytes);\n\n'
        '/* ' + MARK + ' (RADV_PS5_MEMORY_REPORT=1): at most every 10 s, and at once. */\n'
        'void radv_ps5_memory_report(uint64_t pool, uint64_t available, uint64_t driver);\n'
        'void radv_ps5_memory_report_now(void);\n')],
    'src/amd/vulkan/winsys/ps5/radv_ps5_bo.c': [
        ('uint64_t radv_ps5_allocated_bytes;\n', COUNTERS),
        ('   p_atomic_add(&radv_ps5_allocated_bytes, bo->memory.bytes);\n',
         '   p_atomic_add(&radv_ps5_allocated_bytes, bo->memory.bytes);\n'
         '   bo->report_purpose = radv_ps5_report_purpose(priority, domain, flags, bo->memory.bytes);\n'
         '   radv_ps5_report_add(bo->report_purpose, bo->memory.bytes);\n'),
        ('   p_atomic_add(&radv_ps5_allocated_bytes, -(int64_t)bo->memory.bytes);\n',
         '   p_atomic_add(&radv_ps5_allocated_bytes, -(int64_t)bo->memory.bytes);\n'
         '   radv_ps5_report_remove(bo->report_purpose, bo->memory.bytes);\n'),
        ('      bo->sparse = true;\n',
         '      bo->sparse = true;\n'
         '      p_atomic_add(&radv_ps5_sparse_bytes, bo->memory.bytes);\n'
         '      p_atomic_inc(&radv_ps5_sparse_ranges);\n'),
        ('   if (bo->sparse) {\n      radv_ps5_memory_retire(ws->queue, &bo->memory, true);\n',
         '   if (bo->sparse) {\n'
         '      p_atomic_add(&radv_ps5_sparse_bytes, -(int64_t)bo->memory.bytes);\n'
         '      p_atomic_dec(&radv_ps5_sparse_ranges);\n'
         '      radv_ps5_memory_retire(ws->queue, &bo->memory, true);\n'),
        ('      util_dynarray_append(&radv_ps5_retired, entry);\n',
         '      util_dynarray_append(&radv_ps5_retired, entry);\n'
         '      if (!sparse)\n'
         '         p_atomic_add(&radv_ps5_retired_bytes, memory->bytes);\n'),
        ('         radv_ps5_memory_release(&entry->memory, entry->sparse);\n',
         '         if (!entry->sparse)\n'
         '            p_atomic_add(&radv_ps5_retired_bytes, -(int64_t)entry->memory.bytes);\n'
         '         radv_ps5_memory_release(&entry->memory, entry->sparse);\n'),
    ],
    'src/amd/vulkan/radv_physical_device.c': [(
        '   radv_ps5_winsys_heap_usage(&pool, &available, &driver);\n',
        '   radv_ps5_winsys_heap_usage(&pool, &available, &driver);\n'
        '   radv_ps5_memory_report(pool, available, driver); /* ' + MARK + ' */\n')],
}

changed = False
for relative, replacements in edits.items():
    path = source/relative
    start = series/relative if (series/relative).exists() else pristine/relative
    text = (path if relative in ADAPTED_FIRST else start).read_text()
    if MARK not in text:
        for old, new in replacements:
            if text.count(old) != 1:
                raise SystemExit(f'{relative}: pinned anchor changed: {old.splitlines()[0]!r}')
            text = text.replace(old, new)
    if path.read_text() != text:
        path.write_text(text)
        changed = True
if changed:
    # As tools/patch-radv-wsi.py: Ninja keeps every unchanged object and rebuilds these files.
    receipt = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print(f'RADV memory report: by purpose (RADV_PS5_MEMORY_REPORT=1), changed={changed}')
