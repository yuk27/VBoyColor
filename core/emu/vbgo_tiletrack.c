/* See vbgo_tiletrack.h. */
#include "vbgo_tiletrack.h"

#include <string.h>

int vbgo_tt_on = 0;
int vbgo_tt_fill = VBGO_TT_FILLS_NONE;
uint32_t vbgo_tt_world = 0;
const uint8_t *vbgo_tt_block_base = NULL;
uint64_t *vbgo_tt_dst[2];
uint64_t vbgo_tt_stamp_bits = 0;
static const uint8_t s_no_blank[2048];
const uint8_t *vbgo_tt_blank = s_no_blank;
uint8_t vbgo_tt_fill_cells[8192];

/* Tags in the VB's own frame buffer layout - [fb][eye][column * 256 + row],
 * double-buffered like the core's FB, so they're displayed by the same
 * buffer swap logic as the pixels themselves. */
static uint64_t s_fb[2][2][VBGO_TT_WIDTH * VBGO_TT_COLUMN];

/* Per frame buffer: its last drawing pass's stamp, and the character
 * memory / tile hashes / blank flags as that pass started. */
static uint16_t s_stamp[2];
static uint16_t s_next_stamp;
static uint16_t s_chr[2][2048 * 8];
static uint32_t s_hash[2][2048];
static uint8_t s_blank[2][2048];
static int s_have[2];
static int s_pass_open; /* a drawing pass has started (block 0 seen, or tracking switched on mid-pass) */
static int s_last_fb = -1;

/* Per output eye and column: which buffer and eye were shown there
 * (fb | lr << 1 | 4 for "active"), 0 = blank. */
static uint8_t s_disp[2][VBGO_TT_WIDTH];

static uint32_t HashChar(const uint16_t *rows)
{
   /* FNV-1a over the 8 rows' values (byte-order independent). */
   uint32_t h = 2166136261u;
   int i;
   for (i = 0; i < 8; i++)
   {
      h = (h ^ (rows[i] & 0xFF)) * 16777619u;
      h = (h ^ (rows[i] >> 8)) * 16777619u;
   }
   return h;
}

static void StartPass(unsigned fb, const uint16_t *chr_ram)
{
   int c;
   /* A new stamp; on wrap-around forget every old tag, so a pixel left
    * untouched for 65535 passes can't come back to life. */
   if (++s_next_stamp == 0)
   {
      memset(s_fb, 0, sizeof(s_fb));
      memset(s_have, 0, sizeof(s_have));
      s_next_stamp = 1;
   }
   s_stamp[fb] = s_next_stamp;
   vbgo_tt_stamp_bits = (uint64_t)s_next_stamp << 48;
   memcpy(s_chr[fb], chr_ram, sizeof(s_chr[fb]));
   for (c = 0; c < 2048; c++)
   {
      const uint16_t *rows = &s_chr[fb][c * 8];
      s_hash[fb][c] = HashChar(rows);
      s_blank[fb][c] = (rows[0] | rows[1] | rows[2] | rows[3] | rows[4] | rows[5] | rows[6] | rows[7]) == 0;
   }
   /* (Tracking switched on mid-pass: the blocks before this one simply have
    * no tags with this stamp.) */
   s_have[fb] = 1;
   s_pass_open = 1;
}

void vbgo_tiletrack_set_enabled(bool enabled)
{
   vbgo_tt_on = enabled ? 1 : 0;
   if (!enabled)
      s_pass_open = 0;
}

bool vbgo_tiletrack_is_enabled(void) { return vbgo_tt_on != 0; }

void vbgo_tiletrack_set_fill_mode(int mode) { vbgo_tt_fill = mode; }

int vbgo_tiletrack_fill_mode(void) { return vbgo_tt_fill; }

void vbgo_tiletrack_set_fill_cells(const uint8_t *bits)
{
   if (bits)
      memcpy(vbgo_tt_fill_cells, bits, sizeof(vbgo_tt_fill_cells));
   else
      memset(vbgo_tt_fill_cells, 0, sizeof(vbgo_tt_fill_cells));
}

void vbgo_tiletrack_begin_block(const uint8_t *drawing_buffers, const uint16_t *chr_ram, unsigned block_no, unsigned fb)
{
   if (!vbgo_tt_on)
      return;
   fb &= 1;
   /* Once per pass (games update tile graphics between frames), or right
    * after tracking was switched on mid-pass. */
   if (block_no == 0 || !s_pass_open || (int)fb != s_last_fb)
      StartPass(fb, chr_ram);
   s_last_fb = (int)fb;
   vbgo_tt_block_base = drawing_buffers;
   vbgo_tt_blank = s_blank[fb];
   vbgo_tt_dst[0] = &s_fb[fb][0][block_no * 8];
   vbgo_tt_dst[1] = &s_fb[fb][1][block_no * 8];
   vbgo_tt_world = 0;
}

