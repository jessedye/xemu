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
    unsigned stutters;

    unsigned long stutter_lines;
    unsigned long window_index;
    double stutter_ms;
} g_perflog;

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

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
            "# kind=stutter: elapsed_s,frame_ms,download_ms,upload_ms,"
            "tex_cache_mb,resolution\n"
            "kind,elapsed_s,a,b,c,d,e,f,g,h,i,j\n");
    fprintf(stderr, "perflog: writing frame timings to %s (stutter > %.1f ms)\n",
            path, g_perflog.stutter_ms);
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

    fprintf(g_perflog.f,
            "window,%.1f,%u,%.1f,%.2f,%.2f,%.2f,%.2f,%.1f,%u,%.2f,%.2f,%.0f,%ux%u\n",
            elapsed_s, n, n / elapsed_s, avg, p50, p99, worst, fps_1pct_low,
            g_perflog.stutters, g_perflog.download_ms / n,
            g_perflog.upload_ms / n, (double)tex_bytes / (1024 * 1024), width,
            height);

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
