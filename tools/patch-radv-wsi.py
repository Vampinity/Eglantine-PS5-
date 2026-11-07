#!/usr/bin/env python3
"""Apply the pinned Eden display-lifetime adaptation to the Mesa build export."""
from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[1]
source = root.parent/'mihawk-mesa-review/src/vulkan/wsi/wsi_common_videoout.c'
target = root.parent/'mihawk-vulkan-review/.deps/work/radv-src/src/vulkan/wsi/wsi_common_videoout.c'
text = source.read_text()

def replace(old, new):
    global text
    assert text.count(old) == 1, f'Pinned WSI anchor changed: {old[:70]}'
    text = text.replace(old, new)

replace('   bool opened;\n', '   unsigned owners;\n   bool closing, stop;\n   bool opened;\n')
# A set of framebuffers per output size: each remembers the memory to give back.
replace('      uint8_t *buffers;\n   } sets[VIDEOOUT_SIZES];',
        '      uint8_t *buffers;\n      int64_t physical;\n      size_t bytes;\n   } sets[VIDEOOUT_SIZES];')
replace('static int\nsceVideoOutSetFlipRate', 'static int\nsceVideoOutClose(int32_t handle)\n{\n   return 0;\n}\n\nstatic int\nsceVideoOutSetFlipRate')
replace('   uint8_t *buffers = NULL;\n#if defined(__PROSPERO__)\n   int64_t physical = -1;',
        '   uint8_t *buffers = NULL;\n   int64_t physical = -1;\n#if defined(__PROSPERO__)')
replace('   out->sets[index] = (struct wsi_videoout_set){.size = size, .buffers = buffers};',
        '   out->sets[index] = (struct wsi_videoout_set){.size = size, .buffers = buffers, .physical = physical,\n'
        '                                                .bytes = bytes};')
replace('      while (out->queue_count == 0)\n         cnd_wait(&out->changed, &out->lock);',
        '      while (out->queue_count == 0 && !out->stop)\n         cnd_wait(&out->changed, &out->lock);\n      if (out->queue_count == 0 && out->stop) {\n         mtx_unlock(&out->lock);\n         return 0;\n      }')
replace('VkResult\nwsi_display_init_wsi',
        (root/'tools/radv-videoout-lifecycle.inc').read_text()+'\nVkResult\nwsi_display_init_wsi')
# 120 Hz: the app asks for it per game session (headless/display_refresh.h). The pinned source
# decides from the package's param.json, which the app cannot read once it has left the sandbox,
# and for the whole run; the package declares the capability (tools/package-headless-native.sh).
declares = 'videoout_title_declares_high_frame_rate(void)\n{\n'
assert text.count(declares) == 1, 'Pinned WSI anchor changed: the 120 Hz declaration'
body_start = text.index(declares) + len(declares)
body_end = text.index('\n}\n', body_start)
assert 'param.json' in text[body_start:body_end], 'Pinned WSI anchor changed: the 120 Hz declaration reads param.json'
text = (text[:body_start] + '   const char *const asked = getenv("EDEN_VIDEOOUT_120HZ");\n'
        '   return asked != NULL && asked[0] == \'1\';' + text[body_end:])
# The pinned source takes the 120 Hz output back unless a vblank then comes about every 8.3 ms.
# On a display with a variable refresh rate that is wrong: before the first frame is shown it
# idles at the bottom of its range (a vblank every 20.87 ms, 48 Hz, in two reports), the output
# went back to 59.94 Hz, and a game could not have 120 Hz at all. Only a display that stayed at
# 59.94 Hz (a vblank about every 16.7 ms) gives the mode back.
replace('         if (period == 0 || period > VIDEOOUT_HIGH_REFRESH_LIMIT_NS) {',
        '         if (period == 0 || (period > UINT64_C(15000000) && period < UINT64_C(18500000))) {')