void vbgo_tiletrack_display_column(unsigned fb, unsigned lr, unsigned dest_lr, unsigned column, bool display_active)
{
   if (!vbgo_tt_on || column >= VBGO_TT_WIDTH)
      return;
   s_disp[dest_lr & 1][column] = display_active ? (uint8_t)((fb & 1) | ((lr & 1) << 1) | 4) : 0;
}

void vbgo_tiletrack_cpu_fb_write(unsigned fb, unsigned lr, unsigned offset, unsigned bytes)
{
   /* Pixels the game's CPU writes straight into the framebuffer (not drawn
    * from tiles) - forget whatever tile was there. 4 pixels per byte,
    * 64 bytes per column. */
   unsigned b, p;
   if (!vbgo_tt_on)
      return;
   for (b = 0; b < bytes; b++)
   {
      const unsigned o = offset + b;
      const unsigned column = o / 64, row = (o % 64) * 4;
      if (column >= VBGO_TT_WIDTH)
         continue;
      for (p = 0; p < 4; p++)
         s_fb[fb & 1][lr & 1][column * VBGO_TT_COLUMN + row + p] = 0;
   }
}

static int ShownBuffer(unsigned eye)
{
   /* The buffer the eye's columns came from (they all do in practice). */
   unsigned x;
   for (x = 0; x < VBGO_TT_WIDTH; x++)
      if (s_disp[eye & 1][x])
         return s_disp[eye & 1][x] & 1;
   return -1;
}

bool vbgo_tiletrack_eye_view(unsigned eye, vbgo_tt_eye_view *view)
{
   unsigned x;
   const int fb = ShownBuffer(eye);
   if (fb < 0 || !s_have[fb])
      return false;
   view->stamp = s_stamp[fb];
   view->hashes = s_hash[fb];
   view->blank = s_blank[fb];
   view->chr = s_chr[fb];
   for (x = 0; x < VBGO_TT_WIDTH; x++)
   {
      const uint8_t d = s_disp[eye & 1][x];
      view->columns[x] = (d & 4) && (d & 1) == (unsigned)fb ? &s_fb[fb][(d >> 1) & 1][x * VBGO_TT_COLUMN] : NULL;
   }
   return true;
}

bool vbgo_tiletrack_records(unsigned eye, uint64_t *records, uint32_t *cells, bool with_fills)
{
   vbgo_tt_eye_view view;
   unsigned x, y;
   if (!vbgo_tiletrack_eye_view(eye, &view))
      return false;
   for (x = 0; x < VBGO_TT_WIDTH; x++)
   {
      const uint64_t *col = view.columns[x];
      for (y = 0; y < VBGO_TT_HEIGHT; y++)
      {
         const uint64_t t = col ? col[y] : 0;
         uint64_t out = 0;
         uint32_t cell = 0;
         if (t && (t >> 48) == view.stamp && (with_fills || VBGO_TAG_PIXEL(t)))
         {
            const unsigned chr = VBGO_TAG_CHAR(t);
            out = (uint64_t)view.hashes[chr] | ((uint64_t)VBGO_TAG_SUBX(t) << 32) | ((uint64_t)VBGO_TAG_SUBY(t) << 35) |
                  ((uint64_t)VBGO_TAG_PALETTE(t) << 38) | ((uint64_t)VBGO_TAG_IS_OBJ(t) << 40) |
                  ((uint64_t)VBGO_TAG_PIXEL(t) << 41) | ((uint64_t)VBGO_TAG_WORLD(t) << 43) | ((uint64_t)chr << 48) |
                  ((uint64_t)1 << 63);
            if (VBGO_TAG_HAS_CELL(t))
               cell = 0x80000000u | VBGO_TAG_CELL(t);
         }
         records[y * VBGO_TT_WIDTH + x] = out;
         if (cells)
            cells[y * VBGO_TT_WIDTH + x] = cell;
      }
   }
   return true;
}

const uint16_t *vbgo_tiletrack_chr_ram(void)
{
   const int fb = s_last_fb;
   return fb >= 0 && s_have[fb] ? s_chr[fb] : NULL;
}

uint32_t vbgo_tiletrack_hash_rows(const uint16_t rows[8]) { return HashChar(rows); }
