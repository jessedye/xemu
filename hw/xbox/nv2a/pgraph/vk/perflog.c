/*
 * Frame timing log for diagnosing stutter.
 *
 * Average frame rate hides the thing that actually spoils play: an occasional
 * frame that takes far longer than the rest. This records per-frame times,
 * reports the distribution rather than just the mean, and writes a line for
 * each individual slow frame together with what the renderer was doing at the
 * time, so a stutter can be attributed after the fact.
 *
 * Set XEMU_PERFLOG to a file path to enable. Unset, the whole thing costs one
 * predicted-not-taken branch per frame.
 *
 * Output is bounded by construction: one summary line per interval, and a
 * capped number of slow-frame lines per session.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "renderer.h"
#include "hw/xbox/nv2a/debug.h"

/* Counters worth carrying into the log, chosen to separate the usual causes of
 * a hitch: shader or pipeline creation (compilation stall), submission and
 * draw volume (the GPU simply has too much to do), and the reasons pgraph had
 * to synchronise (waiting on the host rather than working). */
static const struct {
    enum NV2A_PROF_COUNTERS_ENUM id;
    const char *name;
} k_counters[] = {
    { NV2A_PROF_SHADER_GEN,          "shader_gen" },
    { NV2A_PROF_PIPELINE_GEN,        "pipeline_gen" },
    { NV2A_PROF_QUEUE_SUBMIT,        "queue_submit" },
    /* Auxiliary submissions, each a separate GPU round trip on top of the
     * frame's own submit. Broken out so it is clear which of them is worth
     * folding into the main command buffer. */
    { NV2A_PROF_QUEUE_SUBMIT_AUX,    "submit_aux_total" },
    { NV2A_PROF_QUEUE_SUBMIT_1,      "submit_surface_create" },
    { NV2A_PROF_QUEUE_SUBMIT_2,      "submit_surface_copy" },
    { NV2A_PROF_QUEUE_SUBMIT_3,      "submit_surface_down" },
    { NV2A_PROF_QUEUE_SUBMIT_4,      "submit_texture_upload" },
    { NV2A_PROF_QUEUE_SUBMIT_5,      "submit_display" },
    { NV2A_PROF_DRAW_ARRAYS,         "draw_arrays" },
    { NV2A_PROF_BEGIN_ENDS,          "begin_ends" },
    { NV2A_PROF_TEX_UPLOAD,          "tex_upload" },
    { NV2A_PROF_FINISH_SURFACE_DOWN, "fin_surface_down" },
    { NV2A_PROF_FINISH_FLIP_STALL,   "fin_flip_stall" },
    { NV2A_PROF_FINISH_PRESENTING,   "fin_presenting" },
    { NV2A_PROF_FINISH_STALLED,      "fin_stalled" },
    { NV2A_PROF_FINISH_NEED_BUFFER_SPACE, "fin_buffer_space" },
    /* Which pool actually ran out; fin_buffer_space only says one did. */
    { NV2A_PROF_NBS_UNIFORM_STAGING,  "nbs_uniform" },
    { NV2A_PROF_NBS_DESCRIPTOR_SETS,  "nbs_descriptors" },
    { NV2A_PROF_NBS_FRAMEBUFFERS,     "nbs_framebuffers" },
    { NV2A_PROF_NBS_INLINE_STAGING,   "nbs_inline" },
    { NV2A_PROF_NBS_COMPUTE_TEX,      "nbs_compute_tex" },
    { NV2A_PROF_NBS_COMPUTE_SURF,     "nbs_compute_surf" },
    /* Render-pass structure: on a tiled GPU every pass boundary is a full
     * tile store and reload, so churn here is GPU time. */
    { NV2A_PROF_PIPELINE_RENDERPASSES, "renderpasses" },
    { NV2A_PROF_PIPELINE_BIND,        "pipeline_bind" },
    { NV2A_PROF_CLEAR,                "clears" },
    { NV2A_PROF_QUERY,                "queries" },
    { NV2A_PROF_SURF_TO_TEX,          "surf_to_tex" },
    { NV2A_PROF_SURF_UPLOAD,          "surf_upload" },
    { NV2A_PROF_SURF_DOWNLOAD,        "surf_download" },
    { NV2A_PROF_SURF_SWIZZLE,         "surf_swizzle" },
};
#define NUM_COUNTERS ARRAY_SIZE(k_counters)

