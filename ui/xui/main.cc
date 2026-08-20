//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <functional>
#include <assert.h>
#include <fpng.h>

#include <deque>
#include <vector>
#include <string>
#include <memory>

#include "actions.hh"
#include "common.hh"
#include "xemu-hud.h"
#include "misc.hh"
#include "gl-helpers.hh"
#include "input-manager.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "scene.hh"
#include "scene-manager.hh"
#include "main-menu.hh"
#include "popup-menu.hh"
#include "notifications.hh"
#include "monitor.hh"
#include "debug.hh"
#include "welcome.hh"
#include "menubar.hh"
#include "compat.hh"
#if defined(_WIN32)
#include "update.hh"
#endif

bool g_screenshot_pending;
const char *g_snapshot_pending_load_name;

float g_main_menu_height;

static ImGuiStyle g_base_style;
static float g_last_scale;
static int g_vsync;
static GLuint g_tex;
static bool g_flip_req;


static void InitializeStyle()
{
    g_font_mgr.Rebuild();

    ImGui::StyleColorsDark();
    ImVec4 *c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.98f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.11f, 0.11f, 0.11f, 0.60f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 0.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.13f, 0.87f, 0.16f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.25f, 0.75f, 0.10f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.83f, 0.49f, 0.04f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.26f, 0.67f, 0.23f, 0.95f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.21f, 0.54f, 0.19f, 0.99f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.24f, 0.60f, 0.21f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.16f, 0.16f, 0.16f, 0.73f);

    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0;
    s.FrameRounding = 6.0;
    s.PopupRounding = 6.0;
    g_base_style = s;
}

static void xemu_hud_init_common(SDL_Window *window, void *sdl_gl_context,
                                 bool with_gl_renderer)
{
    xemu_monitor_init();
    g_vsync = g_config.display.window.vsync;

    if (with_gl_renderer) {
        InitCustomRendering();
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    // Setup Platform/Renderer bindings
    if (with_gl_renderer) {
        ImGui_ImplSDL3_InitForOpenGL(window, sdl_gl_context);
        ImGui_ImplOpenGL3_Init("#version 140");
    } else {
        ImGui_ImplSDL3_InitForVulkan(window);
    }
    ImPlot::CreateContext();

#if defined(_WIN32)
    if (!g_config.general.show_welcome && g_config.general.updates.check) {
        update_window.CheckForUpdates();
    }
#endif
    g_last_scale = g_viewport_mgr.m_scale;
    if (with_gl_renderer) {
        /* Building the font atlas goes straight to the OpenGL backend, so it
         * has nothing to build into here. Nothing draws on this path, so no
         * atlas is needed. */
        InitializeStyle();
    }
    g_main_menu.SetNextViewIndex(g_config.general.last_viewed_menu_index);
    first_boot_window.is_open = g_config.general.show_welcome;
}

static bool g_hud_has_gl_renderer;
bool g_hud_uses_vulkan;

void xemu_hud_init(SDL_Window *window, void *sdl_gl_context)
{
    xemu_hud_init_common(window, sdl_gl_context, true);
    g_hud_has_gl_renderer = true;
}

/* Bring ImGui up without a rendering backend.
 *
 * The overlay is drawn by the OpenGL ImGui backend, so it cannot be shown when
 * presenting through the Vulkan swapchain. The rest of the UI still expects a
 * live ImGui context though: event handling and input capture both call into
 * it. Create the context and the platform binding, and leave the renderer
 * binding and the GL-backed controller artwork out. Nothing calls NewFrame on
 * this path, so no font texture is needed. */
void xemu_hud_init_input_only(SDL_Window *window)
{
    xemu_hud_init_common(window, NULL, false);
    g_hud_has_gl_renderer = false;
}

void xemu_hud_cleanup(void)
{
    if (g_hud_has_gl_renderer) {
        ImGui_ImplOpenGL3_Shutdown();
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void xemu_hud_process_sdl_events(SDL_Event *event)
{
    // Ignore inputs that are consumed by rebinding
    if (g_main_menu.ConsumeRebindEvent(event)) {
        return;
    }

    ImGui_ImplSDL3_ProcessEvent(event);
}

void xemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    if (kbd) *kbd = io.WantCaptureKeyboard;
    if (mouse) *mouse = io.WantCaptureMouse;
}

void xemu_hud_set_framebuffer_texture(GLuint tex, bool flip)
{
    g_tex = tex;
    g_flip_req = flip;
}

void xemu_hud_update(void)
{
    ImGuiIO& io = ImGui::GetIO();
    uint32_t now = SDL_GetTicks();

    g_viewport_mgr.Update();
    g_font_mgr.Update();
    if (g_last_scale != g_viewport_mgr.m_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(g_viewport_mgr.m_scale);
        g_last_scale = g_viewport_mgr.m_scale;
    }

    if (!first_boot_window.is_open && !g_hud_uses_vulkan) {
        /* The Vulkan path presents the guest frame itself; the overlay is
         * drawn over it rather than around a framebuffer texture. */
        int ww, wh;
        SDL_GetWindowSizeInPixels(xemu_get_window(), &ww, &wh);
        RenderFramebuffer(g_tex, ww, wh, g_flip_req);
    }

#ifdef CONFIG_VULKAN
    if (g_hud_uses_vulkan) {
        ImGui_ImplVulkan_NewFrame();
    } else
#endif
    {
        ImGui_ImplOpenGL3_NewFrame();
    }
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_NewFrame();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_input_mgr.Update();

    ImGui::NewFrame();
    ProcessKeyboardShortcuts();

#if defined(CONFIG_RENDERDOC)
    if (g_capture_renderdoc_frame) {
        nv2a_dbg_renderdoc_capture_frames(1, false);
        g_capture_renderdoc_frame = false;
    }
#endif

    if (g_config.display.ui.show_menubar && !first_boot_window.is_open) {
        // Auto-hide main menu after 5s of inactivity
        static uint32_t last_check = 0;
        float alpha = 1.0;
        const uint32_t timeout = 5000;
        const float fade_duration = 1000.0;
        bool menu_wakeup = g_input_mgr.MouseMoved();
        if (menu_wakeup) {
            last_check = now;
        }
        if ((now-last_check) > timeout) {
            if (g_config.display.ui.use_animations) {
                float t = fmin((float)((now-last_check)-timeout)/fade_duration, 1);
                alpha = 1-t;
                if (t >= 1) {
                    alpha = 0;
                }
            } else {
                alpha = 0;
            }
        }
        if (alpha > 0.0) {
            ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Text];
            tc.w = alpha;
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
            ImGui::SetNextWindowBgAlpha(alpha);
            ShowMainMenu();
            ImGui::PopStyleColor();
        } else {
            g_main_menu_height = 0;
        }
    }

    static uint32_t last_mouse_move = 0;
    if (g_input_mgr.MouseMoved()) {
        last_mouse_move = now;
    }

    // FIXME: Handle time wrap around
    if (g_config.display.ui.hide_cursor && (now - last_mouse_move) > 3000) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
        !g_scene_mgr.IsDisplayingScene()) {

        // If the guide button is pressed, wake the ui
        bool menu_button = false;
        uint32_t buttons = g_input_mgr.CombinedButtons();
        if (buttons & CONTROLLER_BUTTON_GUIDE) {
            menu_button = true;
        }

        // Allow controllers without a guide button to also work
        if ((buttons & CONTROLLER_BUTTON_BACK) &&
            (buttons & CONTROLLER_BUTTON_START)) {
            menu_button = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
            g_scene_mgr.PushScene(g_main_menu);
        } else if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (menu_button ||
                   (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    !ImGui::IsAnyItemFocused() && !ImGui::IsAnyItemHovered())) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            xemu_toggle_fullscreen();
        }

        bool mod_key_down = ImGui::IsKeyDown(ImGuiKey_ModShift);
        for (int f_key = 0; f_key < 4; ++f_key) {
            if (ImGui::IsKeyPressed((enum ImGuiKey)(ImGuiKey_F5 + f_key))) {
                ActionActivateBoundSnapshot(f_key, mod_key_down);
                break;
            }
        }
    }

    first_boot_window.Draw();
    monitor_window.Draw();
    apu_window.Draw();
    video_window.Draw();
    compatibility_reporter_window.Draw();
