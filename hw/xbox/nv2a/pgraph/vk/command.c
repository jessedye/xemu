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

static void create_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(
        vkCreateCommandPool(r->device, &create_info, NULL, &r->command_pool));
}

static void destroy_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyCommandPool(r->device, r->command_pool, NULL);
}

static void create_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ARRAY_SIZE(r->command_buffers),
    };
    VK_CHECK(
        vkAllocateCommandBuffers(r->device, &alloc_info, r->command_buffers));

    r->command_buffer = r->command_buffers[0];
    r->aux_command_buffer = r->command_buffers[1];
}

static void destroy_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeCommandBuffers(r->device, r->command_pool,
                         ARRAY_SIZE(r->command_buffers), r->command_buffers);

    r->command_buffer = VK_NULL_HANDLE;
    r->aux_command_buffer = VK_NULL_HANDLE;
}

VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* The aux command buffer was part of the outstanding submission; it must
     * not be re-begun while the GPU may still be executing it. No-op when
     * nothing is outstanding, and safe here because callers have not
     * allocated per-submission resources yet. */
    pgraph_vk_wait_for_submission(pg);

    assert(!r->in_aux_command_buffer);
    r->in_aux_command_buffer = true;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &begin_info));

    return r->aux_command_buffer;
}

void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_aux_command_buffer);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    /* Wait on a fence for this submission rather than vkQueueWaitIdle, which
     * drains everything queued including unrelated rendering. The caller still
     * gets completion before it returns, so the semantics are unchanged; the
     * wait is simply no longer wider than it needs to be. */
    VK_CHECK(vkResetFences(r->device, 1, &r->aux_command_buffer_fence));
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info,
                           r->aux_command_buffer_fence));
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_AUX);
    bool timing = pgraph_vk_perflog_enabled();
    int64_t wait_start = timing ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
    VK_CHECK(vkWaitForFences(r->device, 1, &r->aux_command_buffer_fence,
                             VK_TRUE, UINT64_MAX));
    if (timing) {
        int64_t waited = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - wait_start;
        pgraph_vk_perflog_aux_wait((double)waited / 1000000.0);
    }

    r->in_aux_command_buffer = false;
}

void pgraph_vk_init_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    create_command_pool(pg);
    create_command_buffers(pg);

    VkFenceCreateInfo fence_create_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(vkCreateFence(r->device, &fence_create_info, NULL,
                           &r->aux_command_buffer_fence));

    /* Timestamps are optional: a device reporting no valid bits cannot
     * support them, and the renderer works the same without them. */
    if (r->device_props.limits.timestampComputeAndGraphics) {
        VkQueryPoolCreateInfo query_pool_info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 2,
        };
        VK_CHECK(vkCreateQueryPool(r->device, &query_pool_info, NULL,
                                   &r->timestamp_pool));
        r->timestamp_period_ns = r->device_props.limits.timestampPeriod;
    }
}

void pgraph_vk_finalize_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyFence(r->device, r->aux_command_buffer_fence, NULL);
    r->aux_command_buffer_fence = VK_NULL_HANDLE;

    if (r->timestamp_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(r->device, r->timestamp_pool, NULL);
        r->timestamp_pool = VK_NULL_HANDLE;
    }

    destroy_command_buffers(pg);
    destroy_command_pool(pg);
}