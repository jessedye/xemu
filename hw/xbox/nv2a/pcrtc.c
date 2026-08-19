/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2021 Matt Borgerson
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

#include "nv2a_int.h"

uint64_t pcrtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
        case NV_PCRTC_INTR_0:
            r = d->pcrtc.pending_interrupts;
            break;
        case NV_PCRTC_INTR_EN_0:
            r = d->pcrtc.enabled_interrupts;
            break;
        case NV_PCRTC_START:
            r = d->pcrtc.start;
            break;
        case NV_PCRTC_RASTER:
            r = d->pcrtc.raster++;
            break;
        default:
            break;
    }

    nv2a_reg_log_read(NV_PCRTC, addr, size, r);
    return r;
}

void pcrtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    {
        static unsigned long n, next = 1;
        if (++n >= next) {
            fprintf(stderr, "xtrace: pcrtc_write count=%lu addr=0x%lx val=0x%lx\n",
                    n, (unsigned long)addr, (unsigned long)val);
            next *= 10;
        }
    }

    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PCRTC, addr, size, val);

    switch (addr) {
    case NV_PCRTC_INTR_0:
        d->pcrtc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_INTR_EN_0:
        d->pcrtc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_START:
        val &= 0x07FFFFFF;
        // assert(val < memory_region_size(d->vram));
        d->pcrtc.start = val;
        /* A change of scanout address is a page flip, so counting distinct
         * values per second gives the guest's real frame rate -- which is not
         * the same as the rate the host presents at. */
        static int fps_report = -1;
        if (fps_report < 0) {
            fps_report = getenv("XEMU_FPS") != NULL;
        }
        if (fps_report) {
            static uint32_t last_val = 0xFFFFFFFF;
            static int64_t window_start_ns;
            static unsigned flips;

            if (val != last_val) {
                last_val = val;
                flips++;
            }

            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            if (window_start_ns == 0) {
                window_start_ns = now;
            } else if (now - window_start_ns >= 5 * NANOSECONDS_PER_SECOND) {
                double secs =
                    (double)(now - window_start_ns) / NANOSECONDS_PER_SECOND;
                fprintf(stderr, "xemu-guest-fps: %.1f flips/s\n", flips / secs);
                window_start_ns = now;
                flips = 0;
            }
        }

        NV2A_DPRINTF("PCRTC_START - %x %x %x %x\n",
                d->vram_ptr[val+64], d->vram_ptr[val+64+1],
                d->vram_ptr[val+64+2], d->vram_ptr[val+64+3]);
        break;
    default:
        break;
    }
}