#if defined(_WIN32)
    update_window.Draw();
#endif
    g_scene_mgr.Draw();
    if (!first_boot_window.is_open) notification_manager.Draw();
    g_snapshot_mgr.Draw();

    // static bool show_demo = true;
    // if (show_demo) ImGui::ShowDemoWindow(&show_demo);
}

#ifdef CONFIG_VULKAN
void xemu_hud_render_vulkan(VkCommandBuffer cmd)
{
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    g_screenshot_pending = false;
}

/* The context and platform binding already exist from xemu_hud_init_input_only;
 * this attaches the renderer backend once the presenter can supply one. */
void xemu_hud_init_vulkan(void *window, const XemuHudVulkanInfo *info)
{
    (void)window;
    g_hud_uses_vulkan = true;

    /* The backend is built without prototypes so it cannot collide with volk's
     * globals, which leaves it needing the entry points handed to it. */
    ImGui_ImplVulkan_LoadFunctions(
        [](const char *name, void *user) {
            return vkGetInstanceProcAddr((VkInstance)user, name);
        },
        info->instance);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = info->instance;
    init_info.PhysicalDevice = info->physical_device;
    init_info.Device = info->device;
    init_info.QueueFamily = info->queue_family;
    init_info.Queue = info->queue;
    init_info.DescriptorPool = info->descriptor_pool;
    init_info.RenderPass = info->render_pass;
    init_info.MinImageCount = info->image_count;
    init_info.ImageCount = info->image_count;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&init_info);

    /* Deferred until a renderer backend exists: building the font atlas
     * uploads it through whichever one is installed. */
    InitializeStyle();
}
#endif

void xemu_hud_render()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (g_vsync != g_config.display.window.vsync) {
        g_vsync = g_config.display.window.vsync;
        SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
    }

    if (g_screenshot_pending) {
        SaveScreenshot(g_tex, g_flip_req);
        g_screenshot_pending = false;
    }
}
