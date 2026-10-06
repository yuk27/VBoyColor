# --------------------------------------------------------------------------
# Generates patched copies of the Beetle VB core's VIP (video) sources and
# returns the vip.c path in VBGO_PATCHED_VIP_SOURCE. Two independent changes:
#
# 1. Shade tag (always on): every output pixel carries the Virtual Boy shade
#    index (0-3) it was drawn with, plus the current brightness of the
#    brightest shade - what Multicolor palettes need. Explained below.
# 2. Tile tracking hooks (experimental, off unless the frontend enables it):
#    calls into core/emu/vbgo_tiletrack.c from the ~6 places vip_draw.inc
#    puts a tile pixel on screen (and, for background tiles, where it leaves
#    one transparent - "fills"), plus where blocks are drawn, displayed or
#    overwritten by the CPU - so per-tile color packs know which tile, which
#    pixel of it and which BG map cell every screen pixel came from. See
#    vbgo_tiletrack.h.
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
# Done as generated copies with exact-text replacements (rather than editing
# the submodule, or a .patch file applied with git) so the submodule stays
# pristine and nothing beyond CMake itself is needed at build time - Android
# Studio's CMake runs this exactly like the desktop build does. Line endings
# are normalized first, so a CRLF checkout (git autocrlf on Windows) matches
# too. Every anchor must match exactly once: if a future submodule bump
# changes one, configuration fails loudly here instead of a feature silently
# turning into a no-op.
# --------------------------------------------------------------------------
# Replaces the single occurrence of ANCHOR in the variable named VAR.
function(_vbgo_replace_once VAR ANCHOR REPLACEMENT WHERE)
    string(FIND "${${VAR}}" "${ANCHOR}" FIRST_HIT)
    string(FIND "${${VAR}}" "${ANCHOR}" LAST_HIT REVERSE)
    if(FIRST_HIT EQUAL -1 OR NOT FIRST_HIT EQUAL LAST_HIT)
        message(FATAL_ERROR
            "PatchBeetleVip: expected exactly one occurrence of\n${ANCHOR}\nin ${WHERE}. "
            "The beetle-vb-libretro submodule has probably changed - update this anchor in "
            "cmake/PatchBeetleVip.cmake to match.")
    endif()
    string(REPLACE "${ANCHOR}" "${REPLACEMENT}" RESULT "${${VAR}}")
    set(${VAR} "${RESULT}" PARENT_SCOPE)
endfunction()

# Writes TEXT to PATH via a temp file + configure_file(COPYONLY), so the
# generated source's timestamp only changes when its content does - otherwise
# every configure would force the (large) VIP translation unit to recompile.
function(_vbgo_write_if_changed PATH TEXT)
    file(WRITE "${PATH}.tmp" "${TEXT}")
    configure_file("${PATH}.tmp" "${PATH}" COPYONLY)
endfunction()