/* Board sensors, read once per window rather than per frame. Missing files are
 * skipped, so this stays harmless on hardware that does not expose them. */
static long read_long_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) {
        v = -1;
    }
    fclose(f);
    return v;
}

#define PERFLOG_WINDOW_NS (5 * NANOSECONDS_PER_SECOND)

/* Frame times kept for the current window. At 60 Hz a 5 second window holds
 * about 300, so this is sized with headroom and never allocates. */
#define PERFLOG_MAX_SAMPLES 1024

/* A frame slower than this is recorded individually. The default has to sit
 * above the frame time a title actually targets, or every frame of a 30 fps
 * game (33.3 ms) is flagged and the signal is lost in its own noise. 50 ms is
 * comfortably past both 60 and 30 fps while still catching a visible hitch.
 * Override with XEMU_PERFLOG_STUTTER_MS. */
#define PERFLOG_DEFAULT_STUTTER_MS 50.0

/* Stop writing individual slow-frame lines after this many, so a sustained bad
 * patch cannot fill the disk. The summaries continue regardless. */
#define PERFLOG_MAX_STUTTER_LINES 2000

static struct {
    bool enabled;
    FILE *f;

    int64_t window_start_ns;
    int64_t last_frame_ns;

    double samples[PERFLOG_MAX_SAMPLES];
    unsigned num_samples;
    unsigned dropped_samples;

    /* accumulated over the window */
    double download_ms;
    double upload_ms;
    /* Wall time the emulation thread spent blocked waiting for the GPU,
     * separated by the reason the pipeline had to be drained. */
    double gpu_wait_ms[VK_NUM_FINISH_REASONS];
    /* Fence waits on auxiliary one-shot submissions (uploads, copies,
     * display); previously invisible, hidden inside plain host time. */
    double aux_wait_ms;
    /* Time the GPU spent executing, as reported by the GPU itself. */
    double gpu_busy_ms;
    unsigned stutters;

    unsigned long stutter_lines;
    unsigned long window_index;
    double stutter_ms;

    long counters[NUM_COUNTERS];
} g_perflog;

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static const char *const k_finish_reason_names[VK_NUM_FINISH_REASONS] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = "vertex_buffer_dirty",
    [VK_FINISH_REASON_SURFACE_CREATE] = "surface_create",
    [VK_FINISH_REASON_SURFACE_DOWN] = "surface_down",
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = "need_buffer_space",
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = "framebuffer_dirty",
    [VK_FINISH_REASON_PRESENTING] = "presenting",
    [VK_FINISH_REASON_FLIP_STALL] = "flip_stall",
    [VK_FINISH_REASON_FLUSH] = "flush",
    [VK_FINISH_REASON_STALLED] = "stalled",
};

