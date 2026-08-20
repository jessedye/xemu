/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2019-2025 Matt Borgerson
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

#include "apu_int.h"

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
    };

    d->monitor.stream = NULL;

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    d->monitor.stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (d->monitor.stream == NULL) {
        error_setg(errp, "SDL_OpenAudioDeviceStream failed: %s",
                   SDL_GetError());
        return;
    }

    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(d->monitor.stream);

    SDL_AudioSpec dev_spec;
    int dev_buf_frames = 0;
    int dev_drain_bytes = 0;
    if (SDL_GetAudioDeviceFormat(dev, &dev_spec, &dev_buf_frames)) {
        dev_drain_bytes = dev_buf_frames * spec.channels *
                          SDL_AUDIO_BYTESIZE(spec.format) *
                          spec.freq / dev_spec.freq;
    }
    int frame_bytes = sizeof(d->monitor.frame_buf);
    int drain = MAX(dev_drain_bytes, frame_bytes);

    /* The queue high-water mark is three device buffers, whatever the device
     * happens to report. Nothing bounds that in time, so a host device with a
     * generous buffer - an ALSA plug chain, say - sets the audio latency the
     * player hears, and the throttle only sheds 1 us per 5.33 ms frame, so it
     * stays there once reached. Cap it in milliseconds; 0 restores the
     * unbounded behaviour. */
    const int bytes_per_ms = 48000 * 2 * 2 / 1000;
    int max_latency_ms = 0;
    const char *opt = getenv("XEMU_AUDIO_MAX_LATENCY_MS");
    if (opt && opt[0]) {
        max_latency_ms = atoi(opt);
    }

    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    if (max_latency_ms > 0) {
        int cap = max_latency_ms * bytes_per_ms;
        if (d->monitor.queued_bytes_high > cap) {
            d->monitor.queued_bytes_high = cap;
            d->monitor.queued_bytes_low = MAX(cap / 3, frame_bytes);
        }
    }

    fprintf(stderr,
            "apu: device buffer %d frames (%d bytes), watermarks low %d high %d"
            " (%.0f ms / %.0f ms)\n",
            dev_buf_frames, dev_drain_bytes, d->monitor.queued_bytes_low,
            d->monitor.queued_bytes_high,
            (double)d->monitor.queued_bytes_low / bytes_per_ms,
            (double)d->monitor.queued_bytes_high / bytes_per_ms);

    SDL_ResumeAudioDevice(dev);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    if (d->monitor.stream) {
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
        SDL_SetAudioStreamGain(d->monitor.stream, vu);
        SDL_PutAudioStreamData(d->monitor.stream, d->monitor.frame_buf,
                            sizeof(d->monitor.frame_buf));
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
