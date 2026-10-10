// Flat desktop window - no OpenXR, no headset required at all. Draws the
// same AppMenu/UiRenderer content the composition-layer quad shows on the
// headset builds, presented into a normal window swapchain instead of an
// OpenXR session, driven by arrow keys/A/S instead of controller
// input. This is the fast local-iteration debug build the emulator/menu
// rendering will eventually show up in without needing to put the headset on.
#include "gfx/VulkanRenderer.h"
#include "emu/Emulator.h"
#include "emu/TasMovie.h"
#include "io/Settings.h"
#include "menu/AppMenu.h"
#include "input/ButtonMapping.h"
#include "gfx/UiRenderer.h"
#include "desktop/DesktopPlatform.h"
#include "input/TouchControls.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <stb_image.h> // (implementation in VulkanRenderer.cpp)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        if (glfwGetKey(window, GLFW_KEY_LEFT_ALT) != GLFW_PRESS && glfwGetKey(window, GLFW_KEY_RIGHT_ALT) != GLFW_PRESS)
            menuKey(GLFW_KEY_ENTER, EmuButton_A); // (Alt+Enter: fullscreen)
        menuKey(GLFW_KEY_ESCAPE, EmuButton_B);
        menuKey(GLFW_KEY_BACKSPACE, EmuButton_B);
        menuKey(GLFW_KEY_W, EmuButton_Y); // (the library's list/cards switch)
        menuKey(GLFW_KEY_D, EmuButton_X);

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
            return ((key == GLFW_KEY_S || key == GLFW_KEY_ENTER) && appMenu.SuppressesDesktopKey(EmuButton_A)) ||
                   ((key == GLFW_KEY_A || key == GLFW_KEY_ESCAPE || key == GLFW_KEY_BACKSPACE) &&
                    appMenu.SuppressesDesktopKey(EmuButton_B));
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
        // No Touch controllers here: a Touch binding is the VR app's, from
        // when VBoyColorVR.exe shared this settings file (it has its own,
        // settings-vr.dat, now) - its place goes back to the key default.
        for (MappedButtons &buttons : settings.vbButtons)
            for (MappedButton &b : buttons.Buttons)
                if (b.IsSet && (b.InputDevice == DeviceLeftTouch || b.InputDevice == DeviceRightTouch))
                    b = {};
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
        ApplyDefaultGamepadBindings(settings.vbButtons);
    }

    // Where the game screen goes in a w x h window, the Virtual Boy's shape
    // kept: as big as fits, or (Settings > Screen > Size: Whole pixels) the
    // biggest whole multiple of its pixels that fits.
    void ScreenRect(int w, int h, bool wholePixels, float &x, float &y, float &sw, float &sh)
    {
        const float fit = std::min(w / static_cast<float>(Emulator::kPreviewWidth), h / static_cast<float>(Emulator::kPreviewHeight));
        const float whole = std::floor(fit);
        const float scale = wholePixels && whole >= 1.0f ? whole : fit;
        sw = Emulator::kPreviewWidth * scale;
        sh = Emulator::kPreviewHeight * scale;
        x = std::floor((w - sw) / 2.0f);
        y = std::floor((h - sh) / 2.0f);
    }

    // Settings > Screen > 3D: where each eye's picture (and a copy of the
    // menu) goes in the window. A pane is drawn as if it were a window of
    // its own (vw x vh), placed at (ox, oy) and squeezed by (qx, qy) - a
    // 3D TV's half side by side / top and bottom, stretched back by the TV.
    struct Pane
    {
        float ox, oy, vw, vh, qx, qy;
        Emulator::Eye eye;
    };
    int ScreenPanes(int screen3D, int w, int h, Pane panes[2])
    {
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        const float halfW = std::floor(fw / 2.0f), halfH = std::floor(fh / 2.0f);
        switch (static_cast<Screen3D>(screen3D))
        {
        case Screen3D::SideBySideHalf:
            panes[0] = {0.0f, 0.0f, fw, fh, 0.5f, 1.0f, Emulator::Eye::Left};
            panes[1] = {halfW, 0.0f, fw, fh, 0.5f, 1.0f, Emulator::Eye::Right};
            return 2;
        case Screen3D::SideBySideFull:
        case Screen3D::CrossEyed:
        {
            const bool cross = static_cast<Screen3D>(screen3D) == Screen3D::CrossEyed;
            panes[0] = {0.0f, 0.0f, halfW, fh, 1.0f, 1.0f, cross ? Emulator::Eye::Right : Emulator::Eye::Left};
            panes[1] = {halfW, 0.0f, halfW, fh, 1.0f, 1.0f, cross ? Emulator::Eye::Left : Emulator::Eye::Right};
            return 2;
        }
        case Screen3D::TopBottom:
            panes[0] = {0.0f, 0.0f, fw, fh, 1.0f, 0.5f, Emulator::Eye::Left};
            panes[1] = {0.0f, halfH, fw, fh, 1.0f, 0.5f, Emulator::Eye::Right};
            return 2;
        default: // off, or colored glasses (one picture of both)
            panes[0] = {0.0f, 0.0f, fw, fh, 1.0f, 1.0f, Emulator::Eye::Left};
            return 1;
        }
    }

    // A ROM dropped on the window (GLFW hands over UTF-8 paths), loaded by
    // the render loop.
    // Mouse wheel since last frame (the menu scrolls with it).
    double g_scrollY = 0.0;
    void OnScroll(GLFWwindow *, double, double yoffset) { g_scrollY += yoffset; }

    // F5: the left eye's picture (as normal), both eyes side by side, or the
    // right eye's - for checking how the two differ (e.g. with a Look).
    int g_eyeView = 0; // 0 left, 1 both, 2 right
    // F4 (dev): the phone app's touch controls over the window, the mouse
    // as a finger - for trying their layout without a phone.
    bool g_touchPreview = false;

    std::string g_droppedRom;
    std::string g_droppedMovie; // (a TAS run - .bk2)
    void OnDrop(GLFWwindow *, int count, const char **paths)
    {
        for (int i = 0; i < count; ++i)
        {
            std::string path = paths[i];
            std::string ext = path.size() > 3 ? path.substr(path.size() - 3) : "";
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".vb")
            {
                g_droppedRom = path;
                return;
            }
            if (path.size() > 4 && (path.compare(path.size() - 4, 4, ".bk2") == 0 || path.compare(path.size() - 4, 4, ".BK2") == 0))
            {
                g_droppedMovie = path;
                return;
            }
        }
    }

    // Loads a ROM from anywhere (dropped on the window, or named on the
    // command line - "Open with"); its saves go to the roms folder as usual.
    bool LoadRomFromPath(Emulator &emulator, AppMenu &appMenu, AppSettings &settings, Platform &platform, const std::string &utf8Path)
    {
        try
        {
#if defined(__cpp_char8_t)
            const std::filesystem::path path(reinterpret_cast<const char8_t *>(utf8Path.c_str()));
#else
            const std::filesystem::path path = std::filesystem::u8path(utf8Path);
#endif
            if (!emulator.LoadRom(path.string(), path.stem().string()))
            {
                std::fprintf(stderr, "VBoy Color: couldn't load %s\n", utf8Path.c_str());
                return false;
            }
            settings.ApplyGameColors(platform, emulator.RomName(), emulator.RomCrc()); // this game's colors
            appMenu.Hide();
            return true;
        }
        catch (const std::exception &ex)
        {
            std::fprintf(stderr, "VBoy Color: couldn't load %s (%s)\n", utf8Path.c_str(), ex.what());
            return false;
        }
    }

    // A tool-assisted run (TASVideos' .bk2, dropped on the window or named on
    // the command line): finds its game in the ROMs folder - by name, else by
    // the ROM's SHA-1 the movie records - and plays the run from power-on
    // (see Emulator::LoadRom). F12 records it like any play.
    bool PlayMovieFromPath(Emulator &emulator, AppMenu &appMenu, AppSettings &settings, Platform &platform,
                           const std::string &utf8Path)
    {
        try
        {
#if defined(__cpp_char8_t)
            const std::filesystem::path path(reinterpret_cast<const char8_t *>(utf8Path.c_str()));
#else
            const std::filesystem::path path = std::filesystem::u8path(utf8Path);
#endif
            std::ifstream in(path, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            TasMovie movie;
            std::string error;
            if (!TasMovie::Parse(bytes, movie, error))
            {
                std::fprintf(stderr, "VBoy Color: %s: %s\n", utf8Path.c_str(), error.c_str());
                return false;
            }
            const std::vector<RomEntry> roms = platform.ScanRoms();
            const RomEntry *rom = nullptr;
            for (const RomEntry &entry : roms)
                if (entry.name == movie.gameName)
                    rom = &entry;
            for (size_t i = 0; !rom && !movie.romSha1.empty() && i < roms.size(); ++i)
            {
                const std::vector<uint8_t> data = platform.ReadRomFile(roms[i].fullPath);
                if (!data.empty() && TasMovie::Sha1(data.data(), data.size()) == movie.romSha1)
                    rom = &roms[i];
            }
            if (!rom)
            {
                std::fprintf(stderr, "VBoy Color: the run is for %s - put its ROM in the roms folder\n", movie.gameName.c_str());
                return false;
            }
            if (!emulator.LoadRom(rom->fullPath, rom->name, &movie.frames))
                return false;
            settings.ApplyGameColors(platform, emulator.RomName(), emulator.RomCrc()); // this game's colors
            appMenu.Hide();
            std::printf("Playing %s: %zu frames of %s\n", path.filename().string().c_str(), movie.frames.size(),
                        rom->name.c_str());
            return true;
        }
        catch (const std::exception &ex)
        {
            std::fprintf(stderr, "VBoy Color: couldn't play %s (%s)\n", utf8Path.c_str(), ex.what());
            return false;
        }
    }

    // Alt+Enter: fullscreen on the monitor the window is on, and back.
    void ToggleFullscreen(GLFWwindow *window)
    {
        static int savedX = 100, savedY = 100, savedW = 0, savedH = 0;
        if (GLFWmonitor *current = glfwGetWindowMonitor(window))
        {
            (void)current;
            glfwSetWindowMonitor(window, nullptr, savedX, savedY, savedW, savedH, GLFW_DONT_CARE);
            return;
        }
        glfwGetWindowPos(window, &savedX, &savedY);
        glfwGetWindowSize(window, &savedW, &savedH);
        // (the monitor holding the window's center, else the primary one)
        int count = 0;
        GLFWmonitor **monitors = glfwGetMonitors(&count);
        GLFWmonitor *target = glfwGetPrimaryMonitor();
        const int cx = savedX + savedW / 2, cy = savedY + savedH / 2;
        for (int i = 0; i < count; ++i)
        {
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitors[i], &mx, &my);
            const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
            if (mode && cx >= mx && cx < mx + mode->width && cy >= my && cy < my + mode->height)
                target = monitors[i];
        }
        if (const GLFWvidmode *mode = target ? glfwGetVideoMode(target) : nullptr)
            glfwSetWindowMonitor(window, target, 0, 0, mode->width, mode->height, mode->refreshRate);
    }

} // namespace