void pgraph_vk_perflog_init(void)
{
    const char *path = getenv("XEMU_PERFLOG");

    if (!path || !path[0]) {
        return;
    }

    g_perflog.f = fopen(path, "w");
    if (!g_perflog.f) {
        fprintf(stderr, "perflog: could not open %s\n", path);
        return;
    }

    g_perflog.enabled = true;
    setvbuf(g_perflog.f, NULL, _IOLBF, 0);

    g_perflog.stutter_ms = PERFLOG_DEFAULT_STUTTER_MS;
    const char *thresh = getenv("XEMU_PERFLOG_STUTTER_MS");
    if (thresh && thresh[0]) {
        double v = atof(thresh);
        if (v > 0) {
            g_perflog.stutter_ms = v;
        }
    }

    fprintf(g_perflog.f,
            "# xemu frame timing log\n"
            "# kind=window: elapsed_s,frames,fps,frame_ms_avg,frame_ms_p50,"
            "frame_ms_p99,frame_ms_max,fps_1pct_low,stutters,"
            "download_ms_avg,upload_ms_avg,tex_cache_mb,resolution\n"
            ",cpu_temp_c,cpu_mhz"
            "\n"
            "# kind=stutter: elapsed_s,frame_ms,download_ms,upload_ms,"
            "tex_cache_mb,resolution\n"
            "# kind=event:   elapsed_s,what,value\n");

    /* Name the per-frame counter columns so the file is self describing. */
    fprintf(g_perflog.f, "# counters (per frame):");
    for (unsigned i = 0; i < NUM_COUNTERS; i++) {
        fprintf(g_perflog.f, " %s", k_counters[i].name);
    }
    fprintf(g_perflog.f, "\n");

    /* Wall time per frame the emulation thread spent blocked on the GPU,
     * split by what forced the pipeline to drain. */
    fprintf(g_perflog.f, "# gpu_busy_ms (per frame): gpu_busy\n");
    fprintf(g_perflog.f, "# gpu_wait_ms (per frame):");
    for (unsigned i = 0; i < VK_NUM_FINISH_REASONS; i++) {
        fprintf(g_perflog.f, " %s", k_finish_reason_names[i]);
    }
    fprintf(g_perflog.f, " aux_fence\n");
    fprintf(stderr, "perflog: writing frame timings to %s (stutter > %.1f ms)\n",
            path, g_perflog.stutter_ms);
}

void pgraph_vk_perflog_aux_wait(double wait_ms)
{
    if (g_perflog.enabled) {
        g_perflog.aux_wait_ms += wait_ms;
    }
}

void pgraph_vk_perflog_gpu_busy(double busy_ms)
{
    if (g_perflog.enabled) {
        g_perflog.gpu_busy_ms += busy_ms;
    }
}

void pgraph_vk_perflog_gpu_wait(FinishReason why, double wait_ms)
{
    if (!g_perflog.enabled || why >= VK_NUM_FINISH_REASONS) {
        return;
    }
    g_perflog.gpu_wait_ms[why] += wait_ms;
}

bool pgraph_vk_perflog_enabled(void)
{
    return g_perflog.enabled;
}


static void perflog_flush_window(int64_t now, uint64_t tex_bytes,
                                 unsigned width, unsigned height)
{
    unsigned n = g_perflog.num_samples;
    if (!n) {
        g_perflog.window_start_ns = now;
        return;
    }

    double elapsed_s =
        (double)(now - g_perflog.window_start_ns) / NANOSECONDS_PER_SECOND;

    double sorted[PERFLOG_MAX_SAMPLES];
    memcpy(sorted, g_perflog.samples, n * sizeof(sorted[0]));
    qsort(sorted, n, sizeof(sorted[0]), cmp_double);

    double sum = 0;
    for (unsigned i = 0; i < n; i++) {
        sum += sorted[i];
    }

    double p50 = sorted[n / 2];
    double p99 = sorted[(unsigned)((n - 1) * 0.99)];
    double worst = sorted[n - 1];
    double avg = sum / n;

    /* The 1% low is the frame rate implied by the slowest one percent of
     * frames: the number that reflects how bad the hitches feel. */
    unsigned low_count = n / 100 ? n / 100 : 1;
    double low_sum = 0;
    for (unsigned i = 0; i < low_count; i++) {
        low_sum += sorted[n - 1 - i];
    }
    double fps_1pct_low = 1000.0 / (low_sum / low_count);

    long cpu_mdeg = read_long_file("/sys/class/thermal/thermal_zone0/temp");
    long cpu_khz =
        read_long_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");

    fprintf(g_perflog.f,
            "window,%.1f,%u,%.1f,%.2f,%.2f,%.2f,%.2f,%.1f,%u,%.2f,%.2f,%.0f,%ux%u",
            elapsed_s, n, n / elapsed_s, avg, p50, p99, worst, fps_1pct_low,
            g_perflog.stutters, g_perflog.download_ms / n,
            g_perflog.upload_ms / n, (double)tex_bytes / (1024 * 1024), width,
            height);

    fprintf(g_perflog.f, ",%.1f,%ld",
            cpu_mdeg >= 0 ? cpu_mdeg / 1000.0 : -1.0,
            cpu_khz >= 0 ? cpu_khz / 1000 : -1);

    /* Per-frame averages: a hitch caused by compiling a shader shows up as a
     * non-zero shader_gen or pipeline_gen, while a GPU that simply has too much
     * to do shows in submissions and draws. */
    for (unsigned i = 0; i < NUM_COUNTERS; i++) {
        fprintf(g_perflog.f, ",%.2f", (double)g_perflog.counters[i] / n);
    }
    fprintf(g_perflog.f, ",%.2f", g_perflog.gpu_busy_ms / n);
    for (unsigned i = 0; i < VK_NUM_FINISH_REASONS; i++) {
        fprintf(g_perflog.f, ",%.2f", g_perflog.gpu_wait_ms[i] / n);
    }
    fprintf(g_perflog.f, ",%.2f", g_perflog.aux_wait_ms / n);
    fprintf(g_perflog.f, "\n");

    if (g_perflog.dropped_samples) {
        fprintf(g_perflog.f, "# window %lu dropped %u samples (over capacity)\n",
                g_perflog.window_index, g_perflog.dropped_samples);
    }

    g_perflog.window_index++;
    g_perflog.num_samples = 0;
    g_perflog.dropped_samples = 0;
    g_perflog.download_ms = 0;
    g_perflog.upload_ms = 0;
    g_perflog.stutters = 0;
    memset(g_perflog.counters, 0, sizeof(g_perflog.counters));
    memset(g_perflog.gpu_wait_ms, 0, sizeof(g_perflog.gpu_wait_ms));
    g_perflog.gpu_busy_ms = 0;
    g_perflog.aux_wait_ms = 0;
    g_perflog.window_start_ns = now;
}

