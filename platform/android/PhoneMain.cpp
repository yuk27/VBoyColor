// The phone and tablet app: the same APK as the Quest's, run flat when the
// device isn't a headset (MainActivity.isHeadset - see AndroidMain.cpp's
// android_main). Like the PC window (platform/pc2d/Main.cpp): the game
// screen and the menu drawn with Vulkan into the app's window - here with
// the Virtual Boy controller on the touch screen (core/input/TouchControls)
// and any gamepad the phone has. The touch controls hide while a gamepad is
// in use and come back on the next touch. Android's Back opens the menu (in
// the menu it goes back); fingers tap and drag in the menu like the Quest's
// lasers.
#define VK_USE_PLATFORM_ANDROID_KHR
#include "android/PhoneMain.h"
#include "android/AndroidPlatform.h"
#include "emu/Emulator.h"
#include "gfx/UiRenderer.h"
#include "gfx/VulkanRenderer.h"
#include "input/ButtonMapping.h"
#include "input/TouchControls.h"
#include "io/Settings.h"
#include "menu/AppMenu.h"
#include "menu/pages/AppMenuLayout.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define LOG_TAG "VBoyColor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace
{
    struct Finger
    {
        float x = 0, y = 0;
        bool up = false; // lifted - kept one more frame, so even the quickest tap is seen
    };

    struct PhoneState
    {
        bool resumed = false;
        ANativeWindow *window = nullptr;
        bool windowChanged = false;
        uint32_t gamepadKeyBits = 0, gamepadAxisBits = 0;
        bool gamepadGuideHeld = false;
        bool gamepadUsed = false; // since the last touch
        bool backHeld = false;
        std::map<int32_t, Finger> fingers;
        bool touched = false; // since the last frame
    };

    void CheckVk(VkResult result, const char *what)
    {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string("Vulkan call failed: ") + what + " (" + std::to_string(result) + ")");
    }

    void HandleAppCmd(android_app *app, int32_t cmd)
    {
        auto *state = static_cast<PhoneState *>(app->userData);
        switch (cmd)
        {
        case APP_CMD_INIT_WINDOW:
            state->window = app->window;
            state->windowChanged = true;
            break;
        case APP_CMD_TERM_WINDOW:
            state->window = nullptr;
            state->windowChanged = true;
            break;
        case APP_CMD_RESUME:
            state->resumed = true;
            break;
        case APP_CMD_PAUSE:
            state->resumed = false;
            state->gamepadKeyBits = state->gamepadAxisBits = 0;
            state->gamepadGuideHeld = state->backHeld = false;
            state->fingers.clear();
            break;
        default:
            break;
        }
    }

    // AKEYCODE_* -> ButtonMapper::EmuButton_* bit (as AndroidMain.cpp's).
    uint32_t GamepadKeyBit(int32_t keyCode)
    {
        using namespace ButtonMapper;
        switch (keyCode)
        {
        case AKEYCODE_BUTTON_A:
            return ButtonMapping[EmuButton_A];
        case AKEYCODE_BUTTON_B:
            return ButtonMapping[EmuButton_B];
        case AKEYCODE_BUTTON_X:
            return ButtonMapping[EmuButton_X];
        case AKEYCODE_BUTTON_Y:
            return ButtonMapping[EmuButton_Y];
        case AKEYCODE_BUTTON_L1:
            return ButtonMapping[EmuButton_LShoulder];
        case AKEYCODE_BUTTON_R1:
            return ButtonMapping[EmuButton_RShoulder];
        case AKEYCODE_BUTTON_START:
            return ButtonMapping[EmuButton_Enter];
        case AKEYCODE_BUTTON_SELECT:
            return ButtonMapping[EmuButton_Back];
        case AKEYCODE_DPAD_UP:
            return ButtonMapping[EmuButton_Up];
        case AKEYCODE_DPAD_DOWN:
            return ButtonMapping[EmuButton_Down];
        case AKEYCODE_DPAD_LEFT:
            return ButtonMapping[EmuButton_Left];
        case AKEYCODE_DPAD_RIGHT:
            return ButtonMapping[EmuButton_Right];
        default:
            return 0;
        }
    }

    int32_t HandleInputEvent(android_app *app, AInputEvent *event)
    {
        auto *state = static_cast<PhoneState *>(app->userData);
        const int32_t source = AInputEvent_getSource(event);
        const bool gamepad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                             (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;

        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY)
        {
            const int32_t keyCode = AKeyEvent_getKeyCode(event);
            const bool down = AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN;
            if (keyCode == AKEYCODE_BACK && !gamepad)
            {
                state->backHeld = down; // (the menu - not leaving the app)
                return 1;
            }
            if (!gamepad)
                return 0;
            if (keyCode == AKEYCODE_BUTTON_MODE)
            {
                state->gamepadGuideHeld = down;
                state->gamepadUsed = true;
                return 1;
            }
            const uint32_t bit = GamepadKeyBit(keyCode);
            if (bit == 0)
                return keyCode == AKEYCODE_BACK ? 1 : 0; // (a pad's B can arrive as Back)
            if (down)
                state->gamepadKeyBits |= bit;
            else
                state->gamepadKeyBits &= ~bit;
            state->gamepadUsed = true;
            return 1;
        }

        if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
            return 0;

        if ((source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN)
        {
            const int32_t raw = AMotionEvent_getAction(event);
            const int32_t action = raw & AMOTION_EVENT_ACTION_MASK;
            const size_t index = static_cast<size_t>((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                                     AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            const size_t count = AMotionEvent_getPointerCount(event);
            for (size_t i = 0; i < count; ++i)
            {
                Finger &f = state->fingers[AMotionEvent_getPointerId(event, i)];
                f.x = AMotionEvent_getX(event, i);
                f.y = AMotionEvent_getY(event, i);
            }
            if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP)
                state->fingers[AMotionEvent_getPointerId(event, action == AMOTION_EVENT_ACTION_UP ? 0 : index)].up = true;
            if (action == AMOTION_EVENT_ACTION_CANCEL)
                for (auto &entry : state->fingers)
                    entry.second.up = true;
            if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)
                state->fingers[AMotionEvent_getPointerId(event, index)].up = false;
            state->touched = true;
            state->gamepadUsed = false;
            return 1;
        }

        if (!gamepad)
            return 0;
        using namespace ButtonMapper;
        constexpr float kDeadzone = 0.5f;
        auto axis = [event](int32_t id) { return AMotionEvent_getAxisValue(event, id, 0); };
        const float lx = axis(AMOTION_EVENT_AXIS_X), ly = axis(AMOTION_EVENT_AXIS_Y);
        const float rx = axis(AMOTION_EVENT_AXIS_Z), ry = axis(AMOTION_EVENT_AXIS_RZ);
        const float hx = axis(AMOTION_EVENT_AXIS_HAT_X), hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
        uint32_t bits = 0;
        if (lx < -kDeadzone) bits |= ButtonMapping[EmuButton_LeftStickLeft];
        if (lx > kDeadzone) bits |= ButtonMapping[EmuButton_LeftStickRight];
        if (ly < -kDeadzone) bits |= ButtonMapping[EmuButton_LeftStickUp];
        if (ly > kDeadzone) bits |= ButtonMapping[EmuButton_LeftStickDown];
        if (rx < -kDeadzone) bits |= ButtonMapping[EmuButton_RightStickLeft];
        if (rx > kDeadzone) bits |= ButtonMapping[EmuButton_RightStickRight];
        if (ry < -kDeadzone) bits |= ButtonMapping[EmuButton_RightStickUp];
        if (ry > kDeadzone) bits |= ButtonMapping[EmuButton_RightStickDown];
        if (hx < -kDeadzone) bits |= ButtonMapping[EmuButton_Left];
        if (hx > kDeadzone) bits |= ButtonMapping[EmuButton_Right];
        if (hy < -kDeadzone) bits |= ButtonMapping[EmuButton_Up];
        if (hy > kDeadzone) bits |= ButtonMapping[EmuButton_Down];
        if (bits)
            state->gamepadUsed = true;
        state->gamepadAxisBits = bits;
        return 1;
    }

    // A light vibration (MainActivity.haptic) - an on-screen button pressed.
    void Haptic(android_app *app)
    {
        JNIEnv *env = nullptr;
        app->activity->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        jclass activityClass = env->GetObjectClass(app->activity->clazz);
        jmethodID haptic = env->GetMethodID(activityClass, "haptic", "()V");
        env->DeleteLocalRef(activityClass);
        if (haptic)
            env->CallVoidMethod(app->activity->clazz, haptic);
    }

    // The Vulkan side of the window: surface and swapchain, made again
    // whenever Android hands over a new window (back from the background,
    // a rotation).
    struct Presenter
    {
        VulkanRenderer *renderer = nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        VkExtent2D extent{};
        std::vector<VkImage> images;

        VkSurfaceKHR CreateSurface(VkInstance instance, ANativeWindow *window)
        {
            auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
            if (!create)
                throw std::runtime_error("no vkCreateAndroidSurfaceKHR");
            VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            info.window = window;
            CheckVk(create(instance, &info, nullptr, &surface), "vkCreateAndroidSurfaceKHR");
            return surface;
        }

        void ChooseFormat()
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->GetPhysicalDevice(), surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->GetPhysicalDevice(), surface, &count, formats.data());
            format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
            for (const VkSurfaceFormatKHR &f : formats)
                if (f.format == VK_FORMAT_R8G8B8A8_SRGB || f.format == VK_FORMAT_B8G8R8A8_SRGB)
                {
                    format = f;
                    break;
                }
        }

        void CreateSwapchain(UiRenderer &ui, ANativeWindow *window)
        {
            VkDevice device = renderer->GetDevice();
            vkDeviceWaitIdle(device);
            VkSurfaceCapabilitiesKHR caps{};
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->GetPhysicalDevice(), surface, &caps);
            // The window's own size: Android turns the picture for the
            // display itself (an identity transform).
            extent = {static_cast<uint32_t>(ANativeWindow_getWidth(window)), static_cast<uint32_t>(ANativeWindow_getHeight(window))};
            extent.width = std::clamp(extent.width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
            extent.height = std::clamp(extent.height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));

            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            info.surface = surface;
            info.minImageCount = std::max(2u, caps.minImageCount);
            if (caps.maxImageCount)
                info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
            info.imageFormat = format.format;
            info.imageColorSpace = format.colorSpace;
            info.imageExtent = extent;
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                                    : caps.currentTransform;
            info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                                    : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            info.clipped = VK_TRUE;
            info.oldSwapchain = swapchain;
            VkSwapchainKHR fresh = VK_NULL_HANDLE;
            CheckVk(vkCreateSwapchainKHR(device, &info, nullptr, &fresh), "vkCreateSwapchainKHR");
            if (swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = fresh;
            ui.InvalidateRenderTargets(); // (see pc2d's recreateSwapchain)
            uint32_t count = 0;
            vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
            images.resize(count);
            vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
        }

        void Destroy(UiRenderer &ui)
        {
            if (!renderer || renderer->GetDevice() == VK_NULL_HANDLE)
                return;
            vkDeviceWaitIdle(renderer->GetDevice());
            ui.InvalidateRenderTargets();
            if (swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(renderer->GetDevice(), swapchain, nullptr);
            if (surface != VK_NULL_HANDLE)
                vkDestroySurfaceKHR(renderer->GetInstance(), surface, nullptr);
            swapchain = VK_NULL_HANDLE;
            surface = VK_NULL_HANDLE;
            images.clear();
        }
    };

    // As the PC window's: where the screen goes in a w x h area.
    void FitScreen(float areaX, float areaY, float w, float h, bool wholePixels, float &x, float &y, float &sw, float &sh)
    {
        const float fit = std::min(w / static_cast<float>(Emulator::kPreviewWidth), h / static_cast<float>(Emulator::kPreviewHeight));
        const float whole = std::floor(fit);
        const float scale = wholePixels && whole >= 1.0f ? whole : fit;
        sw = Emulator::kPreviewWidth * scale;
        sh = Emulator::kPreviewHeight * scale;
        x = areaX + std::floor((w - sw) / 2.0f);
        y = areaY + std::floor((h - sh) / 2.0f);
    }
} // namespace

