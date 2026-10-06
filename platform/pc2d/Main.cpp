// Flat desktop window - no OpenXR, no headset required at all. Draws the
// same AppMenu/UiRenderer content the composition-layer quad shows on the
// headset builds, presented into a normal window swapchain instead of an
// OpenXR session, driven by arrow keys/A/S instead of controller
// input. This is the fast local-iteration debug build the emulator/menu
// rendering will eventually show up in without needing to put the headset on.
#include "gfx/VulkanRenderer.h"
#include "emu/Emulator.h"
#include "io/Settings.h"
#include "menu/AppMenu.h"
#include "input/ButtonMapping.h"
#include "gfx/UiRenderer.h"
#include "desktop/DesktopPlatform.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

    void CheckVk(VkResult result, const char *what)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string("Vulkan call failed: ") + what + " (" + std::to_string(result) + ")");
        }
    }

    void PollDesktopButtonState(GLFWwindow *window, uint32_t buttonStates[3])
    {
        using namespace ButtonMapper;
        buttonStates[DeviceGamepad] = 0;
        buttonStates[DeviceLeftTouch] = 0;
        buttonStates[DeviceRightTouch] = 0;

        auto menuKey = [&](int glfwKey, uint32_t emuButton)
        {
            if (glfwGetKey(window, glfwKey) == GLFW_PRESS)
                buttonStates[DeviceRightTouch] |= ButtonMapping[emuButton];
        };
        menuKey(GLFW_KEY_UP, EmuButton_Up);
        menuKey(GLFW_KEY_DOWN, EmuButton_Down);
        menuKey(GLFW_KEY_LEFT, EmuButton_Left);
        menuKey(GLFW_KEY_RIGHT, EmuButton_Right);
        menuKey(GLFW_KEY_S, EmuButton_A);
        menuKey(GLFW_KEY_A, EmuButton_B);

        GLFWgamepadstate pad{};
        if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &pad))
        {
            uint32_t &bits = buttonStates[DeviceGamepad];
            auto button = [&](int glfwButton, uint32_t emuButton)
            {
                if (pad.buttons[glfwButton] == GLFW_PRESS)
                    bits |= ButtonMapping[emuButton];
            };
            button(GLFW_GAMEPAD_BUTTON_A, EmuButton_A);
            button(GLFW_GAMEPAD_BUTTON_B, EmuButton_B);
            button(GLFW_GAMEPAD_BUTTON_X, EmuButton_X);
            button(GLFW_GAMEPAD_BUTTON_Y, EmuButton_Y);
            button(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, EmuButton_LShoulder);
            button(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, EmuButton_RShoulder);
            button(GLFW_GAMEPAD_BUTTON_BACK, EmuButton_Back);
            button(GLFW_GAMEPAD_BUTTON_START, EmuButton_Enter);
            button(GLFW_GAMEPAD_BUTTON_DPAD_UP, EmuButton_Up);
            button(GLFW_GAMEPAD_BUTTON_DPAD_DOWN, EmuButton_Down);
            button(GLFW_GAMEPAD_BUTTON_DPAD_LEFT, EmuButton_Left);
            button(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, EmuButton_Right);
            constexpr float deadzone = 0.5f;
            if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X] < -deadzone)
                bits |= ButtonMapping[EmuButton_LeftStickLeft];
            if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X] > deadzone)
                bits |= ButtonMapping[EmuButton_LeftStickRight];
            if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -deadzone)
                bits |= ButtonMapping[EmuButton_LeftStickUp];
            if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] > deadzone)
                bits |= ButtonMapping[EmuButton_LeftStickDown];
            if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] < -deadzone)
                bits |= ButtonMapping[EmuButton_RightStickLeft];
            if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] > deadzone)
                bits |= ButtonMapping[EmuButton_RightStickRight];
            if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] < -deadzone)
                bits |= ButtonMapping[EmuButton_RightStickUp];
            if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] > deadzone)
                bits |= ButtonMapping[EmuButton_RightStickDown];
        }
    }

    // VB gameplay input - separate key layout from menu navigation (the VB
    // controller has two D-pads plus A/B/L/R/Start/Select, more buttons than
    // the menu's 6). Left D-pad: arrows. Right D-pad: WASD. A/B: X/Z
    // (SNES-style layout). L/R: Q/E. Start/Select: Enter/Backspace.
    //
    // Routed through the same ButtonMapper::EmuButton_*/TranslateToVBBitmask
    // abstraction menu navigation already uses (rather than setting
    // VBButtonBit bits directly, like this used to) so the Emulator Button
    // Mapping menu page can actually rebind these - see
    // ApplyDefaultGameplayBindings for the default key<->EmuButton_* slot
    // assignment these physical keys are wired to.
    uint32_t PollGameplayInput(GLFWwindow *window, const AppSettings &settings, const AppMenu &appMenu)
    {
        using namespace ButtonMapper;
        uint32_t buttonStates[3] = {0, 0, 0};
        PollDesktopButtonState(window, buttonStates);
        appMenu.ApplyGameplayInputSuppression(buttonStates);
        uint32_t result = TranslateToVBBitmask(buttonStates, settings.vbButtons);
        // The menu's select/back keys (see PollDesktopButtonState) still read
        // as pressed on the frame the menu closes - drop them until release,
        // same as the bitmask suppression above does for the other devices.
        auto suppressedKey = [&](int key)
        {
            return (key == GLFW_KEY_S && appMenu.SuppressesDesktopKey(EmuButton_A)) ||
                   (key == GLFW_KEY_A && appMenu.SuppressesDesktopKey(EmuButton_B));
        };
        for (uint32_t vbBit = 0; vbBit < 16; ++vbBit)
            for (const MappedButton &binding : settings.vbButtons[vbBit].Buttons)
                if (binding.IsSet && binding.InputDevice == DeviceKeyboard &&
                    binding.ButtonIndex >= 0 && binding.ButtonIndex <= GLFW_KEY_LAST &&
                    glfwGetKey(window, binding.ButtonIndex) == GLFW_PRESS && !suppressedKey(binding.ButtonIndex))
                    result |= (1u << vbBit);
        return result;
    }

    // Fills in vbButtons/menu-button slots that have never been bound yet
    // (IsSet == false) with this platform's default key layout - runs once
    // at startup, after Settings::Load(), so a user's saved remaps (from a
    // previous run) are never overwritten, only genuinely-unset slots (a
    // fresh settings.dat, or one saved before a button existed) get a
    // default. Mirrors PollGameplayInput's key choices above exactly, so
    // first-run behavior is unchanged from before this abstraction existed.
    void ApplyDefaultGameplayBindings(AppSettings &settings)
    {
        using namespace ButtonMapper;
        auto setDefault = [&](uint32_t vbBit, uint32_t emuButton)
        {
            MappedButton &b = settings.vbButtons[vbBit].Buttons[0];
            if (!b.IsSet)
            {
                b.IsSet = true;
                b.InputDevice = DeviceKeyboard;
                b.ButtonIndex = static_cast<int>(emuButton);
            }
        };
        setDefault(VBButtonBit::LeftUp, GLFW_KEY_UP);
        setDefault(VBButtonBit::LeftDown, GLFW_KEY_DOWN);
        setDefault(VBButtonBit::LeftLeft, GLFW_KEY_LEFT);
        setDefault(VBButtonBit::LeftRight, GLFW_KEY_RIGHT);
        setDefault(VBButtonBit::RightUp, GLFW_KEY_W);
        setDefault(VBButtonBit::RightDown, GLFW_KEY_S);
        setDefault(VBButtonBit::RightLeft, GLFW_KEY_A);
        setDefault(VBButtonBit::RightRight, GLFW_KEY_D);
        setDefault(VBButtonBit::A, GLFW_KEY_X);
        setDefault(VBButtonBit::B, GLFW_KEY_Z);
        setDefault(VBButtonBit::L, GLFW_KEY_Q);
        setDefault(VBButtonBit::R, GLFW_KEY_E);
        setDefault(VBButtonBit::Start, GLFW_KEY_ENTER);
        setDefault(VBButtonBit::Select, GLFW_KEY_BACKSPACE);
    }

} // namespace

