/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2020-2021 Matt Borgerson
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

#ifndef HW_NV2A_H
#define HW_NV2A_H

void nv2a_init(PCIBus *bus, int devfn, MemoryRegion *ram);
void nv2a_context_init(void);
int nv2a_get_framebuffer_surface(void);
bool nv2a_framebuffer_is_top_down(void);
void nv2a_release_framebuffer_surface(void);
void nv2a_set_surface_scale_factor(unsigned int scale);
unsigned int nv2a_get_surface_scale_factor(void);
const uint8_t *nv2a_get_dac_palette(void);
int nv2a_get_screen_off(void);

/* Vulkan presentation backend.
 *
 * The UI creates the surface from its own window and hands it over here, so
 * this header needs no Vulkan types and the UI needs no Vulkan headers beyond
 * the declarations SDL already provides. The handles are carried as integers
 * because a Vulkan surface is a 64-bit handle rather than a pointer on 32-bit
 * hosts. Every one of these is a no-op returning false unless the build has
 * Vulkan and the Vulkan presentation backend is selected. */
uint64_t nv2a_get_vk_instance(void);
bool nv2a_present_init(uint64_t vk_surface, int width, int height);
bool nv2a_present_frame(int width, int height);
void nv2a_present_finalize(void);

#endif
