/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

#include "gloffscreen.h"

#if HAVE_EXTERNAL_MEMORY
static GloContext *g_gl_context;
#else
static GLuint g_display_tex;
#endif

/* Set XEMU_FPS=1 to print the display frame rate every 5 seconds. */
static bool g_fps_report;

#define XTRACE_SKIP(label) do { \
    static unsigned long xk_n, xk_next = 1; \
    if (++xk_n >= xk_next) { \
        fprintf(stderr, "xtrace: %s count=%lu\n", (label), xk_n); \
        xk_next *= 10; \
    } \
} while (0)

static void early_context_init(void)
{
    g_fps_report = getenv("XEMU_FPS") != NULL;
    pgraph_vk_perflog_init();

#if HAVE_EXTERNAL_MEMORY
    g_gl_context = glo_context_create();
#endif
}

static void pgraph_vk_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->vk_renderer_state = (PGRAPHVkState *)g_malloc0(sizeof(PGRAPHVkState));
    pg->vk_renderer_state->surface_general_layout =
        pgraph_vk_env_opt_in("XEMU_SURF_TEX_SAMPLE");
    if (pg->vk_renderer_state->surface_general_layout) {
        fprintf(stderr, "vk: color surfaces in GENERAL layout for direct "
                        "sampling\n");
    }

#if HAVE_EXTERNAL_MEMORY
    glo_set_current(g_gl_context);
#endif

    pgraph_vk_debug_init();

    pgraph_vk_init_instance(pg, errp);
    if (*errp) {
        return;
    }

    pgraph_vk_init_command_buffers(pg);
    pgraph_vk_init_buffers(d);
    pgraph_vk_init_surfaces(pg);
    pgraph_vk_init_shaders(pg);
    pgraph_vk_init_pipelines(pg);
    pgraph_vk_init_textures(pg);
    pgraph_vk_init_reports(pg);
    pgraph_vk_init_compute(pg);
    pgraph_vk_init_display(pg);

    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                   memory_region_size(d->vram));

    pgraph_vk_determine_gpu_properties(d);
}

static void pgraph_vk_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finalize_display(pg);
    pgraph_vk_finalize_compute(pg);
    pgraph_vk_finalize_reports(pg);
    pgraph_vk_finalize_textures(pg);
    pgraph_vk_finalize_pipelines(pg);
    pgraph_vk_finalize_shaders(pg);
    pgraph_vk_finalize_surfaces(pg);
    pgraph_vk_finalize_buffers(d);
    pgraph_vk_finalize_command_buffers(pg);
    pgraph_vk_finalize_instance(pg);

    g_free(pg->vk_renderer_state);
    pg->vk_renderer_state = NULL;
}

static void pgraph_vk_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    /* This invalidates the whole of VRAM: every texture is marked dirty and
     * the entire vertex mirror is re-uploaded. Record how often it happens so
     * the cost is known rather than assumed. */
    pgraph_vk_perflog_event("pgraph_flush", 0);

    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    pgraph_vk_surface_flush(d);
    pgraph_vk_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    for (int i = 0; i < 4; i++) {
        pg->texture_dirty[i] = true;
    }

    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_vk_sync(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    pgraph_vk_render_display(pg);

    /* Present from here rather than from the UI thread: this is the thread
     * that owns the queue. No-op unless the swapchain backend is in use. */
    pgraph_vk_present_pending_init(pg);
    pgraph_vk_present_frame(pg);

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_vk_process_pending(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)
    ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_vk_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_vk_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_vk_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_vk_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

/* Presenting only when the UI thread comes asking drops frames whenever this
 * thread is buried in a long stretch of guest work: the guest, decoupled by
 * the deferred flip wait, keeps flipping at its native rate while the screen
 * sees just the flips that happen to coincide with a sync request. The flip
 * stall is the natural pacing point - end of a guest frame, on the thread
 * that owns the queue - so composite and present right here, once per flip.
 * The reuse guards make the UI thread's own requests nearly free. */
static bool pgraph_vk_present_on_flip(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = pgraph_vk_env_opt_in("XEMU_PRESENT_ON_FLIP");
        if (cached) {
            fprintf(stderr, "vk: presenting at guest flip time\n");
        }
    }
    return cached == 1;
}

