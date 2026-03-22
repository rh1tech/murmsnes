/*
 * Color math for palette-indexed transparency
 *
 * Since the framebuffer uses 8-bit palette indices (not RGB565),
 * transparency blending requires: looking up both colors from CGDATA,
 * blending in 15-bit RGB space, then finding the nearest palette entry.
 *
 * A coarse 8x8x8 grid (512 entries) maps quantized RGB -> nearest palette index.
 * Rebuild the grid whenever the palette changes (S9xFixColourBrightness).
 */
#ifndef COLORMATH_H
#define COLORMATH_H

#include <stdint.h>
#include <stdbool.h>

/* Set true when palette changes; checked before rendering transparency */
extern bool colormath_dirty;

/* Rebuild the nearest-color grid from current palette (IPPU.Red/Green/Blue) */
void colormath_rebuild(void);

/* Blend two palette indices, return nearest palette index to the result.
 * Reads brightness-adjusted colors from IPPU.Red/Green/Blue. */
uint8_t colormath_add(uint8_t main_idx, uint8_t sub_idx);
uint8_t colormath_add_half(uint8_t main_idx, uint8_t sub_idx);
uint8_t colormath_sub(uint8_t main_idx, uint8_t sub_idx);
uint8_t colormath_sub_half(uint8_t main_idx, uint8_t sub_idx);

/* Blend a palette index with a fixed 15-bit color (brightness-adjusted) */
uint8_t colormath_fixed_add(uint8_t main_idx, uint16_t fixed_rgb15);
uint8_t colormath_fixed_add_half(uint8_t main_idx, uint16_t fixed_rgb15);
uint8_t colormath_fixed_sub(uint8_t main_idx, uint16_t fixed_rgb15);
uint8_t colormath_fixed_sub_half(uint8_t main_idx, uint16_t fixed_rgb15);

/* Find nearest palette index for a 15-bit RGB value */
uint8_t colormath_nearest(uint16_t rgb15);

#endif /* COLORMATH_H */
