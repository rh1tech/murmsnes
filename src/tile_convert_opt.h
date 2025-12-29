/*
 * Tile conversion optimization header
 * Assembly-accelerated and cached tile rendering
 */

#ifndef TILE_CONVERT_OPT_H
#define TILE_CONVERT_OPT_H

#include <stdint.h>

/* Initialize assembly tile converter lookup tables */
void init_tile_convert_asm(void);

/* Initialize tile cache system */
void init_tile_cache(void);

/* Invalidate tile cache for a VRAM address range */
void tile_cache_invalidate_range(uint32_t start_addr, uint32_t end_addr);

/* Cached tile conversion - use this instead of ConvertTile */
uint8_t ConvertTile_cached(uint8_t* pCache, uint32_t TileAddr);

/* Get tile cache performance statistics */
void tile_cache_get_stats(uint32_t* hits, uint32_t* misses, float* hit_rate);

/* Reset tile cache statistics */
void tile_cache_reset_stats(void);

#endif /* TILE_CONVERT_OPT_H */
