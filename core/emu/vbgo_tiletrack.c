/* See vbgo_tiletrack.h. */
#include "vbgo_tiletrack.h"

#include <string.h>

int vbgo_tt_on = 0;
uint32_t vbgo_tt_world = 0;
const uint8_t *vbgo_tt_block_base = NULL;
uint32_t vbgo_tt_block[2 * 512 * 8];

/* Tile info in the VB's own framebuffer layout - [fb][eye][column * 256 +
 * row], mirroring the core's double-buffered FB - so it's displayed by the
 * same buffer swap logic as the pixels themselves. */
static uint64_t s_fb[2][2][384 * 256];
static uint64_t s_out[2 * VBGO_TT_EYE_PIXELS];

static const uint16_t *s_chr;
static uint32_t s_char_hash[2048];
static bool s_hashes_valid;
static uint8_t s_char_recorded[2048]; /* per frame: char's rows already in s_seen */

/* hash -> tile rows, for every tile seen while tracking (open addressing). */
#define SEEN_CAPACITY 16384
static struct
{
   uint32_t hash;
   uint8_t used;
   uint16_t rows[8];
} s_seen[SEEN_CAPACITY];
static unsigned s_seen_count;

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

static void RefreshHashes(void)
{
   int c;
   for (c = 0; c < 2048; c++)
      s_char_hash[c] = HashChar(&s_chr[c * 8]);
   memset(s_char_recorded, 0, sizeof(s_char_recorded));
   s_hashes_valid = true;
}

static void RecordTile(uint32_t hash, const uint16_t *rows)
{
   unsigned i = hash & (SEEN_CAPACITY - 1);
   unsigned probes;
   for (probes = 0; probes < SEEN_CAPACITY; probes++, i = (i + 1) & (SEEN_CAPACITY - 1))
   {
      if (!s_seen[i].used)
      {
         if (s_seen_count >= SEEN_CAPACITY * 3 / 4)
            return; /* full enough - stop recording new tiles rather than degrade */
         s_seen[i].used = 1;
         s_seen[i].hash = hash;
         memcpy(s_seen[i].rows, rows, sizeof(s_seen[i].rows));
         s_seen_count++;
         return;
      }
      if (s_seen[i].hash == hash)
         return;
   }
}

void vbgo_tiletrack_set_enabled(bool enabled)
{
   vbgo_tt_on = enabled ? 1 : 0;
   if (!enabled)
      s_hashes_valid = false;
}

bool vbgo_tiletrack_is_enabled(void) { return vbgo_tt_on != 0; }

const uint64_t *vbgo_tiletrack_frame(void) { return s_out; }

bool vbgo_tiletrack_tile_rows(uint32_t hash, uint16_t rows_out[8])
{
   unsigned i = hash & (SEEN_CAPACITY - 1);
   unsigned probes;
   for (probes = 0; probes < SEEN_CAPACITY; probes++, i = (i + 1) & (SEEN_CAPACITY - 1))
   {
      if (!s_seen[i].used)
         return false;
      if (s_seen[i].hash == hash)
      {
         memcpy(rows_out, s_seen[i].rows, sizeof(s_seen[i].rows));
         return true;
      }
   }
   return false;
}

void vbgo_tiletrack_begin_block(const uint8_t *drawing_buffers, const uint16_t *chr_ram, unsigned block_no)
{
   if (!vbgo_tt_on)
      return;
   vbgo_tt_block_base = drawing_buffers;
   s_chr = chr_ram;
   /* Once per frame (games update tile graphics between frames), or right
    * after tracking was switched on mid-frame. */
   if (block_no == 0 || !s_hashes_valid)
      RefreshHashes();
   memset(vbgo_tt_block, 0, sizeof(vbgo_tt_block));
   vbgo_tt_world = 0;
}

void vbgo_tiletrack_end_block(unsigned fb, unsigned block_no)
{
   unsigned lr, row, x;
   if (!vbgo_tt_on)
      return;
   for (lr = 0; lr < 2; lr++)
   {
      uint64_t *dst = s_fb[fb & 1][lr];
      for (row = 0; row < 8; row++)
      {
         const uint32_t *src = &vbgo_tt_block[lr * 4096 + row * 512 + 8];
         for (x = 0; x < 384; x++)
         {
            const uint32_t v = src[x];
            uint64_t out = 0;
            if (v >> 31)
            {
               const uint32_t chr = v & 0x7FF;
               const uint32_t hash = s_char_hash[chr];
               if (!s_char_recorded[chr])
               {
                  RecordTile(hash, &s_chr[chr * 8]);
                  s_char_recorded[chr] = 1;
               }
               out = (uint64_t)hash
                   | ((uint64_t)((v >> 11) & 7) << 32)   /* sub x */
                   | ((uint64_t)((v >> 14) & 7) << 35)   /* sub y */
                   | ((uint64_t)((v >> 17) & 3) << 38)   /* palette */
                   | ((uint64_t)((v >> 19) & 1) << 40)   /* obj */
                   | ((uint64_t)((v >> 20) & 3) << 41)   /* raw pixel */
                   | ((uint64_t)((v >> 22) & 31) << 43)  /* world */
                   | ((uint64_t)chr << 48)
                   | ((uint64_t)1 << 63);
            }
            dst[x * 256 + block_no * 8 + row] = out;
         }
      }
   }
}

void vbgo_tiletrack_display_column(unsigned fb, unsigned lr, unsigned dest_lr, unsigned column, bool display_active)
{
   unsigned y;
   uint64_t *dst;
   const uint64_t *src;
   if (!vbgo_tt_on || column >= VBGO_TT_WIDTH)
      return;
   dst = &s_out[(dest_lr & 1) * VBGO_TT_EYE_PIXELS + column];
   src = &s_fb[fb & 1][lr & 1][column * 256];
   for (y = 0; y < VBGO_TT_HEIGHT; y++)
      dst[y * VBGO_TT_WIDTH] = display_active ? src[y] : 0;
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
      if (column >= 384)
         continue;
      for (p = 0; p < 4; p++)
         s_fb[fb & 1][lr & 1][column * 256 + row + p] = 0;
   }
}