# Say so when the display follows, as the pinned source does when it does not.
replace('''                       (double)period / 1e6);
         }
      } else if (supported > 0) {''', '''                       (double)period / 1e6);
         } else {
            fprintf(stderr, "wsi/videoout: 119.88 Hz output, a vblank every %.3f ms\\n", (double)period / 1e6);
         }
      } else if (supported > 0) {''')
replace('   wsi->base.get_support =', '   videoout_ref();\n\n   wsi->base.get_support =')
replace('   if (wsi)\n      vk_free(alloc, wsi);',
        '   if (wsi) {\n      videoout_unref();\n      vk_free(alloc, wsi);\n      wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = NULL;\n   }')
# Preserve the reason behind RADV's successful-but-empty device enumeration.
# These startup-only diagnostics avoid repeated full game runs for each stage.
DRIVER_ADAPTATIONS = {
    # Restore the pinned upload path: explicit CPU flush did not affect HUD corruption.
    'src/amd/vulkan/radv_cmd_buffer.c': [
        # EglantinePS5: no stream split inside an emulated task draw. Its chunks' records prepass
        # is a dispatch (radv_before_dispatch splits), after which the chunk's mesh draw went out
        # without the graphics state the split reset. The split stays due and falls before the
        # next draw or dispatch outside the task draw (about 512K words of headroom are left).
        ('static void\nradv_split_cmd_stream_if_due(struct radv_cmd_buffer *cmd_buffer)\n{',
         'static _Thread_local unsigned radv_task_emu_depth;\n\nstatic void\nradv_split_cmd_stream_if_due(struct radv_cmd_buffer *cmd_buffer)\n{'),
        ('   if (likely(!device->ws->cs_split_due) || !device->ws->cs_split_due(cs->b))\n      return;',
         '   if (likely(!device->ws->cs_split_due) || !device->ws->cs_split_due(cs->b))\n      return;\n   if (radv_task_emu_depth)\n      return;'),
        ('static void\nradv_task_emu_draw_direct(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)\n{',
         'static void\nradv_task_emu_draw_direct_body(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z);\n\n'
         'static void\nradv_task_emu_draw_direct(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)\n{\n'
         '   radv_task_emu_depth++;\n   radv_task_emu_draw_direct_body(cmd_buffer, x, y, z);\n   radv_task_emu_depth--;\n}\n\n'
         'static void\nradv_task_emu_draw_direct_body(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)\n{'),
        ('static void\nradv_task_emu_draw_indirect(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)\n{',
         'static void\nradv_task_emu_draw_indirect_body(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info);\n\n'
         'static void\nradv_task_emu_draw_indirect(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)\n{\n'
         '   radv_task_emu_depth++;\n   radv_task_emu_draw_indirect_body(cmd_buffer, info);\n   radv_task_emu_depth--;\n}\n\n'
         'static void\nradv_task_emu_draw_indirect_body(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)\n{'),
        # The same for a draw whose mesh shader runs as compute (tools/patch-radv-series): its chunks'
        # records pass is a dispatch too, and a split there would leave its copy draws without the
        # graphics state. exploitizm's build has no such guard. The split that falls due goes first,
        # where radv_before_taskmesh_draw puts it for an emulated task draw: the only split point of
        # this draw is the radv_before_draw inside the guard, and a run of these draws alone would
        # otherwise never split and pass the submission limit.
        ('static void\nradv_mesh_compute_draw(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t x, uint32_t y,\n                       uint32_t z)\n{',
         'static void\nradv_mesh_compute_draw_body(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t x, uint32_t y,\n                            uint32_t z);\n\n'
         'static void\nradv_mesh_compute_draw(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t x, uint32_t y,\n                       uint32_t z)\n{\n'
         '   radv_split_cmd_stream_if_due(cmd_buffer);\n   radv_task_emu_depth++;\n   radv_mesh_compute_draw_body(cmd_buffer, info, x, y, z);\n   radv_task_emu_depth--;\n}\n\n'
         'static void\nradv_mesh_compute_draw_body(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t x, uint32_t y,\n                            uint32_t z)\n{'),
        # EglantinePS5: a command buffer's upload buffer (push descriptors, push constants, vertex
        # descriptors) only ever doubles and stays at the largest size any recording needed, and the
        # pool cycles through every command buffer it ever made. With EDEN_RADV_UPLOAD_TRIM_KIB=<n> one over
        # n KiB that 64 recordings in a row used less than a quarter of is given back at reset; the
        # next recording starts small and grows as before. Heyde Moura measured these buffers and the
        # descriptor pools at 256 MiB and rising on Mario Kart World (EglantinePS5 f25ee25ffe).
        ('static void\nradv_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer, UNUSED VkCommandBufferResetFlags flags)\n{',
         '''#include <stdlib.h>

static void
eden_upload_trim(struct radv_cmd_buffer *cmd_buffer)
{
   static int64_t limit = -1;
   if (limit < 0) {
      const char *const value = getenv("EDEN_RADV_UPLOAD_TRIM_KIB");
      limit = value ? (int64_t)strtoull(value, NULL, 10) << 10 : 0;
   }
   if (!limit || !cmd_buffer->upload.upload_bo)
      return;
   if (cmd_buffer->upload.size <= (uint64_t)limit || (uint64_t)cmd_buffer->upload.offset * 4 >= cmd_buffer->upload.size) {
      cmd_buffer->upload.small_recordings = 0;
      return;
   }
   if (++cmd_buffer->upload.small_recordings < 64)
      return;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   radv_rmv_log_command_buffer_bo_destroy(device, cmd_buffer->upload.upload_bo);
   radv_bo_destroy(device, &cmd_buffer->vk.base, cmd_buffer->upload.upload_bo);
   cmd_buffer->upload.upload_bo = NULL;
   cmd_buffer->upload.map = NULL;
   cmd_buffer->upload.size = 0;
   cmd_buffer->upload.small_recordings = 0;
}

static void
radv_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer, UNUSED VkCommandBufferResetFlags flags)
{'''),
        ('   if (cmd_buffer->upload.upload_bo)\n      radv_cs_add_buffer(device->ws, cs->b, cmd_buffer->upload.upload_bo);\n   cmd_buffer->upload.offset = 0;\n',
         '   eden_upload_trim(cmd_buffer);\n'
         '   if (cmd_buffer->upload.upload_bo)\n      radv_cs_add_buffer(device->ws, cs->b, cmd_buffer->upload.upload_bo);\n   cmd_buffer->upload.offset = 0;\n'),
    ],
    'src/amd/vulkan/radv_cmd_buffer.h': [
        ('   struct radeon_winsys_bo *upload_bo;\n   struct list_head list;\n};\n',
         '   struct radeon_winsys_bo *upload_bo;\n   struct list_head list;\n'
         '   /* EglantinePS5: recordings in a row that used under a quarter of it (eden_upload_trim). */\n'
         '   unsigned small_recordings;\n};\n'),
    ],
    'src/amd/vulkan/radv_physical_device.c': [
        ('   if (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_INITIALIZATION_FAILED)\n      return VK_SUCCESS;',
         '   if (result != VK_SUCCESS) fprintf(stderr, "radv/ps5: physical device creation result=%d\\n", result);\n   if (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_INITIALIZATION_FAILED)\n      return VK_SUCCESS;'),
        ('   if (!pdev->addrlib) {', '   if (!pdev->addrlib) {\n      fprintf(stderr, "radv/ps5: addrlib creation failed\\n");'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_winsys.c': [
        ('   if (!radv_ps5_platform_init())\n      return;', '   if (!radv_ps5_platform_init()) {\n      fprintf(stderr, "radv/ps5: queue platform initialization failed\\n");\n      return;\n   }'),
        ('   if (!radv_ps5_memory_alloc(RADV_PS5_RING_BYTES, RADV_PS5_LARGE_BYTES, true, &queue->ring))\n      return;', '   if (!radv_ps5_memory_alloc(RADV_PS5_RING_BYTES, RADV_PS5_LARGE_BYTES, true, &queue->ring)) {\n      fprintf(stderr, "radv/ps5: queue ring allocation failed\\n");\n      return;\n   }'),
        ('      radv_ps5_memory_free(&queue->ring);', '      fprintf(stderr, "radv/ps5: queue marker allocation failed\\n");\n      radv_ps5_memory_free(&queue->ring);'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': [
        ('''bool
radv_ps5_memory_grant_gpu(void *address, uint64_t bytes)
{
   /* A title's anonymous memory takes GPU access this way, and a shader then
    * writes it through the CPU's address (HARDWARE_FINDINGS.md, 2026-09-27). */
   return sceKernelMprotect(address, bytes,
                            PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ |
                               PS5_KERNEL_PROT_GPU_WRITE) == 0;
}''', (root/'tools/radv-host-memory.inc').read_text().strip()),
        ('   if (!placed) {', '   if (!placed) {\n      fprintf(stderr, "radv/ps5: mapping rejected result=%d address=%p bytes=%llu window32=%d\\n", result, address, (unsigned long long)bytes, window32);'),
        # EglantinePS5: Mario Kart World on next16 was refused a 343 MiB image with 1886 MiB free
        # in one piece. The direct memory was allocated and the kernel refused to map it, and the
        # sparse range had been refused before that without a word. These say which step it was: no
        # granules in a row, or the kernel (with its result) at the address asked for.
        ('   int32_t result = sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, 0, physical, alignment);\n   if (result != 0 && hint != NULL) {',
         '   int32_t result = sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, 0, physical, alignment);\n'
         '   if (result != 0)\n'
         '      fprintf(stderr, "radv/ps5: mapping of %llu bytes at %p refused: 0x%08x (at 0: no %u granules in a row)\\n",\n'
         '              (unsigned long long)bytes, hint, (unsigned)result, (unsigned)DIV_ROUND_UP(bytes, RADV_PS5_LARGE_BYTES));\n'
         '   if (result != 0 && hint != NULL) {'),
        ('   void *address = at;\n   return sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, PS5_KERNEL_MAP_FIXED, physical,\n'
         '                                   RADV_PS5_PAGE_BYTES) == 0 &&\n          address == at;',
         '   void *address = at;\n'
         '   const int32_t result = sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, PS5_KERNEL_MAP_FIXED, physical,\n'
         '                                                   RADV_PS5_PAGE_BYTES);\n'
         '   static uint32_t reported;\n'
         '   if (result != 0 && p_atomic_inc_return(&reported) <= 16)\n'
         '      fprintf(stderr, "radv/ps5: fixed mapping of %llu bytes at %p refused: 0x%08x\\n", (unsigned long long)bytes,\n'
         '              (void *)at, (unsigned)result);\n'
         '   return result == 0 && address == at;'),
        ('      if (granule == UINT32_MAX)\n         return false;\n   }\n   uint8_t *const va = (uint8_t *)(uintptr_t)(RADV_PS5_REGION_BASE + (uint64_t)granule * RADV_PS5_LARGE_BYTES);',
         '      if (granule == UINT32_MAX) {\n'
         '         fprintf(stderr, "radv/ps5: sparse range of %u granules refused: none free in a row\\n", (unsigned)granules);\n'
         '         return false;\n'
         '      }\n'
         '   }\n   uint8_t *const va = (uint8_t *)(uintptr_t)(RADV_PS5_REGION_BASE + (uint64_t)granule * RADV_PS5_LARGE_BYTES);'),
        # EglantinePS5: every out-of-memory ending left 60 to 240 MiB free in 100 to 450 pieces, in most
        # runs nearly all under 2 MiB (40-53), and the pieces fallback takes only whole 2 MiB blocks, so a
        # 16 MiB buffer was refused with 125 MiB free. When no 2 MiB block is left it now takes what
        # there is, 16 KiB at a time: a single small buffer is placed that way already. At most
        # RADV_PS5_MAX_CHUNKS pieces, as before. EDEN_RADV_FINE_PIECES=0 keeps the 2 MiB pieces alone.
        ('   if (sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, 0, physical, RADV_PS5_LARGE_BYTES) != 0)\n      return false;\n   if (address == at)',
         '   const uint64_t grain = (((uintptr_t)at | bytes) % RADV_PS5_LARGE_BYTES) ? RADV_PS5_PAGE_BYTES : RADV_PS5_LARGE_BYTES;\n'
         '   if (sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, 0, physical, grain) != 0)\n      return false;\n   if (address == at)'),
        ('''      if (sceKernelAvailableDirectMemorySize(0, pool, RADV_PS5_LARGE_BYTES, &start, &largest) != 0)
         break;
      const uint64_t take = MIN2(bytes - offset, (uint64_t)largest & ~(RADV_PS5_LARGE_BYTES - 1));
      if (take == 0)
         break;
      int64_t physical = -1;
      if (sceKernelAllocateDirectMemory(0, pool, take, RADV_PS5_LARGE_BYTES, RADV_PS5_DIRECT_TYPE, &physical) != 0) {''',
         '''      uint64_t grain = RADV_PS5_LARGE_BYTES, take = 0;
      if (sceKernelAvailableDirectMemorySize(0, pool, grain, &start, &largest) == 0)
         take = MIN2(bytes - offset, (uint64_t)largest & ~(grain - 1));
      if (take == 0) {
         const char *const fine = getenv("EDEN_RADV_FINE_PIECES");
         if (fine && fine[0] == '0')
            break;
         grain = RADV_PS5_PAGE_BYTES;
         largest = 0;
         if (sceKernelAvailableDirectMemorySize(0, pool, grain, &start, &largest) != 0)
            break;
         take = MIN2(bytes - offset, (uint64_t)largest & ~(grain - 1));
      }
      if (take == 0)
         break;
      int64_t physical = -1;
      if (sceKernelAllocateDirectMemory(0, pool, take, grain, RADV_PS5_DIRECT_TYPE, &physical) != 0) {'''),
    ],
    # EglantinePS5: a compute pipeline whose shader did not compile or upload was still reported
    # built and still put in the pipeline cache as an object with no shader. Reading that object
    # faults: the pipeline's own init at shader->info (run56, SIGSEGV at 0x10a), and every later
    # lookup of the same hash in radv_compute_pipeline_cache_search, where radv_shader_ref(NULL)
    # touches the reference count at 0x10 (run88, 5 s into the boot replay). The empty object also
    # reaches the application's vulkan_pipelines.bin and the driver's disk cache, so the fault
    # comes back at every boot that asks for that pipeline. Three guards: the compile reports the
    # failure and refuses the pipeline; an empty object is never inserted; an empty object in a
    # cache blob is refused on deserialization, which makes the runtime drop the entry and take the
    # fresh compile (vk_pipeline_cache_lookup_object). Each says so on stderr. Nothing changes for a
    # pipeline with a shader.
    'src/amd/vulkan/radv_pipeline_compute.c': [
        ('   cs_stage.feedback.duration += os_time_get_nano() - stage_start;\n'
         '\n'
         '   if (!skip_shaders_cache) {\n'
         '      radv_pipeline_cache_insert(device, cache, &pipeline->base);\n'
         '   }\n'
         '\n'
         '   free(cs_binary);\n'
         '   if (radv_can_dump_shader_stats(&device->compiler_info, cs_stage.nir)) {\n',
         '   cs_stage.feedback.duration += os_time_get_nano() - stage_start;\n'
         '\n'
         '   if (!pipeline->base.shaders[MESA_SHADER_COMPUTE]) {\n'
         '      fprintf(stderr, "radv/ps5: compute pipeline %016llx not built: %s; refused, not cached\\n",\n'
         '              (unsigned long long)pipeline->base.pipeline_hash,\n'
         '              cs_binary ? "the shader\'s upload failed (shader memory)" : "the compiler gave no binary");\n'
         '      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);\n'
         '   } else if (!skip_shaders_cache) {\n'
         '      radv_pipeline_cache_insert(device, cache, &pipeline->base);\n'
         '   }\n'
         '\n'
         '   free(cs_binary);\n'
         '   if (pipeline->base.shaders[MESA_SHADER_COMPUTE] &&\n'
         '       radv_can_dump_shader_stats(&device->compiler_info, cs_stage.nir)) {\n'),
    ],
    'src/amd/vulkan/radv_pipeline_cache.c': [
        ('#include "radv_pipeline_cache.h"\n',
         '#include "radv_pipeline_cache.h"\n#include <stdio.h>\n'),
        ('   unsigned num_shaders = blob_read_uint32(blob);\n',
         '   unsigned num_shaders = blob_read_uint32(blob);\n'
         '   if (num_shaders == 0) {\n'
         '      fprintf(stderr, "radv/ps5: a cached pipeline object has no shader (written by an earlier run); "\n'
         '                      "dropped, the pipeline is compiled again\\n");\n'
         '      return NULL;\n'
         '   }\n'),
        ('   assert(pipeline_obj->num_shaders == 1);\n',
         '   if (pipeline_obj->num_shaders != 1 || !pipeline_obj->shaders[0]) {\n'
         '      fprintf(stderr, "radv/ps5: cached compute pipeline %016llx has no shader; compiled again\\n",\n'
         '              (unsigned long long)pipeline->base.pipeline_hash);\n'
         '      vk_pipeline_cache_object_unref(&device->vk, &pipeline_obj->base);\n'
         '      return false;\n'
         '   }\n'),
        ('   num_shaders += pipeline->gs_copy_shader ? 1 : 0;\n',
         '   num_shaders += pipeline->gs_copy_shader ? 1 : 0;\n'
         '   if (num_shaders == 0) {\n'
         '      fprintf(stderr, "radv/ps5: pipeline %016llx has no shader, not cached\\n",\n'
         '              (unsigned long long)pipeline->pipeline_hash);\n'
         '      return;\n'
         '   }\n'),
    ],
    # EglantinePS5: task/mesh draws hung the console's GPU (Mario Kart World, Zelda: Ocarina of Time).
    'src/amd/vulkan/radv_shader.c': [
        # EglantinePS5: spirv_to_nir returns NULL when its builder cannot be allocated or the module
        # fails to parse, and radv_shader_spirv_to_nir then wrote nir->info.internal (offset 0x40):
        # SIGSEGV at 0x40 on VkPipelineBuilder with the memory pool short (Zelda). A release build
        # reports parse errors only to a debug-report callback: print them, retry a few times (the
        # pool may have room again), and stop with a message instead of the fault.
        ('static void\nradv_spirv_nir_debug(',
         '#include "util/os_time.h"\n\nstatic void\nradv_spirv_nir_debug('),
        ('   vk_debug_report(debug_data->debug_report, vk_flags[level], debug_data->object, 0, 0, "radv", buffer);\n}',
         '   vk_debug_report(debug_data->debug_report, vk_flags[level], debug_data->object, 0, 0, "radv", buffer);\n'
         '   if (level == NIR_SPIRV_DEBUG_LEVEL_ERROR)\n'
         '      fprintf(stderr, "radv/ps5: %s\\n", buffer);\n}'),
        ('      nir = spirv_to_nir(spirv, stage->spirv.size / 4, spec, stage->stage, stage->entrypoint, &spirv_options,\n'
         '                         &compiler_info->nir_options[stage->stage]);\n'
         '      nir->info.internal |= is_internal;',
         '      nir = spirv_to_nir(spirv, stage->spirv.size / 4, spec, stage->stage, stage->entrypoint, &spirv_options,\n'
         '                         &compiler_info->nir_options[stage->stage]);\n'
         '      for (unsigned attempt = 1; !nir && attempt <= 8; attempt++) {\n'
         '         fprintf(stderr, "radv/ps5: SPIR-V to NIR failed (%s, %u words, entry %s), attempt %u of 8\\n",\n'
         '                 _mesa_shader_stage_to_string(stage->stage), (unsigned)(stage->spirv.size / 4),\n'
         '                 stage->entrypoint ? stage->entrypoint : "?", attempt);\n'
         '         os_time_sleep(50000);\n'
         '         nir = spirv_to_nir(spirv, stage->spirv.size / 4, spec, stage->stage, stage->entrypoint, &spirv_options,\n'
         '                            &compiler_info->nir_options[stage->stage]);\n'
         '      }\n'
         '      if (!nir) {\n'
         '         fprintf(stderr, "radv/ps5: SPIR-V to NIR failed for good: out of memory, or a module the parser "\n'
         '                         "refuses (a radv/ps5: SPIR-V offset line above says which)\\n");\n'
         '         abort();\n'
         '      }\n'
         '      nir->info.internal |= is_internal;'),
    ],
    # EglantinePS5: the emulated task memory (69 MiB) and the publish ring (16 MiB) are made with the
    # device whether or not a mesh draw ever comes, and with EDEN_NX2_NO_MESH none does: every use is
    # in a mesh or task draw (radv_task_emu_begin refuses cleanly without the first). With
    # EDEN_RADV_NO_MESH_BUFFERS=1 and EDEN_NX2_NO_MESH set (the core's test: set at all), and no
    # EDEN_NX2_NO_MESH_FILE, which can turn mesh draws back on, they are not made.
    'src/amd/vulkan/radv_device.c': [
        ('   /* Zeroed, as every draw packet leaves it; its address goes in a record\'s\n    * 32 bits. */\n'
         '   if (device->vk.enabled_features.meshShader && radv_mesh_draw_records_enabled(pdev) &&\n',
         '   const char *const eden_no_mesh_buffers = getenv("EDEN_RADV_NO_MESH_BUFFERS");\n'
         '   const bool eden_mesh_buffers = !(eden_no_mesh_buffers && eden_no_mesh_buffers[0] == \'1\' &&\n'
         '                                    getenv("EDEN_NX2_NO_MESH") && !getenv("EDEN_NX2_NO_MESH_FILE"));\n'
         '   if (!eden_mesh_buffers)\n'
         '      fprintf(stderr, "radv/ps5: no mesh draws in this run: task memory and publish ring not made\\n");\n\n'
         '   /* Zeroed, as every draw packet leaves it; its address goes in a record\'s\n    * 32 bits. */\n'
         '   if (eden_mesh_buffers && device->vk.enabled_features.meshShader && radv_mesh_draw_records_enabled(pdev) &&\n'),
        ('   /* Its records are read through a 32-bit address. */\n'
         '   if (device->vk.enabled_features.taskShader && radv_task_emulated(pdev)) {\n',
         '   /* Its records are read through a 32-bit address. */\n'
         '   if (eden_mesh_buffers && device->vk.enabled_features.taskShader && radv_task_emulated(pdev)) {\n'),
    ],
    'src/amd/vulkan/radv_shader_info.c': [
        # Parts of whole primitives that fit one NGG subgroup's 256 vertices: 256 triangles are
        # 4 parts of 64, not 3 of 86 (258 vertices). ac_nir_lower_ngg_mesh derives each part's
        # primitives from this count (part_prims = DIV_ROUND_UP(max_primitives, prim_parts)).
        ('   return MAX2(DIV_ROUND_UP(nir->info.mesh.max_primitives_out * vertices_per_prim, 256), 1);',
         '   return MAX2(DIV_ROUND_UP(nir->info.mesh.max_primitives_out, 256 / vertices_per_prim), 1);'),
    ],
    'src/amd/common/nir/ac_nir_lower_ngg_mesh.c': [
        # The publish wait gives up after about a second instead of spinning for ever: a wrong
        # count then shows as wrong primitives, not as a lost device.
        ('''      nir_loop *loop = nir_push_loop(b);
      {
         nir_def *header = nir_build_load_global(b, 1, 32, s->header_addr, .align_mul = 4,
                                                 .access = ACCESS_COHERENT | ACCESS_VOLATILE);
         nir_break_if(b, nir_ult_imm(b, nir_isub(b, header, first), count));
         ac_nir_sleep(b, 64);
      }
      nir_pop_loop(b, loop);''',
         '''      nir_variable *spins = nir_local_variable_create(b->impl, glsl_uint_type(), "ms_publish_spins");
      nir_store_var(b, spins, nir_imm_int(b, 0), 0x1);
      nir_loop *loop = nir_push_loop(b);
      {
         nir_def *header = nir_build_load_global(b, 1, 32, s->header_addr, .align_mul = 4,
                                                 .access = ACCESS_COHERENT | ACCESS_VOLATILE);
         nir_break_if(b, nir_ult_imm(b, nir_isub(b, header, first), count));
         nir_def *spun = nir_iadd_imm(b, nir_load_var(b, spins), 1);
         nir_store_var(b, spins, spun, 0x1);
         nir_break_if(b, nir_uge(b, spun, nir_imm_int(b, 1 << 18)));
         ac_nir_sleep(b, 64);
      }
      nir_pop_loop(b, loop);'''),
    ],
}
# The driver is rebuilt when the WSI text's hash changes (--check, tools/prepare-build.sh): the
# driver adaptations' digest goes into that text as a comment, so a change to them rebuilds it too.
# The driver series (tools/patch-radv-series.py) counts the same way.
series = [patch.read_bytes() for patch in sorted((root/'tools/patch-radv-series').glob('*.patch'))]
text += '\n/* EglantinePS5 driver adaptations: ' + hashlib.sha256(repr((sorted(DRIVER_ADAPTATIONS.items()), series)).encode()).hexdigest() + ' */\n'
if sys.argv[1:2] == ['--check']:
    # Is the driver that was built (its recorded display source, EDEN_WSI_SHA256) this adaptation?
    recorded = Path(sys.argv[2]).read_text().split()[0] if Path(sys.argv[2]).is_file() else ''
    sys.exit(0 if recorded == hashlib.sha256(text.encode()).hexdigest() else 1)
changed = not target.exists() or target.read_text() != text
if changed:
    target.write_text(text)
    # Upstream caches the whole build by pin. Invalidate only that receipt;
    # Ninja retains all unchanged objects and rebuilds the patched WSI.
    provenance = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
print(f'RADV WSI adaptation changed={changed} sha256={hashlib.sha256(text.encode()).hexdigest()}')

for relative, replacements in DRIVER_ADAPTATIONS.items():
    # What tools/patch-radv-series.py made of the file, if it touched it.
    original = root.parent/'mihawk-vulkan-review/.deps/work/radv-series'/relative
    if not original.exists():
        original = root.parent/'mihawk-mesa-review'/relative
    destination = root.parent/'mihawk-vulkan-review/.deps/work/radv-src'/relative
    adapted = original.read_text()
    for old, new in replacements:
        assert adapted.count(old) == 1, f'Pinned initialization anchor changed: {relative}'
        adapted = adapted.replace(old, new)
    if destination.read_text() != adapted:
        destination.write_text(adapted)
        provenance = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
        if provenance.exists():
            provenance.replace(provenance.with_suffix('.base.txt'))
    print(f'RADV initialization diagnostic {relative} sha256={hashlib.sha256(adapted.encode()).hexdigest()}')