int main(int argc, char **argv)
{
    // A ROM named on the command line (before the working directory moves
    // to the exe's folder - see DesktopPlatform's constructor).
    std::string startRom;
    if (argc > 1)
    {
        std::error_code ec;
        startRom = std::filesystem::absolute(argv[1], ec).string();
    }
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
        std::fprintf(stderr, "VBoy Color: glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow *window = glfwCreateWindow(windowWidth, windowHeight, "VBoy Color", nullptr, nullptr);
    if (!window)
    {
        std::fprintf(stderr, "VBoy Color: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwSetDropCallback(window, OnDrop);
    glfwSetScrollCallback(window, OnScroll);
    // (a click or key press shorter than a frame - or made while a frame
    // took long, e.g. while recording - still reads as pressed once)
    glfwSetInputMode(window, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);
    glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);
#if !defined(_WIN32) // (Windows: the exe's GLFW_ICON resource)
    {
        const std::vector<uint8_t> png = platform.LoadAssetBytes("icon.png");
        int w = 0, h = 0, channels = 0;
        if (stbi_uc *pixels = png.empty() ? nullptr : stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4))
        {
            const GLFWimage icon{w, h, pixels};
            glfwSetWindowIcon(window, 1, &icon);
            stbi_image_free(pixels);
        }
    }
#endif

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
        TouchControls touch;
        touch.Initialize(uiRenderer, platform);
        bool f4WasPressed = false;

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CheckVk(vkCreateFence(renderer.GetDevice(), &fenceInfo, nullptr, &acquireFence), "vkCreateFence");

        std::printf("VBoy Color running (%ux%u)\n", extent.width, extent.height);
        if (!startRom.empty())
        {
#if defined(_WIN32)
            const std::string start = std::filesystem::path(startRom).u8string();
#else
            const std::string start = startRom;
#endif
            const bool movie = start.size() > 4 && (start.compare(start.size() - 4, 4, ".bk2") == 0 ||
                                                    start.compare(start.size() - 4, 4, ".BK2") == 0);
            if (movie)
                PlayMovieFromPath(emulator, appMenu, settings, platform, start);
            else
                LoadRomFromPath(emulator, appMenu, settings, platform, start);
        }

        uint32_t buttonStates[3]{};
        uint32_t lastButtonStates[3]{};
        auto lastFrameTime = std::chrono::steady_clock::now();

        // Tab toggles the menu open/closed - not part of buttonStates
        // (that's the menu-navigation/emulator button set, see
        // ButtonMapping.h) since this is an app-level concern AppMenu itself
        // doesn't read input for (see AppMenu::Show/Hide/ToggleOpen).
        // Edge-triggered so holding the key doesn't spam-toggle every frame.
        bool tabWasPressed = false;
        bool escWasPressed = false;
        // Experimental tile colorization tools (see Emulator::SetTileDebugView /
        // CaptureTileReference): F9 toggles the per-tile debug view, F10 saves
        // a paint-ready reference of the current frame (Shift+F10: of the
        // right eye - for what games draw for that eye only). Tracking stays on in
        // this desktop debug build so captures always have tile data.
        // F8 toggles the game's color pack (if any) for comparison, F11
        // re-imports its paintings (roms/colorpacks/<rom>/) after editing.
        // F7 starts/stops collecting what the pack doesn't color yet into
        // paint sheets (roms/captures/<rom> todo NNN); F6 saves the whole tile
        // memory as a tile sheet (roms/captures/<rom> tiles NNN). F12
        // starts/stops recording gameplay for side-by-side videos - original
        // red and colored, frame for frame (roms/recordings/<rom> NNN, see
        // Emulator::ToggleRecording).
        bool f5WasPressed = false;
        bool f9WasPressed = false, f10WasPressed = false, f8WasPressed = false, f11WasPressed = false,
             f7WasPressed = false, f6WasPressed = false, f12WasPressed = false, fullscreenWasPressed = false;
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

            if (!g_droppedRom.empty())
            {
                LoadRomFromPath(emulator, appMenu, settings, platform, g_droppedRom);
                g_droppedRom.clear();
            }
            if (!g_droppedMovie.empty())
            {
                PlayMovieFromPath(emulator, appMenu, settings, platform, g_droppedMovie);
                g_droppedMovie.clear();
            }
            // While a run plays: how far it is, in the title bar.
            {
                static std::string shownTitle = "VBoy Color";
                size_t frame = 0, total = 0;
                std::string title = "VBoy Color";
                if (emulator.MoviePlaying(frame, total))
                {
                    const int seconds = static_cast<int>(frame / 50.27), length = static_cast<int>(total / 50.27);
                    char text[96];
                    std::snprintf(text, sizeof(text), " - playing the TAS: %d:%02d / %d:%02d", seconds / 60, seconds % 60,
                                  length / 60, length % 60);
                    title += text;
                }
                if (title != shownTitle)
                {
                    glfwSetWindowTitle(window, title.c_str());
                    shownTitle = title;
                }
            }

            const bool altDown = glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
            const bool fullscreenPressed = altDown && glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS;
            if (fullscreenPressed && !fullscreenWasPressed)
                ToggleFullscreen(window);
            fullscreenWasPressed = fullscreenPressed;

            // (a gamepad's Guide button or Back + Start together too, as on the
            // Quest - not the left stick click, which happens while playing)
            GLFWgamepadstate menuPad{};
            const bool padMenu = glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &menuPad) &&
                                 (menuPad.buttons[GLFW_GAMEPAD_BUTTON_GUIDE] == GLFW_PRESS ||
                                  (menuPad.buttons[GLFW_GAMEPAD_BUTTON_BACK] == GLFW_PRESS &&
                                   menuPad.buttons[GLFW_GAMEPAD_BUTTON_START] == GLFW_PRESS));
            const bool tabPressed = glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS || padMenu;
            if (tabPressed && !tabWasPressed)
                appMenu.ToggleOpen();
            tabWasPressed = tabPressed;

            const bool f5Pressed = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
            if (f5Pressed && !f5WasPressed)
            {
                g_eyeView = (g_eyeView + 1) % 3;
                static const char *kViews[3] = {"the left eye", "both eyes side by side", "the right eye"};
                std::printf("Showing %s\n", kViews[g_eyeView]);
            }
            f5WasPressed = f5Pressed;
            const bool f4Pressed = glfwGetKey(window, GLFW_KEY_F4) == GLFW_PRESS;
            if (f4Pressed && !f4WasPressed)
            {
                g_touchPreview = !g_touchPreview;
                std::printf("Touch controls preview %s\n", g_touchPreview ? "on (the mouse is a finger)" : "off");
            }
            f4WasPressed = f4Pressed;
            // (a phone's density: the window's height as a phone's ~400 dp)
            touch.Layout(static_cast<float>(fbWidth), static_cast<float>(fbHeight), fbHeight / 400.0f,
                         TouchControls::UsesRightDpad(emulator.RomName()));
            uint32_t touchBits = 0;
            if (g_touchPreview && !appMenu.IsVisible())
            {
                std::vector<TouchControls::Touch> touches;
                if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
                {
                    double cx = 0, cy = 0;
                    int ww = 1, wh = 1;
                    glfwGetCursorPos(window, &cx, &cy);
                    glfwGetWindowSize(window, &ww, &wh);
                    touches.push_back({0, static_cast<float>(cx) * fbWidth / std::max(1, ww), static_cast<float>(cy) * fbHeight / std::max(1, wh)});
                }
                bool menuPressed = false;
                touchBits = touch.Update(touches, menuPressed);
                if (menuPressed && emulator.HasGame())
                    appMenu.Show();
            }
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
            const bool f12Pressed = glfwGetKey(window, GLFW_KEY_F12) == GLFW_PRESS;
            if (f12Pressed && !f12WasPressed)
            {
                if (emulator.IsRecording())
                    std::printf("Recording: writing the last frames...\n");
                std::printf("%s\n", emulator.ToggleRecording().c_str());
            }
            f12WasPressed = f12Pressed;

            // Settings > Screen > 3D: one pane, or one per eye (see ScreenPanes) -
            // the menu shows in each, as big as fits a pane.
            Pane panes[2];
            const int paneCount = ScreenPanes(settings.screen3D, fbWidth, fbHeight, panes);

            // The menu renders at the largest integer logical-to-physical
            // scale (see AppMenuLayout.h's kMenuScale) that still fits the
            // current window (pane), so it's always as big as possible without
            // ever needing to upscale (and blur) its offscreen texture.
            const int scaleX = static_cast<int>(panes[0].vw / kMenuWidth);
            const int scaleY = static_cast<int>(panes[0].vh / kMenuHeight);
            const float menuScale = static_cast<float>(std::max(1, std::min(scaleX, scaleY)));
            appMenu.SetMenuScale(uiRenderer, menuScale);
            const float menuX = std::floor((panes[0].vw - kMenuWidth * menuScale) / 2.0f);
            const float menuY = std::floor((panes[0].vh - kMenuHeight * menuScale) / 2.0f);

            // The mouse, in the menu's own units: hover, click, wheel (over
            // whichever pane's copy of the menu it is).
            {
                double cursorX = 0, cursorY = 0;
                int windowW = 1, windowH = 1;
                glfwGetCursorPos(window, &cursorX, &cursorY);
                glfwGetWindowSize(window, &windowW, &windowH);
                const float px = static_cast<float>(cursorX) * fbWidth / std::max(1, windowW);
                const float py = static_cast<float>(cursorY) * fbHeight / std::max(1, windowH);
                const Pane *over = &panes[0];
                for (int i = 1; i < paneCount; ++i)
                    if (px >= panes[i].ox && py >= panes[i].oy)
                        over = &panes[i];
                const float vx = (px - over->ox) / over->qx, vy = (py - over->oy) / over->qy;
                const float mx = (vx - menuX) / menuScale, my = (vy - menuY) / menuScale;
                const bool inside = glfwGetWindowAttrib(window, GLFW_HOVERED) && mx >= 0 && my >= 0 && mx < kMenuWidth &&
                                    my < kMenuHeight;
                appMenu.SetPointer(inside, mx, my, glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS,
                                   static_cast<float>(-g_scrollY));
                g_scrollY = 0.0;
            }

            std::memcpy(lastButtonStates, buttonStates, sizeof(buttonStates));
            PollDesktopButtonState(window, buttonStates);
            const bool menuWasOpen = appMenu.IsOpen();
            appMenu.Update(buttonStates, lastButtonStates, deltaSeconds);

            // Esc opens the menu from a game too (in the menu it's Back) -
            // after the menu's update, so the press that opens it isn't also
            // taken as Back, and one that just closed it doesn't reopen it.
            const bool escPressed = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (escPressed && !escWasPressed && !menuWasOpen && !appMenu.IsOpen() && emulator.HasGame())
                appMenu.Show();
            escWasPressed = escPressed;

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
            emulator.SetShadePalette(settings.EffectiveShadePalette());

            // Pause emulation while the menu is open so gameplay doesn't
            // keep advancing behind it.
            if (!appMenu.IsOpen())
            {
                // (no game input while Alt is held: Alt+Enter toggles fullscreen, not Start)
                emulator.SetGameplayInput(altDown ? 0 : PollGameplayInput(window, settings, appMenu) | touchBits);
                emulator.RunFrame(deltaSeconds);
            }

            if (appMenu.IsVisible())
                appMenu.RenderToBuffer(uiRenderer);
            const int anaglyph = g_eyeView == 0 ? AnaglyphOf(settings.screen3D) : 0;
            emulator.PrepareScreen(uiRenderer, chosen.format, settings.ScreenTint(), settings.ScreenPattern(),
                                   settings.screenLook, anaglyph);

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
                float sx = 0, sy = 0, sw = 0, sh = 0;
                if (g_eyeView == 1)
                {
                    // (F5) Each eye in its half of the window.
                    const int half = static_cast<int>(extent.width) / 2;
                    ScreenRect(half, static_cast<int>(extent.height), settings.screenWholePixels, sx, sy, sw, sh);
                    emulator.DrawScreen(uiRenderer, sx, sy, sw, sh, Emulator::Eye::Left, settings.ScreenTint(),
                                        settings.ScreenPattern(), settings.screenLook);
                    emulator.DrawScreen(uiRenderer, sx + half, sy, sw, sh, Emulator::Eye::Right, settings.ScreenTint(),
                                        settings.ScreenPattern(), settings.screenLook);
                }
                else if (g_eyeView == 2)
                {
                    // (F5) The right eye's picture.
                    ScreenRect(static_cast<int>(extent.width), static_cast<int>(extent.height), settings.screenWholePixels,
                               sx, sy, sw, sh);
                    emulator.DrawScreen(uiRenderer, sx, sy, sw, sh, Emulator::Eye::Right, settings.ScreenTint(),
                                        settings.ScreenPattern(), settings.screenLook);
                }
                else
                {
                    for (int i = 0; i < paneCount; ++i)
                    {
                        const Pane &p = panes[i];
                        ScreenRect(static_cast<int>(p.vw), static_cast<int>(p.vh), settings.screenWholePixels, sx, sy, sw, sh);
                        float x = p.ox + sx * p.qx, y = p.oy + sy * p.qy, w = sw * p.qx, h = sh * p.qy;
                        if (g_touchPreview && paneCount == 1)
                            touch.ScreenArea(x, y, w, h); // (between the touch controls)
                        if (anaglyph > 0)
                            emulator.DrawAnaglyph(uiRenderer, x, y, w, h, anaglyph, settings.screenLook);
                        else
                            emulator.DrawScreen(uiRenderer, x, y, w, h, p.eye, settings.ScreenTint(), settings.ScreenPattern(),
                                                settings.screenLook);
                    }
                }
            }
            if (g_touchPreview && !appMenu.IsVisible())
                touch.Draw(uiRenderer);
            if (appMenu.IsVisible())
                for (int i = 0; i < paneCount; ++i)
                    appMenu.Draw(uiRenderer, panes[i].ox + menuX * panes[i].qx, panes[i].oy + menuY * panes[i].qy, panes[i].qx,
                                 panes[i].qy);
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
        std::fprintf(stderr, "VBoy Color: %s\n", ex.what());
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