static void pgraph_vk_flip_stall(NV2AState *d)
{
    pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_FLIP_STALL);

    if (pgraph_vk_present_on_flip()) {
        pgraph_vk_render_display(&d->pgraph);
        pgraph_vk_present_frame(&d->pgraph);
    }

    pgraph_vk_debug_frame_terminator();
}

static void pgraph_vk_pre_savevm_trigger(NV2AState *d)
{
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_savevm_wait(NV2AState *d)
{
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_shutdown_trigger(NV2AState *d)
{
    // qatomic_set(&d->pgraph.vk_renderer_state->shader_cache_writeback_pending, true);
    // qemu_event_reset(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);
}

static void pgraph_vk_pre_shutdown_wait(NV2AState *d)
{
    // qemu_event_wait(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);   
}

static bool pgraph_vk_framebuffer_is_top_down(NV2AState *d)
{
    /* With external memory the UI samples the Vulkan image directly; without
     * it we upload guest VRAM, which is stored top-down. */
    return !HAVE_EXTERNAL_MEMORY;
}

static int pgraph_vk_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        static unsigned long n, next = 1;
        if (++n >= next) {
            fprintf(stderr, "xtrace: framebuffer MISS count=%lu "
                            "pcrtc.start=0x%lx line_offset=0x%x surface=%p\n",
                    n, (unsigned long)d->pcrtc.start,
                    (unsigned)vga_display_params.line_offset, (void *)surface);
            next *= 10;
        }
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }
    /* One call per presented frame, so this is the display frame rate. */
    if (g_fps_report) {
        static int64_t window_start_ns;
        static unsigned frames;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        frames++;
        if (window_start_ns == 0) {
            window_start_ns = now;
        } else if (now - window_start_ns >= 5 * NANOSECONDS_PER_SECOND) {
            double secs = (double)(now - window_start_ns) / NANOSECONDS_PER_SECOND;
            fprintf(stderr, "xemu-fps: %.1f fps (%ux%u)\n", frames / secs,
                    surface->width, surface->height);
            window_start_ns = now;
            frames = 0;
        }
    }

    assert(surface->color);

    surface->frame_time = pg->frame_time;

#if !HAVE_EXTERNAL_MEMORY
    /* The UI presents at the host refresh rate, which is faster than the
     * guest produces frames, so the same image is often requested several
     * times. Downloading it again costs a GPU stall plus a full-frame copy in
     * each direction, so reuse the texture until the surface is drawn to
     * again or the guest flips to a different buffer. */
    {
        static unsigned last_draw_time;
        static hwaddr last_vram_addr;
        static bool have_cached;

        bool unchanged = have_cached && g_display_tex &&
                         surface->draw_time == last_draw_time &&
                         surface->vram_addr == last_vram_addr;

        last_draw_time = surface->draw_time;
        last_vram_addr = surface->vram_addr;
        have_cached = true;

        if (unchanged) {
            if (g_fps_report) {
                XTRACE_SKIP("present_reused_texture");
            }
            qemu_mutex_unlock(&d->pfifo.lock);
            return g_display_tex;
        }
    }
#endif

#if HAVE_EXTERNAL_MEMORY
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);
    return r->display.gl_texture_id;
