/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "renderer.h"

// Largest buffer we can reasonably ask this device for. Desktop GPUs have
// gigabytes of device-local memory; integrated and embedded parts often have
// far less, so derive the size from what the driver reports instead of
// assuming. Returns a value clamped to the device limits.
static VkDeviceSize clamp_buffer_size(PGRAPHVkState *r, VkDeviceSize desired,
                                      bool storage)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(r->physical_device, &props);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &mem_props);

    VkDeviceSize largest_heap = 0;
    for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++) {
        if (mem_props.memoryHeaps[i].size > largest_heap) {
            largest_heap = mem_props.memoryHeaps[i].size;
        }
    }

    // Leave the bulk of the heap for surfaces and textures.
    VkDeviceSize budget = largest_heap / 16;
    VkDeviceSize limit = desired;
    if (storage && limit > (VkDeviceSize)props.limits.maxStorageBufferRange) {
        limit = (VkDeviceSize)props.limits.maxStorageBufferRange;
    }
    if (budget && limit > budget) {
        limit = budget;
    }

    // Keep something workable even on very small heaps.
    VkDeviceSize floor = 8 * 1024 * 1024;
    if (limit < floor) {
        limit = desired < floor ? desired : floor;
    }
    return limit;
}

static void create_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Heap size reports capacity, not what is free. On a unified memory device
    // another process may already hold most of it, so a request sized from the
    // heap can still fail. Halve the request until it fits rather than
    // aborting, and record the size actually obtained.
    const VkDeviceSize min_size = 8 * 1024 * 1024;
    VkDeviceSize size = buffer->buffer_size;
    VkResult result;

    for (;;) {
        VkBufferCreateInfo buffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = buffer->usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        result = vmaCreateBuffer(r->allocator, &buffer_create_info,
                                 &buffer->alloc_info, &buffer->buffer,
                                 &buffer->allocation, NULL);
        if (result == VK_SUCCESS) {
            break;
        }
        if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
            result != VK_ERROR_OUT_OF_HOST_MEMORY) {
            break;
        }
        if (size <= min_size) {
            break;
        }
        size /= 2;
        fprintf(stderr,
                "Buffer allocation (usage 0x%x) short of memory, retrying at "
                "%zu MiB\n",
                buffer->usage, (size_t)(size / (1024 * 1024)));
    }

    if (result != VK_SUCCESS) {
        fprintf(stderr,
                "Failed to allocate %zu MiB buffer (usage 0x%x): vk_result %d\n",
                (size_t)(size / (1024 * 1024)), buffer->usage, result);
    } else {
        buffer->buffer_size = size;
    }
    VK_CHECK(result);
}

static void destroy_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vmaDestroyBuffer(r->allocator, buffer->buffer, buffer->allocation);
    buffer->buffer = VK_NULL_HANDLE;
    buffer->allocation = VK_NULL_HANDLE;
}

