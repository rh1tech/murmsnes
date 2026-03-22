/*
 * Color math for palette-indexed transparency
 * See colormath.h for design overview.
 */
#include "colormath.h"
#include "ppu.h"
#include <limits.h>

bool colormath_dirty = true;

/*
 * 8x8x8 coarse grid: for each quantized RGB cell, stores the palette index
 * whose brightness-adjusted color is nearest to the cell center.
 * Grid index = (r>>2) | ((g>>2)<<3) | ((b>>2)<<6), where r/g/b are 5-bit.
 */
static uint8_t nearest_grid[512];

void colormath_rebuild(void)
{
   /* Build array of brightness-adjusted 5-bit RGB per palette entry */
   uint8_t pal_r[256], pal_g[256], pal_b[256];
   for (int i = 0; i < 256; i++) {
      pal_r[i] = IPPU.Red[i];
      pal_g[i] = IPPU.Green[i];
      pal_b[i] = IPPU.Blue[i];
   }

   for (int cell = 0; cell < 512; cell++) {
      /* Cell center in 5-bit space (each cell spans 4 levels, center at +2) */
      int cr = ((cell & 7) << 2) + 2;
      int cg = (((cell >> 3) & 7) << 2) + 2;
      int cb = (((cell >> 6) & 7) << 2) + 2;

      int best_dist = INT_MAX;
      uint8_t best_idx = 0;

      for (int i = 0; i < 256; i++) {
         int dr = cr - pal_r[i];
         int dg = cg - pal_g[i];
         int db = cb - pal_b[i];
         int dist = dr * dr + dg * dg + db * db;

         if (dist < best_dist) {
            best_dist = dist;
            best_idx = (uint8_t)i;
            if (dist == 0) break;
         }
      }
      nearest_grid[cell] = best_idx;
   }
   colormath_dirty = false;
}

static inline uint8_t lookup_nearest(int r, int g, int b)
{
   /* Clamp to 0-31 range */
   if (r < 0) r = 0; else if (r > 31) r = 31;
   if (g < 0) g = 0; else if (g > 31) g = 31;
   if (b < 0) b = 0; else if (b > 31) b = 31;
   return nearest_grid[(r >> 2) | ((g >> 2) << 3) | ((b >> 2) << 6)];
}

uint8_t colormath_nearest(uint16_t rgb15)
{
   return lookup_nearest(rgb15 & 0x1F, (rgb15 >> 5) & 0x1F, (rgb15 >> 10) & 0x1F);
}

uint8_t colormath_add(uint8_t main_idx, uint8_t sub_idx)
{
   int r = IPPU.Red[main_idx]   + IPPU.Red[sub_idx];
   int g = IPPU.Green[main_idx] + IPPU.Green[sub_idx];
   int b = IPPU.Blue[main_idx]  + IPPU.Blue[sub_idx];
   return lookup_nearest(r, g, b);  /* lookup_nearest clamps to 31 */
}

uint8_t colormath_add_half(uint8_t main_idx, uint8_t sub_idx)
{
   int r = (IPPU.Red[main_idx]   + IPPU.Red[sub_idx])   >> 1;
   int g = (IPPU.Green[main_idx] + IPPU.Green[sub_idx]) >> 1;
   int b = (IPPU.Blue[main_idx]  + IPPU.Blue[sub_idx])  >> 1;
   return lookup_nearest(r, g, b);
}

uint8_t colormath_sub(uint8_t main_idx, uint8_t sub_idx)
{
   int r = IPPU.Red[main_idx]   - IPPU.Red[sub_idx];
   int g = IPPU.Green[main_idx] - IPPU.Green[sub_idx];
   int b = IPPU.Blue[main_idx]  - IPPU.Blue[sub_idx];
   return lookup_nearest(r, g, b);  /* lookup_nearest clamps to 0 */
}

uint8_t colormath_sub_half(uint8_t main_idx, uint8_t sub_idx)
{
   int r = (IPPU.Red[main_idx]   - IPPU.Red[sub_idx]);   if (r < 0) r = 0; r >>= 1;
   int g = (IPPU.Green[main_idx] - IPPU.Green[sub_idx]); if (g < 0) g = 0; g >>= 1;
   int b = (IPPU.Blue[main_idx]  - IPPU.Blue[sub_idx]);  if (b < 0) b = 0; b >>= 1;
   return lookup_nearest(r, g, b);
}

uint8_t colormath_fixed_add(uint8_t main_idx, uint16_t fixed_rgb15)
{
   int fr = fixed_rgb15 & 0x1F;
   int fg = (fixed_rgb15 >> 5) & 0x1F;
   int fb = (fixed_rgb15 >> 10) & 0x1F;
   int r = IPPU.Red[main_idx]   + fr;
   int g = IPPU.Green[main_idx] + fg;
   int b = IPPU.Blue[main_idx]  + fb;
   return lookup_nearest(r, g, b);
}

uint8_t colormath_fixed_add_half(uint8_t main_idx, uint16_t fixed_rgb15)
{
   int fr = fixed_rgb15 & 0x1F;
   int fg = (fixed_rgb15 >> 5) & 0x1F;
   int fb = (fixed_rgb15 >> 10) & 0x1F;
   int r = (IPPU.Red[main_idx]   + fr) >> 1;
   int g = (IPPU.Green[main_idx] + fg) >> 1;
   int b = (IPPU.Blue[main_idx]  + fb) >> 1;
   return lookup_nearest(r, g, b);
}

uint8_t colormath_fixed_sub(uint8_t main_idx, uint16_t fixed_rgb15)
{
   int fr = fixed_rgb15 & 0x1F;
   int fg = (fixed_rgb15 >> 5) & 0x1F;
   int fb = (fixed_rgb15 >> 10) & 0x1F;
   int r = IPPU.Red[main_idx]   - fr;
   int g = IPPU.Green[main_idx] - fg;
   int b = IPPU.Blue[main_idx]  - fb;
   return lookup_nearest(r, g, b);
}

uint8_t colormath_fixed_sub_half(uint8_t main_idx, uint16_t fixed_rgb15)
{
   int fr = fixed_rgb15 & 0x1F;
   int fg = (fixed_rgb15 >> 5) & 0x1F;
   int fb = (fixed_rgb15 >> 10) & 0x1F;
   int r = (IPPU.Red[main_idx]   - fr); if (r < 0) r = 0; r >>= 1;
   int g = (IPPU.Green[main_idx] - fg); if (g < 0) g = 0; g >>= 1;
   int b = (IPPU.Blue[main_idx]  - fb); if (b < 0) b = 0; b >>= 1;
   return lookup_nearest(r, g, b);
}
