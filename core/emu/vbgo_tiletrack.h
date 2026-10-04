/* VirtualBoyGo tile tracking - records, for every pixel the Virtual Boy's
 * VIP draws, which 8x8 character (tile) it came from and where inside that
 * tile, so a frontend can recolor by tile (per-game color packs) instead of
 * only by shade. Experimental.
 *
 * Hooked into the Beetle VB core through generated copies of vip.c and
 * vip_draw.inc (see cmake/PatchBeetleVip.cmake) - the submodule itself stays
 * untouched. Plain C, shared by the core (vip.c) and the frontend.
 *
 * Off by default; while off, the core draws exactly as upstream (only a
 * flag check per drawn pixel). Turn on with vbgo_tiletrack_set_enabled().
 */
#ifndef VBGO_TILETRACK_H
#define VBGO_TILETRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Frontend API ------------------------------------------------------ */

/* Per displayed pixel, both eyes: VBGO_TT_EYE_PIXELS entries per eye, eye 0
 * (left half of the side-by-side output) first, rows of 384. Zero = no tile
 * (background color, or drawn directly into the framebuffer by the CPU). */
#define VBGO_TT_WIDTH  384
#define VBGO_TT_HEIGHT 224
#define VBGO_TT_EYE_PIXELS (VBGO_TT_WIDTH * VBGO_TT_HEIGHT)

/* Field layout of one entry. */
#define VBGO_TT_HASH(v)    ((uint32_t)(v))              /* hash of the tile's 8x8 pixel data */
#define VBGO_TT_SUBX(v)    ((unsigned)((v) >> 32) & 7)  /* x inside the tile (tile space, flips undone) */
#define VBGO_TT_SUBY(v)    ((unsigned)((v) >> 35) & 7)  /* y inside the tile */
#define VBGO_TT_PALETTE(v) ((unsigned)((v) >> 38) & 3)  /* GPLT/JPLT palette number used */
#define VBGO_TT_IS_OBJ(v)  ((unsigned)((v) >> 40) & 1)  /* 1 = sprite (OBJ), 0 = background map */
#define VBGO_TT_PIXEL(v)   ((unsigned)((v) >> 41) & 3)  /* raw 2-bit tile pixel value (1-3) */
#define VBGO_TT_WORLD(v)   ((unsigned)((v) >> 43) & 31) /* world (layer) 0-31 that drew it */
#define VBGO_TT_CHAR(v)    ((unsigned)((v) >> 48) & 0x7FF) /* character slot (debug only - slots get reused) */
#define VBGO_TT_VALID(v)   ((unsigned)((v) >> 63) & 1)

void vbgo_tiletrack_set_enabled(bool enabled);
bool vbgo_tiletrack_is_enabled(void);

/* Tile info for the frame most recently output by retro_run(), laid out as
 * described above (2 * VBGO_TT_EYE_PIXELS entries). Contents are only
 * meaningful while tracking is enabled. */
const uint64_t *vbgo_tiletrack_frame(void);

/* The 8 rows (2 bits per pixel, pixel 0 in the low bits) of the tile with
 * this hash, as last seen while tracking. False if never seen. */
bool vbgo_tiletrack_tile_rows(uint32_t hash, uint16_t rows_out[8]);

/* The VB's character memory as of the last frame drawn while tracking: 2048
 * tiles of 8 rows (2 bits per pixel, pixel 0 in the low bits), everything
 * the game has loaded at the moment - shown on screen or not. NULL until a
 * frame was drawn with tracking on. */
const uint16_t *vbgo_tiletrack_chr_ram(void);

/* The hash tile records use for a tile with these 8 rows. */
uint32_t vbgo_tiletrack_hash_rows(const uint16_t rows[8]);

/* ---- Core hooks (called from the patched vip.c / vip_draw.inc only) ---- */

extern int vbgo_tt_on;
extern uint32_t vbgo_tt_world;
extern const uint8_t *vbgo_tt_block_base;
extern uint32_t vbgo_tt_block[2 * 512 * 8];

/* Packed per drawn pixel while a block of 8 lines is being drawn; converted
 * to the 64-bit form (with the tile hash) when the block is stored. */
#define VBGO_TT_TAG(target_ptr, chr, sx, sy, pal, obj, pv)                                          \
   do                                                                                             \
   {                                                                                              \
      if (vbgo_tt_on)                                                                             \
         vbgo_tt_block[(const uint8_t *)(target_ptr) - vbgo_tt_block_base] =                       \
            0x80000000u | (uint32_t)(chr) | ((uint32_t)(sx) << 11) | ((uint32_t)(sy) << 14) |    \
            ((uint32_t)(pal) << 17) | ((uint32_t)(obj) << 19) | ((uint32_t)(pv) << 20) |         \
            (vbgo_tt_world << 22);                                                                \
   } while (0)

void vbgo_tiletrack_begin_block(const uint8_t *drawing_buffers, const uint16_t *chr_ram, unsigned block_no);
void vbgo_tiletrack_end_block(unsigned fb, unsigned block_no);
void vbgo_tiletrack_display_column(unsigned fb, unsigned lr, unsigned dest_lr, unsigned column, bool display_active);
void vbgo_tiletrack_cpu_fb_write(unsigned fb, unsigned lr, unsigned offset, unsigned bytes);

#ifdef __cplusplus
}
#endif

#endif