#else
    qemu_mutex_unlock(&d->pfifo.lock);

    /* Time the download and the upload separately: the display path runs at
     * the host refresh rate, which is faster than the guest produces frames,
     * so this is where redundant work would show up. */
    int64_t t0 = 0, t1 = 0, t2 = 0;
    bool timing = g_fps_report || pgraph_vk_perflog_enabled();
    if (timing) {
        t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }

    // The download is skipped unless the surface is flagged dirty, and the
    // flag is cleared once downloaded. Without interop we need current pixels
    // in guest memory, so ask for them -- but do not stall waiting. The work
    // happens on the pfifo thread, and blocking here costs the scheduling
    // round trip plus the GPU sync. If the pixels are not ready yet, present
    // the texture from the previous frame: one frame of latency instead of a
    // stall that was measured at a third of the frame budget.
    // The download is skipped unless the surface is flagged dirty, and the
    // flag is cleared once downloaded. Without interop we need current pixels
    // in guest memory every frame, so request one each time.
    //
    // A non-blocking variant was tried here: request the download and present
    // the previous frame rather than waiting. It removed the download cost
    // entirely but cut the frame rate from 16.6 to 6.3 fps, because asking
    // every other frame defeats the unchanged-surface reuse below and keeps
    // the pfifo thread synchronising constantly. Measured, not assumed.
    qatomic_set(&surface->draw_dirty, true);
    pgraph_vk_wait_for_surface_download(surface);

    if (timing) {
        t1 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }


    // Without GL/Vulkan external memory interop the surface is downloaded to
    // guest VRAM, so upload it to a plain texture using the UI's context.
    if (!g_display_tex) {
        glGenTextures(1, &g_display_tex);
        glBindTexture(GL_TEXTURE_2D, g_display_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, g_display_tex);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, surface->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, surface->width, surface->height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, d->vram_ptr + surface->vram_addr);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (pgraph_vk_perflog_enabled()) {
        int64_t tp = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        pgraph_vk_perflog_frame((double)(t1 - t0) / 1000000.0,
                                (double)(tp - t1) / 1000000.0,
                                r->texture_cache_bytes, surface->width,
                                surface->height);
    }

    if (g_fps_report) {
        static int64_t win_ns, dl_ns, ul_ns;
        static unsigned n;
        t2 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        dl_ns += t1 - t0;
        ul_ns += t2 - t1;
        n++;
        if (win_ns == 0) {
            win_ns = t2;
        } else if (t2 - win_ns >= 5 * NANOSECONDS_PER_SECOND) {
            fprintf(stderr,
                    "xemu-present: %u frames, download %.2f ms/frame, "
                    "upload %.2f ms/frame\n",
                    n, dl_ns / 1.0e6 / n, ul_ns / 1.0e6 / n);
            win_ns = t2;
            dl_ns = ul_ns = 0;
            n = 0;
        }
    }

    return g_display_tex;
#endif
}

static PGRAPHRenderer pgraph_vk_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_VULKAN,
    .name = "Vulkan",
    .ops = {
        .init = pgraph_vk_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_vk_finalize,
        .clear_report_value = pgraph_vk_clear_report_value,
        .clear_surface = pgraph_vk_clear_surface,
        .draw_begin = pgraph_vk_draw_begin,
        .draw_end = pgraph_vk_draw_end,
        .flip_stall = pgraph_vk_flip_stall,
        .flush_draw = pgraph_vk_flush_draw,
        .get_report = pgraph_vk_get_report,
        .image_blit = pgraph_vk_image_blit,
        .pre_savevm_trigger = pgraph_vk_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_vk_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_vk_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_vk_pre_shutdown_wait,
        .process_pending = pgraph_vk_process_pending,
        .process_pending_reports = pgraph_vk_process_pending_reports,
        .surface_update = pgraph_vk_surface_update,
        .set_surface_scale_factor = pgraph_vk_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_vk_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_vk_get_framebuffer_surface,
            .framebuffer_is_top_down = pgraph_vk_framebuffer_is_top_down,
        .get_gpu_properties = pgraph_vk_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_vk_renderer);
}

void pgraph_vk_check_memory_budget(PGRAPHState *pg)
{
#if 0 // FIXME
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties const *props;
    vmaGetMemoryProperties(r->allocator, &props);

    g_autofree VmaBudget *budgets = g_malloc_n(props->memoryHeapCount, sizeof(VmaBudget));
    vmaGetHeapBudgets(r->allocator, budgets);

    const float budget_threshold = 0.8;
    bool near_budget = false;

    for (int i = 0; i < props->memoryHeapCount; i++) {
        VmaBudget *b = &budgets[i];
        float use_to_budget_ratio =
            (double)b->statistics.allocationBytes / (double)b->budget;
        NV2A_VK_DPRINTF("Heap %d: used %lu/%lu MiB (%.2f%%)", i,
                        b->statistics.allocationBytes / (1024 * 1024),
                        b->budget / (1024 * 1024), use_to_budget_ratio * 100);
        near_budget |= use_to_budget_ratio > budget_threshold;
    }

    // If any heaps are near budget, free up some resources
    if (near_budget) {
        pgraph_vk_trim_texture_cache(pg);
    }
#endif

#if 0
    char *s;
    vmaBuildStatsString(r->allocator, &s, VK_TRUE);
    puts(s);
    vmaFreeStatsString(r->allocator, s);
#endif
}
