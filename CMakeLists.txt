cmake_minimum_required(VERSION 3.22)
project(VBoyColor LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single source of truth for the release version (SettingsPage's version
# label) - bump this by hand at each release tag.
set(VBGO_VERSION "1.0.0")

# Deliberately independent of CMAKE_BUILD_TYPE/$<CONFIG>: an optimized
# Release-config build is still just a local dev build (e.g. for on-device
# perf testing) unless this is explicitly set, so it gets the same
# dev.<commit count> version string as a Debug build - see
# cmake/GenerateVersion.cmake. Only flip this ON for the build you're
# actually cutting as a numbered release (Android: `./gradlew assembleRelease
# -Pofficial=true`; PC: `-DVBGO_RELEASE_BUILD=ON` on the cmake configure line).
option(VBGO_RELEASE_BUILD "This is an official numbered release, not a dev build" OFF)
# A pre-release's label (beta.2 for the tag v1.0.0-beta.2): its builds are
# "v<VERSION>-<label>" - set by CI for such a tag (see .github/workflows).
set(VBGO_PRERELEASE "" CACHE STRING "Pre-release label, e.g. beta.2 (empty: none)")

# MSVC's default source/execution charset isn't UTF-8 - without this, UTF-8
# string literals in source (e.g. any future non-ASCII text in this project)
# get silently mangled at compile time before the UI's UTF-8-aware text
# rendering (see UiFontManager::EnsureGlyphsForText) ever sees them.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Android: native code is always optimized, also in the debug variant that
# Android Studio's Run/Debug buttons build - otherwise that variant compiles
# with no optimization at all (CMAKE_BUILD_TYPE=Debug), which makes the
# emulator and the coloring 2-3x slower and the games stutter on the headset.
# -g stays, so crashes still symbolize. To step through native code line by
# line in a debugger, configure with -DVBGO_OPTIMIZE_DEBUG=OFF.
if(ANDROID)
    option(VBGO_OPTIMIZE_DEBUG "Optimize native code in debug builds too" ON)
    if(VBGO_OPTIMIZE_DEBUG)
        add_compile_options($<$<CONFIG:Debug>:-O2>)
    endif()
endif()

include(FetchContent)

# --------------------------------------------------------------------------
# Dependencies - all fetched, no system Vulkan/OpenXR SDK install required.
# --------------------------------------------------------------------------
FetchContent_Declare(
    Vulkan-Headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.290
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Vulkan-Headers)

FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.3.290.0
    GIT_SHALLOW TRUE
)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(volk)
target_link_libraries(volk PUBLIC Vulkan-Headers)

FetchContent_Declare(
    OpenXR-SDK
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.42
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(OpenXR-SDK)

# Font glyph rasterization for the menu UI (core/gfx/UiRenderer). Only needs
# to run at font-load time (baking a glyph atlas), not per-frame.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG VER-2-13-2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(freetype)

# --------------------------------------------------------------------------
# Shader compilation: GLSL -> SPIR-V via glslang's C++ API, driven by a tiny
# build-time compiler tool (no system glslc/glslangValidator needed).
#
# Only built for the host (PC) configuration - the compiler is a build tool
# that must run on the machine doing the build, not on the target (Android
# cross-compiles produce ARM64 binaries that can't run during their own
# build). The generated headers land in the committed core/gfx/generated_shaders/
# directory, so an Android build just consumes whatever the PC build most
# recently produced instead of re-running the compiler itself. Re-run a PC
# configure/build after changing shaders/*.vert|frag to refresh them.
# --------------------------------------------------------------------------
set(VBGO_GENERATED_SHADER_DIR "${CMAKE_SOURCE_DIR}/core/gfx/generated_shaders")

if(NOT ANDROID)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG vulkan-sdk-1.3.290.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glslang)

    add_executable(vbgo_shader_compiler tools/ShaderCompiler.cpp)
    target_link_libraries(vbgo_shader_compiler PRIVATE glslang SPIRV glslang-default-resource-limits)
    target_include_directories(vbgo_shader_compiler PRIVATE ${glslang_SOURCE_DIR})

    function(vbgo_add_shader OUT_VAR SHADER_SOURCE)
        get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
        set(HEADER_OUT "${VBGO_GENERATED_SHADER_DIR}/${SHADER_NAME}.h")
        add_custom_command(
            OUTPUT ${HEADER_OUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${VBGO_GENERATED_SHADER_DIR}"
            COMMAND $<TARGET_FILE:vbgo_shader_compiler> ${SHADER_SOURCE} ${HEADER_OUT}
            DEPENDS ${SHADER_SOURCE} vbgo_shader_compiler
            VERBATIM
        )
        set(${OUT_VAR} ${HEADER_OUT} PARENT_SCOPE)
    endfunction()

    vbgo_add_shader(UI_VERT_HEADER ${CMAKE_SOURCE_DIR}/shaders/ui.vert)
    vbgo_add_shader(UI_SOLID_FRAG_HEADER ${CMAKE_SOURCE_DIR}/shaders/ui_solid.frag)
    vbgo_add_shader(UI_TEXT_FRAG_HEADER ${CMAKE_SOURCE_DIR}/shaders/ui_text.frag)
    vbgo_add_shader(UI_IMAGE_FRAG_HEADER ${CMAKE_SOURCE_DIR}/shaders/ui_image.frag)
    vbgo_add_shader(UI_IMAGE_ROUNDED_FRAG_HEADER ${CMAKE_SOURCE_DIR}/shaders/ui_image_rounded.frag)
    vbgo_add_shader(SCREEN_PATTERN_FRAG_HEADER ${CMAKE_SOURCE_DIR}/shaders/screen_pattern.frag)
    vbgo_add_shader(SCREEN_FILTER_FRAG_HEADER ${CMAKE_SOURCE_DIR}/shaders/screen_filter.frag)
    vbgo_add_shader(ANAGLYPH_FRAG_HEADER ${CMAKE_SOURCE_DIR}/shaders/anaglyph.frag)
    add_custom_target(vbgo_shaders DEPENDS ${UI_VERT_HEADER} ${UI_SOLID_FRAG_HEADER}
                                            ${UI_TEXT_FRAG_HEADER} ${UI_IMAGE_FRAG_HEADER} ${UI_IMAGE_ROUNDED_FRAG_HEADER}
                                            ${SCREEN_PATTERN_FRAG_HEADER} ${SCREEN_FILTER_FRAG_HEADER} ${ANAGLYPH_FRAG_HEADER})

endif()

# --------------------------------------------------------------------------
# Desktop asset embedding: bundles fonts/icons directly into
# the executable as Win32 RCDATA resources (DesktopPlatform::LoadAssetBytes
# loads them via FindResource/LoadResource/LockResource) instead of needing
# loose files next to it. rc.exe packs the raw bytes as-is - unlike a
# generated C array, this doesn't bloat compile time parsing megabytes of
# decimal literals for asset-sized (as opposed to shader-sized) data. Only
# meaningful on Windows; Android already bundles assets via the APK's own
# assets/ + AAssetManager.
# --------------------------------------------------------------------------
if(WIN32 AND NOT ANDROID)
    set(VBGO_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets")
    configure_file(
        ${CMAKE_SOURCE_DIR}/platform/desktop/DesktopAssets.rc.in
        ${CMAKE_BINARY_DIR}/generated/DesktopAssets.rc
        @ONLY
    )
endif()

# --------------------------------------------------------------------------
# Virtual Boy emulator core - libretro/beetle-vb-libretro (a fork of
# Mednafen's VB emulation), vendored as a git submodule at
# third_party/beetle-vb-libretro, GPL-2.0 licensed (see that folder's
# COPYING). Source list mirrors that submodule's own Makefile.common (the
# core has no CMake build of its own - it's normally built via GNU make -
# so this enumerates the same files Makefile.common lists for
# HAVE_GRIFFIN!=1, NEED_BLIP=1, rather than a directory glob, to stay in
# sync with exactly what upstream's own build compiles).
# --------------------------------------------------------------------------
set(VB_CORE_DIR ${CMAKE_SOURCE_DIR}/third_party/beetle-vb-libretro)

# vip.c (and the vip_draw.inc it includes) are compiled from generated
# copies that tag each output pixel with its VB shade index (for per-shade
# color palettes) and hook in the experimental tile tracker - see
# cmake/PatchBeetleVip.cmake. Everything else is compiled straight from the
# untouched submodule.
include(${CMAKE_SOURCE_DIR}/cmake/PatchBeetleVip.cmake)
vbgo_generate_patched_vip(${VB_CORE_DIR} ${CMAKE_BINARY_DIR}/generated/beetle-vb)

add_library(vb_core STATIC
    ${VB_CORE_DIR}/mednafen/hw_cpu/v810/v810_cpu.cpp
    ${VB_CORE_DIR}/mednafen/mempatcher.cpp
    ${VB_CORE_DIR}/libretro.cpp
    ${VB_CORE_DIR}/mednafen/vb/vsu.c
    ${VB_CORE_DIR}/mednafen/vb/input.c
    ${VB_CORE_DIR}/mednafen/vb/timer.c
    ${VBGO_PATCHED_VIP_SOURCE}
    ${VB_CORE_DIR}/mednafen/hw_cpu/v810/fpu-new/softfloat.c
    ${VB_CORE_DIR}/mednafen/sound/Blip_Buffer.c
    ${VB_CORE_DIR}/mednafen/state.c
    ${VB_CORE_DIR}/mednafen/settings.c
    ${VB_CORE_DIR}/libretro-common/compat/compat_strl.c
    ${VB_CORE_DIR}/libretro-common/compat/compat_snprintf.c
    core/emu/vbgo_tiletrack.c
)
target_include_directories(vb_core PUBLIC
    ${VB_CORE_DIR}
    ${VB_CORE_DIR}/mednafen
    ${VB_CORE_DIR}/mednafen/include
    ${VB_CORE_DIR}/libretro-common/include
)
# The generated vip.c lives outside the submodule, so its quoted includes
# ("vb.h", "vip_draw.inc", "../math_ops.h", ...) can no longer resolve
# relative to its own folder - point them at the original one instead.
target_include_directories(vb_core PRIVATE ${VB_CORE_DIR}/mednafen/vb ${CMAKE_SOURCE_DIR}/core/emu)
# Matches upstream's own Makefile flags (see Makefile/Makefile.common) for a
# WANT_32BPP, non-static-linking, non-HAVE_GRIFFIN build.
target_compile_definitions(vb_core PRIVATE
    WANT_32BPP
    __LIBRETRO__
    STDC_HEADERS
    __STDC_LIMIT_MACROS
    MEDNAFEN_VERSION="0.9.31"
    MEDNAFEN_VERSION_NUMERIC=931
)
if(MSVC)
    # Mirrors the Makefile's windows_msvc* platform block - the submodule
    # already ships MSVC-specific compat code (mednafen/msvc_compat.h,
    # mednafen/hw_cpu/v810/v810_op_table_msvc.inc) for exactly this.
    target_compile_definitions(vb_core PRIVATE INLINE=_inline NOMINMAX _CRT_SECURE_NO_DEPRECATE)
else()
    target_compile_definitions(vb_core PRIVATE INLINE=inline)
endif()

# --------------------------------------------------------------------------
# Core app: OpenXR session/swapchain lifecycle + Vulkan rendering. Shared,
# platform-agnostic code - the actual per-platform entry points (pc/, pc2d/,
# android/) live under platform/ and just call into this.
# --------------------------------------------------------------------------
add_library(vbgo_app STATIC
    core/app/OpenXrApp.cpp
    core/gfx/VulkanRenderer.cpp
    core/gfx/UiRenderer.cpp
    core/gfx/UiRendererPipelines.cpp
    core/gfx/UiFontManager.cpp
    core/gfx/UiVulkanUtils.cpp
    core/gfx/UiIconSet.cpp
    core/menu/AppMenu.cpp
    core/menu/MenuPage.cpp
    core/menu/MenuWidgets.cpp
    core/menu/ThumbnailLibrary.cpp
    core/menu/pages/LibraryGrid.cpp
    core/menu/pages/LibraryPage.cpp
    core/menu/pages/SaveStatesPage.cpp
    core/menu/pages/SettingsPage.cpp
    core/menu/pages/AboutPage.cpp
    core/menu/pages/EmulatorButtonMapPage.cpp
    core/menu/pages/MoveScreenPage.cpp
    core/menu/pages/FolderPage.cpp
    core/emu/Emulator.cpp
    core/emu/ShadeColorizer.cpp
    core/emu/TileColorPack.cpp
    core/emu/ColorPackRenderer.cpp
    core/emu/DepthColors.cpp
    core/emu/UncoloredCollector.cpp
    core/emu/FrameRecorder.cpp
    core/emu/AudioOutput.cpp
    core/emu/TasMovie.cpp
    core/input/XrInput.cpp
    core/input/ButtonMapping.cpp
    core/io/Settings.cpp
    core/io/DataFolders.cpp
    core/input/TouchControls.cpp
)
target_include_directories(vbgo_app PUBLIC core ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_SOURCE_DIR}/platform)
target_link_libraries(vbgo_app PUBLIC openxr_loader volk freetype vb_core)
target_compile_definitions(vbgo_app PUBLIC XR_USE_GRAPHICS_API_VULKAN)
if(NOT ANDROID)
    add_dependencies(vbgo_app vbgo_shaders)
endif()

# Generates generated/Version.h (kGeneratedVersionString - see
# AppMenuLayout.h) fresh on every build, not just every configure, so the
# commit count always reflects HEAD even if nothing else triggered a
# reconfigure.
set(VBGO_GENERATED_VERSION_DIR "${CMAKE_BINARY_DIR}/generated")
add_custom_target(vbgo_version ALL
    COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -DOUT_FILE=${VBGO_GENERATED_VERSION_DIR}/Version.h
            -DVBGO_VERSION=${VBGO_VERSION}
            -DVBGO_RELEASE_BUILD=${VBGO_RELEASE_BUILD}
            -DVBGO_PRERELEASE=${VBGO_PRERELEASE}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenerateVersion.cmake
    BYPRODUCTS ${VBGO_GENERATED_VERSION_DIR}/Version.h
    COMMENT "Generating version header"
)
target_include_directories(vbgo_app PUBLIC ${VBGO_GENERATED_VERSION_DIR})
add_dependencies(vbgo_app vbgo_version)

if(ANDROID)
    target_compile_definitions(vbgo_app PUBLIC XR_USE_PLATFORM_ANDROID)
    target_sources(vbgo_app PRIVATE platform/android/AndroidPlatform.cpp)
else()
    target_sources(vbgo_app PRIVATE platform/desktop/DesktopPlatform.cpp)
    if(WIN32)
        target_link_libraries(vbgo_app PUBLIC winhttp) # (DesktopPlatform's downloads)
    endif()
endif()

if(ANDROID)
    find_library(ANDROID_LIB android)
    find_library(LOG_LIB log)

    add_library(vboycolor SHARED
        platform/android/AndroidMain.cpp
        platform/android/PhoneMain.cpp
    )
    target_link_libraries(vboycolor PRIVATE vbgo_app android_native_app_glue ${ANDROID_LIB} ${LOG_LIB})
    target_include_directories(vboycolor PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
    # ANativeActivity_onCreate (in android_native_app_glue.c) is only looked
    # up dynamically by the OS, never called from our own code, so the
    # linker drops it as unreferenced unless forced to keep it.
    target_link_options(vboycolor PRIVATE "-u" "ANativeActivity_onCreate")

    add_library(android_native_app_glue STATIC
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
    )
    target_include_directories(android_native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
else()
    add_executable(VBoyColorVR
        platform/pc/Main.cpp
    )
    target_link_libraries(VBoyColorVR PRIVATE vbgo_app)
    if(WIN32)
        # Resources must be a source of the executable itself, not the
        # vbgo_app static lib - the linker only pulls .lib members that are
        # referenced by symbol, and a compiled .rc resource has none, so it
        # would silently never make it into the final .exe otherwise.
        target_sources(VBoyColorVR PRIVATE ${CMAKE_BINARY_DIR}/generated/DesktopAssets.rc)
    endif()

    # MSVC's incremental linker has been observed to silently keep a stale
    # .exe (identical byte-for-byte) across rebuilds when only an upstream
    # static lib's contents changed, without erroring - forcing a full
    # relink every time trades a bit of link speed for actually picking up
    # changes, which matters a lot for the F5 edit-build-run loop.
    if(MSVC)
        target_link_options(VBoyColorVR PRIVATE "/INCREMENTAL:NO")
    endif()

    if(TARGET openxr_loader AND WIN32)
        get_target_property(OPENXR_LOADER_TYPE openxr_loader TYPE)
        if(OPENXR_LOADER_TYPE STREQUAL "SHARED_LIBRARY")
            add_custom_command(
                TARGET VBoyColorVR POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:openxr_loader> $<TARGET_FILE_DIR:VBoyColorVR>
            )
        endif()
    endif()

    # Fonts/icons are embedded into the binary as Win32
    # resources (see DesktopAssets.rc.in above) - only the ROMs folder needs
    # to exist next to the exe.
    add_custom_command(
        TARGET VBoyColorVR POST_BUILD
        COMMAND ${CMAKE_COMMAND} -D "SRC=${CMAKE_SOURCE_DIR}/colorpacks" -D "DST=$<TARGET_FILE_DIR:VBoyColorVR>/colorpacks"
                -P "${CMAKE_SOURCE_DIR}/cmake/BundleColorPacks.cmake"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VBoyColorVR>/roms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/assets/PlaceRomsHere.txt" "$<TARGET_FILE_DIR:VBoyColorVR>/roms"
    )

    # --------------------------------------------------------------------
    # Flat desktop window, no headset/OpenXR runtime needed at all - the
    # fast local-iteration debug build. Reuses the same UiRenderer/AppMenu
    # content-drawing code as the composition-layer quads on the headset
    # builds, just presented into a normal window swapchain instead of an
    # OpenXR session.
    # --------------------------------------------------------------------
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)

    add_executable(VBoyColor
        platform/pc2d/Main.cpp
    )
    target_link_libraries(VBoyColor PRIVATE vbgo_app glfw)
    if(MSVC)
        target_link_options(VBoyColor PRIVATE "/INCREMENTAL:NO")
    endif()
    if(WIN32)
        # See VBoyColorVR's identical target_sources call above for why
        # this can't just live on the shared vbgo_app static lib instead.
        target_sources(VBoyColor PRIVATE ${CMAKE_BINARY_DIR}/generated/DesktopAssets.rc)
    endif()

    # Fonts/icons are embedded into the binary as Win32
    # resources (see DesktopAssets.rc.in above) - only the ROMs folder needs
    # to exist next to the exe.
    add_custom_command(
        TARGET VBoyColor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -D "SRC=${CMAKE_SOURCE_DIR}/colorpacks" -D "DST=$<TARGET_FILE_DIR:VBoyColor>/colorpacks"
                -P "${CMAKE_SOURCE_DIR}/cmake/BundleColorPacks.cmake"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VBoyColor>/roms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/assets/PlaceRomsHere.txt" "$<TARGET_FILE_DIR:VBoyColor>/roms"
    )
    if(NOT WIN32)
        # Off Windows the fonts and icons are loose files next to the
        # executable (DesktopPlatform::LoadAssetBytes).
        add_custom_command(
            TARGET VBoyColor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/assets/runtime/fonts" "$<TARGET_FILE_DIR:VBoyColor>/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/assets/runtime/icons" "$<TARGET_FILE_DIR:VBoyColor>/icons"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/assets/runtime/logo" "$<TARGET_FILE_DIR:VBoyColor>/logo"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/assets/runtime/icon.png" "$<TARGET_FILE_DIR:VBoyColor>/icon.png"
        )
    endif()
endif()