void pgraph_vk_perflog_frame(double download_ms, double upload_ms,
                             uint64_t tex_bytes, unsigned width,
                             unsigned height)
{
    if (!g_perflog.enabled) {
        return;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (g_perflog.last_frame_ns == 0) {
        g_perflog.last_frame_ns = now;
        g_perflog.window_start_ns = now;
        return;
    }

    double frame_ms = (double)(now - g_perflog.last_frame_ns) / 1000000.0;
    g_perflog.last_frame_ns = now;

    if (g_perflog.num_samples < PERFLOG_MAX_SAMPLES) {
        g_perflog.samples[g_perflog.num_samples++] = frame_ms;
    } else {
        g_perflog.dropped_samples++;
    }
    g_perflog.download_ms += download_ms;
    g_perflog.upload_ms += upload_ms;

    /* Counters report the most recently completed frame, so sampling them here
     * accumulates them over the window. */
    for (unsigned i = 0; i < NUM_COUNTERS; i++) {
        g_perflog.counters[i] += nv2a_profile_get_counter_value(k_counters[i].id);
    }

    if (frame_ms > g_perflog.stutter_ms) {
        g_perflog.stutters++;
        if (g_perflog.stutter_lines < PERFLOG_MAX_STUTTER_LINES) {
            g_perflog.stutter_lines++;
            fprintf(g_perflog.f, "stutter,%.1f,%.2f,%.2f,%.2f,%.0f,%ux%u\n",
                    (double)(now - g_perflog.window_start_ns) /
                        NANOSECONDS_PER_SECOND,
                    frame_ms, download_ms, upload_ms,
                    (double)tex_bytes / (1024 * 1024), width, height);
            if (g_perflog.stutter_lines == PERFLOG_MAX_STUTTER_LINES) {
                fprintf(g_perflog.f,
                        "# stutter line limit reached; summaries continue\n");
            }
        }
    }

    if (now - g_perflog.window_start_ns >= PERFLOG_WINDOW_NS) {
        perflog_flush_window(now, tex_bytes, width, height);
    }
}

void pgraph_vk_perflog_event(const char *what, double cost_ms)
{
    if (!g_perflog.enabled || !g_perflog.f) {
        return;
    }

    /* Events are rare by nature (evictions, cache pressure), so they are not
     * capped separately; they ride the same file. */
    fprintf(g_perflog.f, "event,%.1f,%s,%.2f\n",
            (double)(qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
                     g_perflog.window_start_ns) /
                NANOSECONDS_PER_SECOND,
            what, cost_ms);
}