int main()
{
    DesktopPlatform platform;

    // Window is sized to exactly fit the upscaled game screen; the menu is
    // a smaller fixed-size (kMenuWidth*kMenuScale x kMenuHeight*kMenuScale
    // physical pixels - kMenuWidth/kMenuHeight alone are logical units, see
    // AppMenuLayout.h) panel composited (rounded corners and all) at a
    // centered offset within it, not the window's full size.
    const int windowWidth = static_cast<int>(Emulator::kPreviewWidth * Emulator::kScale);
    const int windowHeight = static_cast<int>(Emulator::kPreviewHeight * Emulator::kScale);

    if (!glfwInit())
    {
        std::fprintf(stderr, "VirtualBoyGo 2D: glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow *window = glfwCreateWindow(windowWidth, windowHeight, "VirtualBoyGo (2D debug)", nullptr, nullptr);
    if (!window)
    {
        std::fprintf(stderr, "VirtualBoyGo 2D: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    VulkanRenderer renderer;
    UiRenderer uiRenderer;
    Emulator emulator;
    AppMenu appMenu;
    AppSettings settings;
    settings.Load(platform); // no-op (defaults stand) on first run/missing file
    ApplyDefaultGameplayBindings(settings);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFence acquireFence = VK_NULL_HANDLE;
    int exitCode = 0;

    try
    {
        uint32_t glfwExtCount = 0;
        const char **glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        const std::vector<const char *> instanceExtensions(glfwExts, glfwExts + glfwExtCount);

        VkInstance instance = renderer.CreateInstanceStandalone(instanceExtensions);
        CheckVk(glfwCreateWindowSurface(instance, window, nullptr, &surface), "glfwCreateWindowSurface");
        renderer.CreateDeviceForSurface(surface);

        // Prefer an sRGB surface format - matches the color-space handling
        // UiRenderer's image-loading path (UiRenderer::LoadImage) expects.
        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(renderer.GetPhysicalDevice(), surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(renderer.GetPhysicalDevice(), surface, &formatCount, formats.data());
        VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB} : formats[0];
        for (const auto &f : formats)
        {
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB)
            {
                chosen = f;
                break;
            }
        }

        VkExtent2D extent{};
        std::vector<VkImage> swapchainImages;

        // (Re)creates the swapchain at the window's current framebuffer
        // size - called once up front and again whenever that size changes
        // (see the resize check in the render loop below).
        auto recreateSwapchain = [&]()
        {
            int fbWidth = 0, fbHeight = 0;
            glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
            extent = {static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight)};

            vkDeviceWaitIdle(renderer.GetDevice());

            VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            swapchainInfo.surface = surface;
            swapchainInfo.minImageCount = 2;
            swapchainInfo.imageFormat = chosen.format;
            swapchainInfo.imageColorSpace = chosen.colorSpace;
            swapchainInfo.imageExtent = extent;
            swapchainInfo.imageArrayLayers = 1;
            swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            swapchainInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            swapchainInfo.clipped = VK_TRUE;
            swapchainInfo.oldSwapchain = swapchain;

            VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
            CheckVk(vkCreateSwapchainKHR(renderer.GetDevice(), &swapchainInfo, nullptr, &newSwapchain),
                    "vkCreateSwapchainKHR");
            if (swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(renderer.GetDevice(), swapchain, nullptr);
            swapchain = newSwapchain;

            // The old swapchain's images (and UiRenderer's per-image
            // framebuffer cache for them) are now invalid - a new swapchain
            // image can be handed back the same VkImage handle value, which
            // would otherwise hit a stale cache entry pointing at a
            // destroyed framebuffer/view (empty no-op before the first call,
            // since uiRenderer isn't initialized yet at that point).
            uiRenderer.InvalidateRenderTargets();

            uint32_t imageCount = 0;
            vkGetSwapchainImagesKHR(renderer.GetDevice(), swapchain, &imageCount, nullptr);
            swapchainImages.resize(imageCount);
            vkGetSwapchainImagesKHR(renderer.GetDevice(), swapchain, &imageCount, swapchainImages.data());
        };
        recreateSwapchain();

        uiRenderer.Initialize(renderer.GetDevice(), renderer.GetPhysicalDevice(), renderer.GetQueue(),
                              renderer.GetQueueFamilyIndex(), renderer.GetCommandPool(), renderer.GetCommandBuffer());
        emulator.Initialize(uiRenderer, platform);
        emulator.SetTileTracking(true); // see the F9/F10 tools below
        emulator.SetAuthoring(true);    // captures know every fill a painter can paint
        appMenu.Initialize(uiRenderer, chosen.format, emulator, settings, platform, ButtonMappingProfile::Desktop);

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CheckVk(vkCreateFence(renderer.GetDevice(), &fenceInfo, nullptr, &acquireFence), "vkCreateFence");

        std::printf("VirtualBoyGo 2D debug window running (%ux%u)\n", extent.width, extent.height);

        uint32_t buttonStates[3]{};
        uint32_t lastButtonStates[3]{};
        auto lastFrameTime = std::chrono::steady_clock::now();

        // Tab toggles the menu open/closed - not part of buttonStates
        // (that's the menu-navigation/emulator button set, see
        // ButtonMapping.h) since this is an app-level concern AppMenu itself
        // doesn't read input for (see AppMenu::Show/Hide/ToggleOpen).
        // Edge-triggered so holding the key doesn't spam-toggle every frame.
        bool tabWasPressed = false;
        // Experimental tile colorization tools (see Emulator::SetTileDebugView /
        // CaptureTileReference): F9 toggles the per-tile debug view, F10 saves
        // a paint-ready reference of the current frame (Shift+F10: of the
        // right eye - for what games draw for that eye only). Tracking stays on in
        // this desktop debug build so captures always have tile data.
        // F8 toggles the game's color pack (if any) for comparison, F11
        // re-imports its paintings (roms/colorpacks/<rom>/) after editing.
        // F7 starts/stops collecting what the pack doesn't color yet into
        // paint sheets (roms/captures/<rom> todo NNN); F6 saves the whole tile
        // memory as a tile sheet (roms/captures/<rom> tiles NNN).
        bool f9WasPressed = false, f10WasPressed = false, f8WasPressed = false, f11WasPressed = false,
             f7WasPressed = false, f6WasPressed = false;
        bool keyboardWasDown[GLFW_KEY_LAST + 1]{};

        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();

            // Minimized (0x0 framebuffer) - a zero-extent swapchain is
            // invalid, so just wait for the window to become usable again
            // instead of spinning a render loop that can't present anything.
            int fbWidth = 0, fbHeight = 0;
            glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
            if (fbWidth == 0 || fbHeight == 0)
            {
                glfwWaitEvents();
                continue;
            }

            // Recreate the swapchain when the window has actually been
            // resized (cheap check - only rebuilds on an actual size change).
            if (static_cast<uint32_t>(fbWidth) != extent.width || static_cast<uint32_t>(fbHeight) != extent.height)
                recreateSwapchain();

            const auto now = std::chrono::steady_clock::now();
            const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            const bool tabPressed = glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS;
            if (tabPressed && !tabWasPressed)
                appMenu.ToggleOpen();
            tabWasPressed = tabPressed;

            const bool f9Pressed = glfwGetKey(window, GLFW_KEY_F9) == GLFW_PRESS;
            if (f9Pressed && !f9WasPressed)
            {
                emulator.SetTileDebugView(!emulator.IsTileDebugView());
                std::printf("Tile debug view %s\n", emulator.IsTileDebugView() ? "on" : "off");
            }
            f9WasPressed = f9Pressed;
            const bool f10Pressed = glfwGetKey(window, GLFW_KEY_F10) == GLFW_PRESS;
            if (f10Pressed && !f10WasPressed)
            {
                const bool rightEye = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                                      glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
                const std::string captured = emulator.CaptureTileReference(rightEye);
                std::printf(captured.empty() ? (rightEye ? "Right-eye capture: nothing here is drawn for the right eye only\n"
                                                         : "Tile reference capture failed (no game running?)\n")
                                             : "Saved roms/captures/%s.png + .tiles\n",
                            captured.c_str());
            }
            f10WasPressed = f10Pressed;
            const bool f8Pressed = glfwGetKey(window, GLFW_KEY_F8) == GLFW_PRESS;
            if (f8Pressed && !f8WasPressed)
            {
                emulator.SetColorPackEnabled(!emulator.IsColorPackEnabled());
                std::printf("Color pack %s%s\n", emulator.IsColorPackEnabled() ? "on" : "off",
                            emulator.HasColorPack() ? "" : " (no pack loaded for this game)");
            }
            f8WasPressed = f8Pressed;
            const bool f11Pressed = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
            if (f11Pressed && !f11WasPressed)
                std::printf("%s\n", emulator.ReloadColorPack().c_str());
            f11WasPressed = f11Pressed;
            const bool f7Pressed = glfwGetKey(window, GLFW_KEY_F7) == GLFW_PRESS;
            if (f7Pressed && !f7WasPressed)
                std::printf("%s\n", emulator.SetCollectingUncolored(!emulator.IsCollectingUncolored()).c_str());
            f7WasPressed = f7Pressed;
            const bool f6Pressed = glfwGetKey(window, GLFW_KEY_F6) == GLFW_PRESS;
            if (f6Pressed && !f6WasPressed)
            {
                const std::string saved = emulator.CaptureTileSheet();
                std::printf(saved.empty() ? "Tile sheet capture failed (no game running?)\n"
                                          : "Saved roms/captures/%s.png + .tiles\n",
                            saved.c_str());
            }
            f6WasPressed = f6Pressed;

            std::memcpy(lastButtonStates, buttonStates, sizeof(buttonStates));
            PollDesktopButtonState(window, buttonStates);
            appMenu.Update(buttonStates, lastButtonStates, deltaSeconds);

            // Gamepad mapping capture uses physical gamepad edges, never
            // the synthetic keyboard/menu device slots.
            for (int bit = 0; bit < ButtonMapper::EmuButtonCount; ++bit)
            {
                const uint32_t mask = ButtonMapper::ButtonMapping[bit];
                if ((buttonStates[ButtonMapper::DeviceGamepad] & mask) &&
                    !(lastButtonStates[ButtonMapper::DeviceGamepad] & mask))
                    appMenu.SubmitRawMappingInput({true, ButtonMapper::DeviceGamepad, bit});
            }

            // Raw key edges let the mapping page bind any GLFW keyboard key,
            // independent of the fixed menu-navigation controls above.
            for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key)
            {
                const bool down = glfwGetKey(window, key) == GLFW_PRESS;
                if (down && !keyboardWasDown[key])
                    appMenu.SubmitRawMappingInput({true, ButtonMapper::DeviceKeyboard, key});
                keyboardWasDown[key] = down;
            }

            // Even while paused - a palette change from the menu re-colors
            // the frame already on screen (see Emulator::SetShadePalette).
            const bool packShown = emulator.HasColorPack() && emulator.IsColorPackEnabled();
            emulator.SetShadePalette(settings.EffectiveShadePalette(packShown));

            // Pause emulation while the menu is open so gameplay doesn't
            // keep advancing behind it.
            if (!appMenu.IsOpen())
            {
                emulator.SetGameplayInput(PollGameplayInput(window, settings, appMenu));
                emulator.RunFrame(deltaSeconds);
            }

            // The menu renders at the largest integer logical-to-physical
            // scale (see AppMenuLayout.h's kMenuScale) that still fits the
            // current window, so it's always as big as possible without
            // ever needing to upscale (and blur) its offscreen texture.
            const int scaleX = static_cast<int>(fbWidth / kMenuWidth);
            const int scaleY = static_cast<int>(fbHeight / kMenuHeight);
            const float menuScale = static_cast<float>(std::max(1, std::min(scaleX, scaleY)));
            appMenu.SetMenuScale(uiRenderer, menuScale);
            const float menuX = (static_cast<float>(fbWidth) - kMenuWidth * menuScale) / 2.0f;
            const float menuY = (static_cast<float>(fbHeight) - kMenuHeight * menuScale) / 2.0f;

            if (appMenu.IsVisible())
                appMenu.RenderToBuffer(uiRenderer);

            vkResetFences(renderer.GetDevice(), 1, &acquireFence);
            uint32_t imageIndex = 0;
            const VkResult acquireResult = vkAcquireNextImageKHR(renderer.GetDevice(), swapchain, UINT64_MAX,
                                                                 VK_NULL_HANDLE, acquireFence, &imageIndex);
            if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
            {
                recreateSwapchain();
                continue;
            }
            if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
                continue;
            vkWaitForFences(renderer.GetDevice(), 1, &acquireFence, VK_TRUE, UINT64_MAX);

            // UiRenderer::EndFrame blocks internally (vkQueueWaitIdle) until
            // rendering is complete, so presenting right after is safe
            // without a rendering-finished semaphore.
            uiRenderer.BeginFrame(swapchainImages[imageIndex], chosen.format, extent.width, extent.height,
                                  appMenu.GetBackgroundColor());
            if (emulator.HasScreen())
            {
                emulator.DrawScreen(uiRenderer, 0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                    Emulator::Eye::Left, settings.ScreenTint(packShown), settings.ScreenPattern(packShown));
            }
            if (appMenu.IsVisible())
                appMenu.Draw(uiRenderer, menuX, menuY);
            uiRenderer.EndFrame();

            VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &swapchain;
            presentInfo.pImageIndices = &imageIndex;
            const VkResult presentResult = vkQueuePresentKHR(renderer.GetQueue(), &presentInfo);
            if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
                recreateSwapchain();
        }

        vkDeviceWaitIdle(renderer.GetDevice());
    }
    catch (const std::exception &ex)
    {
        std::fprintf(stderr, "VirtualBoyGo 2D: %s\n", ex.what());
        exitCode = 1;
    }

    // Flush cart SRAM (if any) for whatever ROM is currently loaded - LoadRom
    // already does this on every ROM switch, but app exit has no LoadRom
    // call to piggyback on.
    emulator.Shutdown();

    if (acquireFence != VK_NULL_HANDLE)
        vkDestroyFence(renderer.GetDevice(), acquireFence, nullptr);
    if (swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(renderer.GetDevice(), swapchain, nullptr);
    // Surface must be destroyed before the instance - renderer.Shutdown()
    // destroys the instance, so this has to happen first. uiRenderer also
    // owns Vulkan resources backed by renderer's device, so it must go first.
    if (surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(renderer.GetInstance(), surface, nullptr);
    uiRenderer.Shutdown();
    renderer.Shutdown();

    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