void pgraph_vk_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Profile buffer sizes

    VmaAllocationCreateInfo host_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };
    VmaAllocationCreateInfo device_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
    };

    r->storage_buffers[BUFFER_STAGING_DST] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = clamp_buffer_size(r, 4096 * 4096 * 4, false),
    };

    r->storage_buffers[BUFFER_STAGING_SRC] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_STAGING_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_COMPUTE_DST] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = clamp_buffer_size(r, (VkDeviceSize)4096 * 4096 * 8, true),
    };

    r->storage_buffers[BUFFER_COMPUTE_SRC] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_COMPUTE_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_INDEX] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        /* One hundred times the largest batch is far more than a frame can
         * use, and unlike the other buffers this was not clamped, so it
         * reserved about 199 MiB here and the same again for its staging
         * partner. Clamp it like the rest. */
        .buffer_size =
            clamp_buffer_size(r, sizeof(pg->inline_elements) * 100, false),
    };

    r->storage_buffers[BUFFER_INDEX_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_INDEX].buffer_size,
    };

    // FIXME: Don't assume that we can render with host mapped buffer
    r->storage_buffers[BUFFER_VERTEX_RAM] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = memory_region_size(d->vram),
    };

    r->bitmap_size = memory_region_size(d->vram) / 4096;
    r->uploaded_bitmap = bitmap_new(r->bitmap_size);
    bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
    r->referenced_bitmap = bitmap_new(r->bitmap_size);
    bitmap_clear(r->referenced_bitmap, 0, r->bitmap_size);
    r->referenced_inflight_bitmap = bitmap_new(r->bitmap_size);
    bitmap_clear(r->referenced_inflight_bitmap, 0, r->bitmap_size);

    r->storage_buffers[BUFFER_VERTEX_INLINE] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = clamp_buffer_size(
            r,
            (VkDeviceSize)NV2A_VERTEXSHADER_ATTRIBUTES * NV2A_MAX_BATCH_LENGTH *
                4 * sizeof(float) * 10,
            false),
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_VERTEX_INLINE].buffer_size,
    };

    r->storage_buffers[BUFFER_UNIFORM] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .buffer_size = 8 * 1024 * 1024,
    };

    r->storage_buffers[BUFFER_UNIFORM_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_UNIFORM].buffer_size,
    };

    for (int i = 0; i < BUFFER_COUNT; i++) {
        create_buffer(pg, &r->storage_buffers[i]);
    }

    /* Report what was actually reserved. The requested sizes are large and get
     * clamped against the device heap, so the figure that matters on a small
     * board is this one, not the constant in the source. */
    {
        VkDeviceSize total = 0;
        for (int i = 0; i < BUFFER_COUNT; i++) {
            total += r->storage_buffers[i].buffer_size;
        }
        fprintf(stderr, "vk-buffers: reserved %zu MiB across %d buffers\n",
                (size_t)(total / (1024 * 1024)), BUFFER_COUNT);
        for (int i = 0; i < BUFFER_COUNT; i++) {
            fprintf(stderr, "vk-buffers:   [%d] %zu MiB\n", i,
                    (size_t)(r->storage_buffers[i].buffer_size / (1024 * 1024)));
        }
    }

    /* Several of these are used as a pair, with data copied from one into the
     * other, and each takes its size from its partner before anything is
     * allocated. If one of a pair ended up smaller because the device was short
     * of memory, reconcile both to the smaller size so a copy cannot outrun the
     * smaller allocation. Lowering a recorded size is always safe: the buffer
     * itself is at least that large. */
    static const int paired_buffers[][2] = {
        { BUFFER_STAGING_DST, BUFFER_STAGING_SRC },
        { BUFFER_COMPUTE_DST, BUFFER_COMPUTE_SRC },
        { BUFFER_INDEX, BUFFER_INDEX_STAGING },
        { BUFFER_VERTEX_INLINE, BUFFER_VERTEX_INLINE_STAGING },
        { BUFFER_UNIFORM, BUFFER_UNIFORM_STAGING },
    };

    for (int i = 0; i < ARRAY_SIZE(paired_buffers); i++) {
        StorageBuffer *a = &r->storage_buffers[paired_buffers[i][0]];
        StorageBuffer *b = &r->storage_buffers[paired_buffers[i][1]];

        if (a->buffer_size == b->buffer_size) {
            continue;
        }

        VkDeviceSize smaller =
            a->buffer_size < b->buffer_size ? a->buffer_size : b->buffer_size;
        fprintf(stderr,
                "Paired buffers were allocated at different sizes "
                "(%zu and %zu MiB); using %zu MiB for both\n",
                (size_t)(a->buffer_size / (1024 * 1024)),
                (size_t)(b->buffer_size / (1024 * 1024)),
                (size_t)(smaller / (1024 * 1024)));
        a->buffer_size = smaller;
        b->buffer_size = smaller;
    }

    // FIXME: Add fallback path for device using host mapped memory

    int buffers_to_map[] = { BUFFER_VERTEX_RAM,
                             BUFFER_INDEX_STAGING,
                             BUFFER_VERTEX_INLINE_STAGING,
                             BUFFER_UNIFORM_STAGING };

    for (int i = 0; i < ARRAY_SIZE(buffers_to_map); i++) {
        VK_CHECK(vmaMapMemory(
            r->allocator, r->storage_buffers[buffers_to_map[i]].allocation,
            (void **)&r->storage_buffers[buffers_to_map[i]].mapped));
    }
}

void pgraph_vk_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (r->storage_buffers[i].mapped) {
            vmaUnmapMemory(r->allocator, r->storage_buffers[i].allocation);
        }
        destroy_buffer(pg, &r->storage_buffers[i]);
    }

    g_free(r->uploaded_bitmap);
    r->uploaded_bitmap = NULL;
    g_free(r->referenced_bitmap);
    r->referenced_bitmap = NULL;
    g_free(r->referenced_inflight_bitmap);
    r->referenced_inflight_bitmap = NULL;
}

bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = &r->storage_buffers[index];
    return (ROUND_UP(b->buffer_offset, alignment) + size) <= b->buffer_size;
}

VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDeviceSize total_size = 0;
    for (int i = 0; i < count; i++) {
        total_size += sizes[i];
    }
    assert(pgraph_vk_buffer_has_space_for(pg, index, total_size, alignment));

    StorageBuffer *b = &r->storage_buffers[index];
    VkDeviceSize starting_offset = ROUND_UP(b->buffer_offset, alignment);

    assert(b->mapped);

    for (int i = 0; i < count; i++) {
        b->buffer_offset = ROUND_UP(b->buffer_offset, alignment);
        memcpy(b->mapped + b->buffer_offset, data[i], sizes[i]);
        b->buffer_offset += sizes[i];
    }

    return starting_offset;
}