void RunPhoneApp(android_app *app, AndroidPlatform &platform)
{
    PhoneState state;
    app->userData = &state;
    app->onAppCmd = HandleAppCmd;
    app->onInputEvent = HandleInputEvent;
    // (no dimming while playing with a gamepad)
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);

    VulkanRenderer renderer;
    UiRenderer uiRenderer;
    Emulator emulator;
    AppMenu appMenu;
    AppSettings settings;
    TouchControls touch;
    Presenter presenter;
    presenter.renderer = &renderer;
    bool ready = false; // the device, the renderer, the menu - once
    VkFence acquireFence = VK_NULL_HANDLE;
    bool settingsLoaded = false;

    uint32_t buttonStates[3]{}, lastButtonStates[3]{};
    bool menuComboWasHeld = false, backWasHeld = false;
    std::string shownRom;
    auto lastFrameTime = std::chrono::steady_clock::now();

    for (;;)
    {
        // Events: all that are waiting - or, with nothing to show (in the
        // background, no window, the ROMs folder picker up), wait for one.
        bool quit = false;
        for (;;)
        {
            const bool active = state.resumed && state.window && platform.HasRomsFolder();
            int events = 0;
            android_poll_source *source = nullptr;
            if (ALooper_pollAll(active ? 0 : -1, nullptr, &events, reinterpret_cast<void **>(&source)) < 0)
                break;
            const bool wasResumed = state.resumed;
            if (source)
                source->process(app, source);
            if (app->destroyRequested)
            {
                quit = true;
                break;
            }
            // The window is going away: let go of it now, while it still
            // exists. Gone to the background: keep the game's save safe
            // (Android may close the app there without a word).
            if (state.windowChanged && !state.window)
                presenter.Destroy(uiRenderer);
            if (wasResumed && !state.resumed && ready)
                emulator.SaveRamNow();
        }
        if (quit)
            break;

        if (state.windowChanged)
        {
            state.windowChanged = false;
            try
            {
                presenter.Destroy(uiRenderer);
                if (state.window)
                {
                    if (!ready)
                    {
                        const std::vector<const char *> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
                        VkInstance instance = renderer.CreateInstanceStandalone(extensions);
                        presenter.CreateSurface(instance, state.window);
                        renderer.CreateDeviceForSurface(presenter.surface);
                    }
                    else
                        presenter.CreateSurface(renderer.GetInstance(), state.window);
                    if (!ready)
                        presenter.ChooseFormat();
                    presenter.CreateSwapchain(uiRenderer, state.window);
                    if (!ready)
                    {
                        if (!settingsLoaded)
                        {
                            settings.Load(platform);
                            ButtonMapper::ApplyDefaultGamepadBindings(settings.vbButtons);
                            settingsLoaded = true;
                        }
                        uiRenderer.Initialize(renderer.GetDevice(), renderer.GetPhysicalDevice(), renderer.GetQueue(),
                                              renderer.GetQueueFamilyIndex(), renderer.GetCommandPool(), renderer.GetCommandBuffer());
                        emulator.Initialize(uiRenderer, platform);
                        appMenu.Initialize(uiRenderer, presenter.format.format, emulator, settings, platform,
                                           ButtonMappingProfile::Phone);
                        touch.Initialize(uiRenderer, platform);
                        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                        CheckVk(vkCreateFence(renderer.GetDevice(), &fenceInfo, nullptr, &acquireFence), "vkCreateFence");
                        ready = true;
                        LOGI("Phone app running (%ux%u)", presenter.extent.width, presenter.extent.height);
                    }
                }
            }
            catch (const std::exception &ex)
            {
                LOGE("Phone app: %s", ex.what());
                break;
            }
        }
        if (!ready || presenter.swapchain == VK_NULL_HANDLE || !state.resumed)
            continue;

        // The window may have changed size without a new one (a resize).
        if (static_cast<uint32_t>(ANativeWindow_getWidth(state.window)) != presenter.extent.width ||
            static_cast<uint32_t>(ANativeWindow_getHeight(state.window)) != presenter.extent.height)
            presenter.CreateSwapchain(uiRenderer, state.window);

        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

        const float w = static_cast<float>(presenter.extent.width), h = static_cast<float>(presenter.extent.height);
        const int32_t density = AConfiguration_getDensity(app->config);
        const float dp = (density > 0 && density < 0xfffe ? density : 160) / 160.0f;

        // The touch controls: hidden while a gamepad plays, back on a touch.
        if (state.gamepadUsed)
            touch.SetVisible(false);
        else if (state.touched)
            touch.SetVisible(true);
        state.touched = false;
        if (emulator.RomName() != shownRom)
            shownRom = emulator.RomName();
        touch.Layout(w, h, dp, TouchControls::UsesRightDpad(shownRom));

        // The menu: as big as fits.
        const int scaleX = static_cast<int>(w / kMenuWidth), scaleY = static_cast<int>(h / kMenuHeight);
        const float menuScale = static_cast<float>(std::max(1, std::min(scaleX, scaleY)));
        appMenu.SetMenuScale(uiRenderer, menuScale);
        const float menuX = std::floor((w - kMenuWidth * menuScale) / 2.0f), menuY = std::floor((h - kMenuHeight * menuScale) / 2.0f);

        std::vector<TouchControls::Touch> touches;
        for (const auto &[id, f] : state.fingers)
            touches.push_back({id, f.x, f.y});

        // A finger in the menu: tap to pick, drag to scroll (the first one).
        if (appMenu.IsVisible())
        {
            const Finger *first = state.fingers.empty() ? nullptr : &state.fingers.begin()->second;
            const float mx = first ? (first->x - menuX) / menuScale : 0.0f, my = first ? (first->y - menuY) / menuScale : 0.0f;
            appMenu.SetPointer(first != nullptr, mx, my, first && !first->up, 0.0f, false, true);
        }
        else
            appMenu.SetPointer(false, 0, 0, false);

        // Menu buttons: the gamepad's, and Back as the menu's B.
        std::memcpy(lastButtonStates, buttonStates, sizeof(buttonStates));
        buttonStates[ButtonMapper::DeviceGamepad] = state.gamepadKeyBits | state.gamepadAxisBits;
        buttonStates[ButtonMapper::DeviceLeftTouch] = 0;
        buttonStates[ButtonMapper::DeviceRightTouch] =
            state.backHeld && appMenu.IsOpen() ? ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_B] : 0;
        const bool menuWasOpen = appMenu.IsOpen();
        appMenu.Update(buttonStates, lastButtonStates, deltaSeconds);

        // Opening the menu: Back, the gamepad's Guide or Select + Start, the
        // on-screen menu button.
        const uint32_t selectStart = ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_Back] |
                                     ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_Enter];
        const bool combo = (state.gamepadKeyBits & selectStart) == selectStart || state.gamepadGuideHeld;
        if (combo && !menuComboWasHeld)
            appMenu.ToggleOpen();
        menuComboWasHeld = combo;
        if (state.backHeld && !backWasHeld && !menuWasOpen && !appMenu.IsOpen() && emulator.HasGame())
            appMenu.Show();
        backWasHeld = state.backHeld;

        // Gamepad buttons for the mapping page.
        for (int bit = 0; bit < ButtonMapper::EmuButtonCount; ++bit)
        {
            const uint32_t mask = ButtonMapper::ButtonMapping[bit];
            if ((buttonStates[ButtonMapper::DeviceGamepad] & mask) && !(lastButtonStates[ButtonMapper::DeviceGamepad] & mask))
                appMenu.SubmitRawMappingInput({true, ButtonMapper::DeviceGamepad, bit});
        }

        emulator.SetShadePalette(settings.EffectiveShadePalette());
        bool menuPressed = false;
        const uint32_t touchBits = appMenu.IsVisible() ? 0 : touch.Update(touches, menuPressed);
        if (menuPressed && emulator.HasGame())
            appMenu.Show();
        if (touch.NewlyPressed())
            Haptic(app);
        if (!appMenu.IsOpen())
        {
            // (only the gamepad's buttons go through the mapping - the other
            // slots are the menu's)
            uint32_t gameplay[3] = {buttonStates[ButtonMapper::DeviceGamepad], 0, 0};
            appMenu.ApplyGameplayInputSuppression(gameplay);
            emulator.SetGameplayInput(ButtonMapper::TranslateToVBBitmask(gameplay, settings.vbButtons) | touchBits);
            emulator.RunFrame(deltaSeconds);
        }

        // (the lifted fingers have been seen)
        for (auto it = state.fingers.begin(); it != state.fingers.end();)
            it = it->second.up ? state.fingers.erase(it) : std::next(it);

        if (appMenu.IsVisible())
            appMenu.RenderToBuffer(uiRenderer);
        const int anaglyph = AnaglyphOf(settings.screen3D);
        emulator.PrepareScreen(uiRenderer, presenter.format.format, settings.ScreenTint(), settings.ScreenPattern(),
                               settings.screenLook, anaglyph);

        vkResetFences(renderer.GetDevice(), 1, &acquireFence);
        uint32_t imageIndex = 0;
        const VkResult acquired = vkAcquireNextImageKHR(renderer.GetDevice(), presenter.swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                                        acquireFence, &imageIndex);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            presenter.CreateSwapchain(uiRenderer, state.window);
            continue;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            continue;
        vkWaitForFences(renderer.GetDevice(), 1, &acquireFence, VK_TRUE, UINT64_MAX);

        uiRenderer.BeginFrame(presenter.images[imageIndex], presenter.format.format, presenter.extent.width,
                              presenter.extent.height, appMenu.GetBackgroundColor());
        if (emulator.HasScreen())
        {
            float ax = 0, ay = 0, aw = 0, ah = 0;
            touch.ScreenArea(ax, ay, aw, ah);
            const Screen3D mode = static_cast<Screen3D>(settings.screen3D);
            float sx = 0, sy = 0, sw = 0, sh = 0;
            if (mode == Screen3D::SideBySideHalf || mode == Screen3D::SideBySideFull || mode == Screen3D::CrossEyed ||
                mode == Screen3D::TopBottom)
            {
                // A phone viewer or a 3D screen: the whole screen, one eye per half.
                const bool topBottom = mode == Screen3D::TopBottom, half = mode == Screen3D::SideBySideHalf || topBottom;
                const bool cross = mode == Screen3D::CrossEyed;
                for (int eye = 0; eye < 2; ++eye)
                {
                    const float ox = topBottom ? 0.0f : eye * std::floor(w / 2.0f), oy = topBottom ? eye * std::floor(h / 2.0f) : 0.0f;
                    if (half)
                    {
                        FitScreen(0, 0, w, h, settings.screenWholePixels, sx, sy, sw, sh);
                        sx = topBottom ? sx : sx / 2.0f;
                        sw = topBottom ? sw : sw / 2.0f;
                        sy = topBottom ? sy / 2.0f : sy;
                        sh = topBottom ? sh / 2.0f : sh;
                    }
                    else
                        FitScreen(0, 0, std::floor(w / 2.0f), h, settings.screenWholePixels, sx, sy, sw, sh);
                    const Emulator::Eye which = (eye == 0) != cross ? Emulator::Eye::Left : Emulator::Eye::Right;
                    emulator.DrawScreen(uiRenderer, ox + sx, oy + sy, sw, sh, which, settings.ScreenTint(), settings.ScreenPattern(),
                                        settings.screenLook);
                }
            }
            else
            {
                FitScreen(ax, ay, aw, ah, settings.screenWholePixels, sx, sy, sw, sh);
                if (anaglyph > 0)
                    emulator.DrawAnaglyph(uiRenderer, sx, sy, sw, sh, anaglyph, settings.screenLook);
                else
                    emulator.DrawScreen(uiRenderer, sx, sy, sw, sh, Emulator::Eye::Left, settings.ScreenTint(), settings.ScreenPattern(),
                                        settings.screenLook);
            }
        }
        if (appMenu.IsVisible())
            appMenu.Draw(uiRenderer, menuX, menuY);
        else
            touch.Draw(uiRenderer);
        uiRenderer.EndFrame();

        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &presenter.swapchain;
        presentInfo.pImageIndices = &imageIndex;
        const VkResult presented = vkQueuePresentKHR(renderer.GetQueue(), &presentInfo);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
            presenter.CreateSwapchain(uiRenderer, state.window);
    }

    if (ready)
    {
        vkDeviceWaitIdle(renderer.GetDevice());
        emulator.Shutdown();
        if (acquireFence != VK_NULL_HANDLE)
            vkDestroyFence(renderer.GetDevice(), acquireFence, nullptr);
        presenter.Destroy(uiRenderer);
        uiRenderer.Shutdown();
        renderer.Shutdown();
    }
}
