/*
 * Present the composited display image through a Vulkan swapchain.
 *
 * The renderer already composites the guest framebuffer, scaling and the
 * PVIDEO overlay into a VkImage. Where the Vulkan driver can export that image
 * to OpenGL the UI samples it directly, but on a driver without those
 * extensions the frame is instead read back through guest memory and uploaded
 * as a GL texture, which costs a GPU round trip every frame.
 *
 * This presents that same image directly, so no readback is needed. It is
 * selected at runtime and is inert unless chosen; see
 * XEMU_DISPLAY_BACKEND=vulkan.
 */

#include "qemu/osdep.h"
#include "renderer.h"

typedef struct PresentState {
    bool initialized;
    bool unavailable;

    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR present_mode;

    uint32_t num_images;
    VkImage *images;

    /* Window size, published by the UI thread and read by the pgraph thread
     * when it presents. */
    int window_width;
    int window_height;

    /* Draw time of the frame currently on screen, so an unchanged one is not
     * presented again. */
    unsigned last_draw_time;
    bool have_presented;

    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkFence in_flight;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
} PresentState;

static PresentState g_present;

static void destroy_swapchain(PGRAPHVkState *r)
{
    if (g_present.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(r->device, g_present.swapchain, NULL);
        g_present.swapchain = VK_NULL_HANDLE;
    }
    g_free(g_present.images);
    g_present.images = NULL;
    g_present.num_images = 0;
}

static bool create_swapchain(PGRAPHVkState *r, uint32_t width, uint32_t height)
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical_device,
                                                  g_present.surface,
                                                  &caps) != VK_SUCCESS) {
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = CLAMP(width, caps.minImageExtent.width,
                             caps.maxImageExtent.width);
        extent.height = CLAMP(height, caps.minImageExtent.height,
                              caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) {
        /* Minimised: nothing to present to. */
        return false;
    }

    uint32_t num_formats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical_device, g_present.surface,
                                         &num_formats, NULL);
    if (!num_formats) {
        return false;
    }
    g_autofree VkSurfaceFormatKHR *formats =
        g_malloc_n(num_formats, sizeof(*formats));
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical_device, g_present.surface,
                                         &num_formats, formats);

    /* Any format will do: the image is copied, not sampled, so no conversion
     * is implied by the choice. */
    VkSurfaceFormatKHR chosen = formats[0];

    /* FIFO blocks the caller until the next vertical blank. Presenting runs on
     * the thread that also executes the guest's command stream, so blocking
     * there starves emulation; prefer a mode that returns immediately. */
    uint32_t num_modes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical_device,
                                              g_present.surface, &num_modes,
                                              NULL);
    g_autofree VkPresentModeKHR *modes =
        num_modes ? g_malloc_n(num_modes, sizeof(*modes)) : NULL;
    if (num_modes) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical_device,
                                                  g_present.surface,
                                                  &num_modes, modes);
    }

    /* FIFO is the only mode guaranteed to exist, so it is the fallback. */
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < num_modes; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        }
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }

    uint32_t num_images = caps.minImageCount + 1;
    if (caps.maxImageCount && num_images > caps.maxImageCount) {
        num_images = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g_present.surface,
        .minImageCount = num_images,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        /* Transfer destination rather than colour attachment: the composited
         * image is blitted in, not rendered to. */
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
    };

    if (vkCreateSwapchainKHR(r->device, &create_info, NULL,
                             &g_present.swapchain) != VK_SUCCESS) {
        return false;
    }

    vkGetSwapchainImagesKHR(r->device, g_present.swapchain,
                            &g_present.num_images, NULL);
    g_present.images = g_malloc_n(g_present.num_images, sizeof(VkImage));
    vkGetSwapchainImagesKHR(r->device, g_present.swapchain,
                            &g_present.num_images, g_present.images);

    g_present.format = chosen.format;
    g_present.extent = extent;
    g_present.present_mode = present_mode;
    g_present.have_presented = false;
    return true;
}

VkInstance pgraph_vk_get_instance(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    return r ? r->instance : VK_NULL_HANDLE;
}

bool pgraph_vk_present_init(PGRAPHState *pg, VkSurfaceKHR surface, int width,
                            int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (g_present.initialized || g_present.unavailable) {
        return g_present.initialized;
    }

    g_present.surface = surface;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(r->physical_device,
                                         indices.queue_family,
                                         g_present.surface, &supported);
    if (!supported) {
        fprintf(stderr,
                "present: the render queue family cannot present to this "
                "surface\n");
        g_present.unavailable = true;
        return false;
    }

    if (!create_swapchain(r, width, height)) {
        fprintf(stderr, "present: could not create a swapchain\n");
        g_present.unavailable = true;
        return false;
    }

    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VK_CHECK(vkCreateSemaphore(r->device, &sem_info, NULL,
                               &g_present.image_available));
    VK_CHECK(vkCreateSemaphore(r->device, &sem_info, NULL,
                               &g_present.render_finished));
    VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &g_present.in_flight));

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(vkCreateCommandPool(r->device, &pool_info, NULL,
                                 &g_present.command_pool));

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_present.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(r->device, &alloc_info,
                                      &g_present.command_buffer));

    g_present.initialized = true;
    static const char *const mode_names[] = {
        [VK_PRESENT_MODE_IMMEDIATE_KHR] = "immediate",
        [VK_PRESENT_MODE_MAILBOX_KHR] = "mailbox",
        [VK_PRESENT_MODE_FIFO_KHR] = "fifo",
        [VK_PRESENT_MODE_FIFO_RELAXED_KHR] = "fifo-relaxed",
    };
    fprintf(stderr, "present: swapchain ready, %ux%u, %u images, %s\n",
            g_present.extent.width, g_present.extent.height,
            g_present.num_images, mode_names[g_present.present_mode]);
    return true;
}

