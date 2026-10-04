# --------------------------------------------------------------------------
# Generates a patched copy of the Beetle VB core's VIP (video) source that
# tags every output pixel with the Virtual Boy shade index (0-3) it was drawn
# with, plus the current brightness of the brightest shade, and returns its
# path in VBGO_PATCHED_VIP_SOURCE.
#
# Why: per-shade color palettes (Red Viper-style colorization - see
# core/emu/ShadeColorizer.h) need to know *which* of the VB's 4 shades each
# pixel is, not just how bright it ended up. Brightness alone can't tell
# shades apart: games set the 3 shade brightnesses freely (BRTA/BRTB/BRTC)
# and ramp all of them during fades, so two different shades can land on the
# same output value. The core knows the index at exactly one point - its
# 4-entry BrightCLUT (shade index -> output color), rebuilt by
# RecalcBrightnessCache() on every brightness change - so that's where the
# tag goes. The brightest shade's level (BrightnessCache[3]) rides along so
# the colorizer can apply a game's fades as one uniform dim of the whole
# palette, rather than dimming each shade against a guess of what that
# particular game's "normal" brightness is.
#
# The tag lives in the top ("X") byte of the core's XRGB8888 output, which
# the libretro API defines as unused - bits 0-1 the shade index, bits 2-7
# BrightnessCache[3] >> 2 (0-63). The RGB the core outputs is byte-for-byte
# unchanged, so any code that ignores the tag sees the exact same frame as
# before. Emulator::RunFrame always overwrites that byte with opaque alpha
# before uploading anyway.
#
# Done as a one-line generated copy (rather than editing the submodule, or a
# .patch file applied with git) so the submodule stays pristine and nothing
# beyond CMake itself is needed at build time - Android Studio's CMake runs
# this exactly like the desktop build does. If a future submodule bump
# changes the line below, configuration fails loudly here instead of the
# feature silently turning into a no-op.
# --------------------------------------------------------------------------
function(vbgo_generate_patched_vip VB_CORE_DIR OUT_DIR)
    set(VIP_SOURCE "${VB_CORE_DIR}/mednafen/vb/vip.c")
    set(VIP_PATCHED "${OUT_DIR}/vip.c")

    # Re-run configure (and so this function) whenever upstream's file changes,
    # e.g. after a submodule bump.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VIP_SOURCE}")

    file(READ "${VIP_SOURCE}" VIP_TEXT)

    set(ANCHOR "BrightCLUT[lr][i] = ColorLUT[lr][BrightnessCache[i]];")
    set(REPLACEMENT "BrightCLUT[lr][i] = ColorLUT[lr][BrightnessCache[i]] | ((uint32)i << 24) | ((uint32)(BrightnessCache[3] >> 2) << 26); /* VirtualBoyGo: shade index + fade tag, see cmake/PatchBeetleVip.cmake */")

    string(FIND "${VIP_TEXT}" "${ANCHOR}" FIRST_HIT)
    string(FIND "${VIP_TEXT}" "${ANCHOR}" LAST_HIT REVERSE)
    if(FIRST_HIT EQUAL -1 OR NOT FIRST_HIT EQUAL LAST_HIT)
        message(FATAL_ERROR
            "PatchBeetleVip: expected exactly one occurrence of\n  ${ANCHOR}\nin ${VIP_SOURCE}. "
            "The beetle-vb-libretro submodule has probably changed - update the anchor in "
            "cmake/PatchBeetleVip.cmake to match RecalcBrightnessCache()'s BrightCLUT assignment.")
    endif()

    string(REPLACE "${ANCHOR}" "${REPLACEMENT}" VIP_TEXT "${VIP_TEXT}")

    # Write via a temp file + configure_file(COPYONLY) so the generated source's
    # timestamp only changes when its content does - otherwise every configure
    # would force the (large) VIP translation unit to recompile.
    file(WRITE "${VIP_PATCHED}.tmp" "${VIP_TEXT}")
    configure_file("${VIP_PATCHED}.tmp" "${VIP_PATCHED}" COPYONLY)

    set(VBGO_PATCHED_VIP_SOURCE "${VIP_PATCHED}" PARENT_SCOPE)
endfunction()