function(vbgo_generate_patched_vip VB_CORE_DIR OUT_DIR)
    set(VIP_SOURCE "${VB_CORE_DIR}/mednafen/vb/vip.c")
    set(DRAW_SOURCE "${VB_CORE_DIR}/mednafen/vb/vip_draw.inc")

    # Re-run configure (and so this function) whenever upstream's files change,
    # e.g. after a submodule bump.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VIP_SOURCE}" "${DRAW_SOURCE}")

    file(READ "${VIP_SOURCE}" VIP)
    file(READ "${DRAW_SOURCE}" DRAW)
    string(REPLACE "\r\n" "\n" VIP "${VIP}")
    string(REPLACE "\r\n" "\n" DRAW "${DRAW}")

    # ---- 1. Shade tag ------------------------------------------------------
    _vbgo_replace_once(VIP
        [=[BrightCLUT[lr][i] = ColorLUT[lr][BrightnessCache[i]];]=]
        [=[BrightCLUT[lr][i] = ColorLUT[lr][BrightnessCache[i]] | ((uint32)i << 24) | ((uint32)(BrightnessCache[3] >> 2) << 26); /* VirtualBoyGo: shade index + fade tag, see cmake/PatchBeetleVip.cmake */]=]
        "vip.c (RecalcBrightnessCache's BrightCLUT assignment)")

    # ---- 2. Tile tracking hooks: vip.c ---------------------------------------
    _vbgo_replace_once(VIP
        [=[#include "vip.h"]=]
        [=[#include "vip.h"
#include "vbgo_tiletrack.h" /* VirtualBoyGo */]=]
        "vip.c (includes)")
    # Each block of 8 lines: tell the tracker where the block's pixels go
    # (it writes its tags straight into its own frame buffer copy).
    _vbgo_replace_once(VIP
        [=[               VIP_DrawBlock(DrawingBlock, DrawingBuffers[0] + 8, DrawingBuffers[1] + 8);]=]
        [=[               vbgo_tiletrack_begin_block(DrawingBuffers[0], CHR_RAM, DRAM, DrawingBlock, DrawingFB); /* VirtualBoyGo */
               VIP_DrawBlock(DrawingBlock, DrawingBuffers[0] + 8, DrawingBuffers[1] + 8);]=]
        "vip.c (VIP_DrawBlock call)")
    # Displayed column -> output tags (side-by-side is the only 3D mode the
    # frontend uses; this function is what writes it).
    _vbgo_replace_once(VIP
        [=[   uint32 *target = surface->pixels + Column + (dest_lr ? (384 + VBSBS_Separation) : 0);
   const int32 pitch32 = surface->pitch32;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];
]=]
        [=[   uint32 *target = surface->pixels + Column + (dest_lr ? (384 + VBSBS_Separation) : 0);
   const int32 pitch32 = surface->pitch32;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];

   vbgo_tiletrack_display_column(fb, lr, dest_lr, Column, DisplayActive_arg); /* VirtualBoyGo */
]=]
        "vip.c (CopyFBColumnToTarget_SideBySide_BASE)")
    # Games drawing straight into the framebuffer with the CPU: no tile there.
    _vbgo_replace_once(VIP
        [=[            FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF] = V;]=]
        [=[         {
            FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF] = V;
            vbgo_tiletrack_cpu_fb_write((A >> 15) & 1, (A >> 16) & 1, A & 0x7FFF, 1); /* VirtualBoyGo */
         }]=]
        "vip.c (VIP_Write8 framebuffer store)")
    _vbgo_replace_once(VIP
        [=[            StoreU16_LE((uint16 *)&FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF], V);]=]
        [=[         {
            StoreU16_LE((uint16 *)&FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF], V);
            vbgo_tiletrack_cpu_fb_write((A >> 15) & 1, (A >> 16) & 1, A & 0x7FFF, 2); /* VirtualBoyGo */
         }]=]
        "vip.c (VIP_Write16 framebuffer store)")

    # ---- 2. Tile tracking hooks: vip_draw.inc ---------------------------------
    # Normal BG maps: remember which map cell (halfword index in DRAM) each
    # character comes from - the overplane character when outside the map.
    _vbgo_replace_once(DRAW
        [=[ uint32 BGMap_Base = bgmap_base_raw << 12;]=]
        [=[ uint32 BGMap_Base = bgmap_base_raw << 12;
 unsigned vbgo_cell = 0; /* VirtualBoyGo */]=]
        "vip_draw.inc (DrawBG locals)")
    _vbgo_replace_once(DRAW
        [=[  bgsc = bgsc_overplane;

  if(SourceX < SourceX_Size && SourceY < SourceY_Size)
   bgsc = BGMap[(BGMap_Base | ((SourceX << 3) & ~0xFFF) | ((SourceX >> 3) & 0x3F)) & 0xFFFF];]=]
        [=[  bgsc = bgsc_overplane;
  vbgo_cell = overplane_char & 0xFFFF; /* VirtualBoyGo */

  if(SourceX < SourceX_Size && SourceY < SourceY_Size)
  {
   vbgo_cell = (BGMap_Base | ((SourceX << 3) & ~0xFFF) | ((SourceX >> 3) & 0x3F)) & 0xFFFF; /* VirtualBoyGo */
   bgsc = BGMap[vbgo_cell];
  }]=]
        "vip_draw.inc (DrawBG map read)")
    # The unrolled 8-pixel path (a whole character row at once) tags its 8
    # pixels afterwards - pixel k shows in-tile x k, or 7 - k when the
    # character is flipped horizontally; transparent ones may become fills.
    _vbgo_replace_once(DRAW
        [=[   x += 7;
   SourceX += 8;]=]
        [=[   if(vbgo_tt_on) /* VirtualBoyGo */
   {
    unsigned int k;
    const int vbgo_fills = vbgo_tt_fills_from(char_no, vbgo_cell);
    for(k = 0; k < 8; k++)
    {
     const unsigned int sub_x = (bgsc & 0x2000) ? 7 - k : k;
     const unsigned int pv = (pixels >> (sub_x * 2)) & 3;
     if(pv)
      VBGO_TT_TAG(&target[x + k], char_no, sub_x, char_sub_y, palette_selector, 0, pv, VBGO_TT_CELL_BITS(vbgo_cell));
     else if(vbgo_fills)
      vbgo_tt_fill_px(&target[x + k], char_no, sub_x, char_sub_y, palette_selector, vbgo_cell);
    }
   }

   x += 7;
   SourceX += 8;]=]
        "vip_draw.inc (DrawBG 8-pixel path)")
    _vbgo_replace_once(DRAW
        [=[   if(pixel)
    target[x] = GPLT_Cache[palette_selector][pixel];
   SourceX++;]=]
        [=[   if(pixel)
   {
    target[x] = GPLT_Cache[palette_selector][pixel];
    VBGO_TT_TAG(&target[x], char_no, char_sub_x, char_sub_y, palette_selector, 0, pixel, VBGO_TT_CELL_BITS(vbgo_cell)); /* VirtualBoyGo */
   }
   else if(vbgo_tt_on && vbgo_tt_fills_from(char_no, vbgo_cell)) /* VirtualBoyGo */
    vbgo_tt_fill_px(&target[x], char_no, char_sub_x, char_sub_y, palette_selector, vbgo_cell);
   SourceX++;]=]
        "vip_draw.inc (DrawBG per-pixel store)")
    # Affine (scaled/rotated) maps - no-rotation path, where char_sub_x is a
    # bit offset (2 bits per pixel), and the general path.
    _vbgo_replace_once(DRAW
        [=[ const uint16 *param_ptr = &DRAM[(ParamBase + 8 * (RealY - DestY)) & 0xFFFF];]=]
        [=[ const uint16 *param_ptr = &DRAM[(ParamBase + 8 * (RealY - DestY)) & 0xFFFF];
 unsigned vbgo_cell = 0; /* VirtualBoyGo */]=]
        "vip_draw.inc (DrawAffine locals)")
    _vbgo_replace_once(DRAW
        [=[  bgsc = bgsc_overplane;

  if(SourceX < (SourceX_Size << 9))
   bgsc = BGMap[(BGMap_Base | ((SourceX >> 6) & ~0xFFF) | ((SourceX >> 12) & 0x3F)) & 0xFFFF];]=]
        [=[  bgsc = bgsc_overplane;
  vbgo_cell = OverplaneChar & 0xFFFF; /* VirtualBoyGo */

  if(SourceX < (SourceX_Size << 9))
  {
   vbgo_cell = (BGMap_Base | ((SourceX >> 6) & ~0xFFF) | ((SourceX >> 12) & 0x3F)) & 0xFFFF; /* VirtualBoyGo */
   bgsc = BGMap[vbgo_cell];
  }]=]
        "vip_draw.inc (DrawAffine no-rotation map read)")
    _vbgo_replace_once(DRAW
        [=[  if(pixel)
   target[x] = GPLT_Cache[bgsc >> 14][pixel];]=]
        [=[  if(pixel)
  {
   target[x] = GPLT_Cache[bgsc >> 14][pixel];
   VBGO_TT_TAG(&target[x], bgsc & 0x7FF, char_sub_x >> 1, char_sub_y, bgsc >> 14, 0, pixel, VBGO_TT_CELL_BITS(vbgo_cell)); /* VirtualBoyGo */
  }
  else if(vbgo_tt_on && vbgo_tt_fills_from(bgsc & 0x7FF, vbgo_cell)) /* VirtualBoyGo */
   vbgo_tt_fill_px(&target[x], bgsc & 0x7FF, char_sub_x >> 1, char_sub_y, bgsc >> 14, vbgo_cell);]=]
        "vip_draw.inc (DrawAffine no-rotation store)")
    _vbgo_replace_once(DRAW
        [=[   bgsc = BGMap[(BGMap_Base | m_index | sub_index) & 0xFFFF];
  }]=]
        [=[   vbgo_cell = (BGMap_Base | m_index | sub_index) & 0xFFFF; /* VirtualBoyGo */
   bgsc = BGMap[vbgo_cell];
  }
  else
   vbgo_cell = OverplaneChar & 0xFFFF; /* VirtualBoyGo */]=]
        "vip_draw.inc (DrawAffine general map read)")
    _vbgo_replace_once(DRAW
        [=[  if(pixel)
   target[x] = GPLT_Cache[palette_selector][pixel];

  SourceX += dx;
  SourceY += dy;]=]
        [=[  if(pixel)
  {
   target[x] = GPLT_Cache[palette_selector][pixel];
   VBGO_TT_TAG(&target[x], char_no, char_sub_x, char_sub_y, palette_selector, 0, pixel, VBGO_TT_CELL_BITS(vbgo_cell)); /* VirtualBoyGo */
  }
  else if(vbgo_tt_on && vbgo_tt_fills_from(char_no, vbgo_cell)) /* VirtualBoyGo */
   vbgo_tt_fill_px(&target[x], char_no, char_sub_x, char_sub_y, palette_selector, vbgo_cell);

  SourceX += dx;
  SourceY += dy;]=]
        "vip_draw.inc (DrawAffine general store)")
    # Sprites: pixel i of the char row is stored on loop iteration i (8 - meow),
    # whichever direction the target walks for horizontal flip. No map cell -
    # the OBJ number instead (both eyes draw the same OBJ, so it ties a
    # sprite's pixels in one eye to the other's); no fills (a sprite's
    # transparent pixels aren't part of it).
    _vbgo_replace_once(DRAW
        [=[      if(pixels & 3)
       *target = JPLT_Cache[palette_selector][pixels & 3];
      target--;]=]
        [=[      if(pixels & 3)
      {
       *target = JPLT_Cache[palette_selector][pixels & 3];
       VBGO_TT_TAG(target, char_no, 8 - meow, char_sub_y, palette_selector, 1, pixels & 3, VBGO_TT_OBJ_BITS(oam)); /* VirtualBoyGo */
      }
      target--;]=]
        "vip_draw.inc (DrawOBJ flipped store)")
    _vbgo_replace_once(DRAW
        [=[      if(pixels & 3)
       *target = JPLT_Cache[palette_selector][pixels & 3];
      target++;]=]
        [=[      if(pixels & 3)
      {
       *target = JPLT_Cache[palette_selector][pixels & 3];
       VBGO_TT_TAG(target, char_no, 8 - meow, char_sub_y, palette_selector, 1, pixels & 3, VBGO_TT_OBJ_BITS(oam)); /* VirtualBoyGo */
      }
      target++;]=]
        "vip_draw.inc (DrawOBJ store)")
    # Which world (layer) is drawing.
    _vbgo_replace_once(DRAW
        [=[  if(end)
   break;
]=]
        [=[  if(end)
   break;

  vbgo_tt_world = world; /* VirtualBoyGo */
]=]
        "vip_draw.inc (VIP_DrawBlock world loop)")

    # vip.c #includes "vip_draw.inc"; a quoted include resolves next to the
    # including file first, so the generated vip.c picks up the generated
    # vip_draw.inc beside it.
    _vbgo_write_if_changed("${OUT_DIR}/vip.c" "${VIP}")
    _vbgo_write_if_changed("${OUT_DIR}/vip_draw.inc" "${DRAW}")

    set(VBGO_PATCHED_VIP_SOURCE "${OUT_DIR}/vip.c" PARENT_SCOPE)
endfunction()