void pgraph_vk_present_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!g_present.initialized) {
        return;
    }

    vkDeviceWaitIdle(r->device);

    vkDestroyCommandPool(r->device, g_present.command_pool, NULL);
    vkDestroyFence(r->device, g_present.in_flight, NULL);
    vkDestroySemaphore(r->device, g_present.render_finished, NULL);
    vkDestroySemaphore(r->device, g_present.image_available, NULL);
    destroy_swapchain(r);
    /* The surface belongs to whoever created it from the window. */

    memset(&g_present, 0, sizeof(g_present));
}

static void transition(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src_access, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
}

bool pgraph_vk_present_frame(PGRAPHState *pg)
{
    int width = g_present.window_width;
    int height = g_present.window_height;

    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!g_present.initialized || !r->display.image) {
        return false;
    }

    /* The image on screen is still the current one; presenting it again would
     * cost a full-screen blit and show nothing new. */
    if (g_present.have_presented &&
        g_present.last_draw_time == r->display.draw_time) {
        return false;
    }

    /* Skip this frame rather than wait: the caller is the thread running the
     * guest, and the display can afford to miss a frame far more cheaply than
     * emulation can afford to stall. */
    if (vkWaitForFences(r->device, 1, &g_present.in_flight, VK_TRUE, 0) !=
        VK_SUCCESS) {
        return false;
    }

    uint32_t index = 0;
    VkResult result = vkAcquireNextImageKHR(
        r->device, g_present.swapchain, 0, g_present.image_available,
        VK_NULL_HANDLE, &index);
    if (result == VK_NOT_READY || result == VK_TIMEOUT) {
        return false;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        /* The window changed size; rebuild on the next frame. */
        vkDeviceWaitIdle(r->device);
        destroy_swapchain(r);
        if (!create_swapchain(r, width, height)) {
            g_present.initialized = false;
        }
        return false;
    }
    if (result != VK_SUCCESS) {
        return false;
    }

    VK_CHECK(vkResetFences(r->device, 1, &g_present.in_flight));
    VK_CHECK(vkResetCommandBuffer(g_present.command_buffer, 0));

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(g_present.command_buffer, &begin_info));

    VkImage target = g_present.images[index];

    transition(g_present.command_buffer, target, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT);
    transition(g_present.command_buffer, r->display.image,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    /* Blit rather than copy so the composited image is scaled to the window
     * without a separate pass. */
    VkImageBlit blit = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .layerCount = 1 },
        .srcOffsets = { { 0, 0, 0 },
                        { r->display.width, r->display.height, 1 } },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .layerCount = 1 },
        .dstOffsets = { { 0, 0, 0 },
                        { (int32_t)g_present.extent.width,
                          (int32_t)g_present.extent.height, 1 } },
    };
    vkCmdBlitImage(g_present.command_buffer, r->display.image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    transition(g_present.command_buffer, target,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
               0);
    transition(g_present.command_buffer, r->display.image,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);

    VK_CHECK(vkEndCommandBuffer(g_present.command_buffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_present.image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_present.command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &g_present.render_finished,
    };
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, g_present.in_flight));

    /* Record the frame the same way the OpenGL path does, so the two are
     * directly comparable. There is no readback here, so the download and
     * upload costs are zero by construction; the blit is charged as the
     * present cost. */
    if (pgraph_vk_perflog_enabled()) {
        pgraph_vk_perflog_frame(0.0, 0.0, 0,
                                g_present.extent.width,
                                g_present.extent.height);
    }

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_present.render_finished,
        .swapchainCount = 1,
        .pSwapchains = &g_present.swapchain,
        .pImageIndices = &index,
    };
    vkQueuePresentKHR(r->queue, &present_info);

    g_present.last_draw_time = r->display.draw_time;
    g_present.have_presented = true;

    return true;
}

/* Public entry points. These bridge the UI's opaque handles to the renderer
 * state, keeping Vulkan types out of the interface between the two. */

uint64_t nv2a_get_vk_instance(void)
{
    return (uint64_t)(uintptr_t)pgraph_vk_get_instance(&g_nv2a->pgraph);
}

bool nv2a_present_init(uint64_t vk_surface, int width, int height)
{
    return pgraph_vk_present_init(&g_nv2a->pgraph,
                                  (VkSurfaceKHR)vk_surface, width, height);
}

bool nv2a_present_frame(int width, int height)
{
    NV2AState *d = g_nv2a;
    PGRAPHState *pg = &d->pgraph;

    if (!g_present.initialized) {
        return false;
    }

    g_present.window_width = width;
    g_present.window_height = height;

    /* Ask the pgraph thread to composite and present. The compositing and the
     * present are both queue submissions, and a VkQueue cannot be used from
     * two threads at once, so this thread only waits for the result. */
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_event_reset(&pg->sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&pg->sync_complete);

    return true;
}

void nv2a_present_finalize(void)
{
    pgraph_vk_present_finalize(&g_nv2a->pgraph);
}
