/*
 * Assembly-optimized tile conversion wrapper
 * Links C code with optimized assembly implementations
 */

#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "gfx.h"
#include "tile.h"

/* External assembly function */
extern uint8_t ConvertTile_opt_8bpp(uint8_t* pCache, uint32_t TileAddr, uint8_t* vram_base);

/* External C function from tile.c */
extern uint8_t ConvertTile(uint8_t* pCache, uint32_t TileAddr);

/* Lookup tables from tile.c */
extern const uint32_t odd[4][16];
extern const uint32_t even[4][16];

/* Assembly-accessible table pointers */
uint32_t* __attribute__((section(".data"))) odd_table_base = NULL;
uint32_t* __attribute__((section(".data"))) even_table_base = NULL;

/* Initialize assembly tile converter */
void init_tile_convert_asm(void)
{
    /* Set up lookup table pointers for assembly code */
    odd_table_base = (uint32_t*)&odd[0][0];
    even_table_base = (uint32_t*)&even[0][0];
}

/* Wrapper function that decides between C and assembly */
static inline uint8_t ConvertTile_wrapper(uint8_t* pCache, uint32_t TileAddr)
{
#if PICO_ON_DEVICE
    /* Use assembly optimized version for 8bpp mode */
    if (BG.BitShift == 8) {
        return ConvertTile_opt_8bpp(pCache, TileAddr, Memory.VRAM);
    }
#endif
    
    /* Fall back to C version for 4bpp/2bpp or non-Pico platforms */
    return ConvertTile(pCache, TileAddr);
}

/* 
 * Tile cache implementation to reduce redundant conversions
 * 
 * The performance log shows ~60,000 tile conversions per frame.
 * Most tiles don't change between frames, so caching is highly effective.
 */

#define TILE_CACHE_ENTRIES 2048
#define TILE_CACHE_MASK    (TILE_CACHE_ENTRIES - 1)

typedef struct {
    uint32_t vram_addr;        /* VRAM address key */
    uint32_t vram_gen;         /* Generation counter */
    uint8_t  flags;            /* Tile flags (opaque, depth, etc.) */
    uint8_t  cached_tile[64];  /* 8x8 pixels */
} __attribute__((aligned(32))) TileCacheEntry;

static TileCacheEntry tile_cache[TILE_CACHE_ENTRIES];
static uint32_t tile_cache_generation = 0;

/* Statistics */
static uint32_t tile_cache_hits = 0;
static uint32_t tile_cache_misses = 0;

/* Initialize tile cache */
void init_tile_cache(void)
{
    for (int i = 0; i < TILE_CACHE_ENTRIES; i++) {
        tile_cache[i].vram_addr = 0xFFFFFFFF;
        tile_cache[i].vram_gen = 0;
    }
    tile_cache_generation = 1;
    tile_cache_hits = 0;
    tile_cache_misses = 0;
}

/* Invalidate tile cache on VRAM write */
void tile_cache_invalidate_range(uint32_t start_addr, uint32_t end_addr)
{
    /* Simple approach: increment generation counter */
    /* This effectively invalidates all cached tiles */
    tile_cache_generation++;
    
    if (tile_cache_generation == 0) {
        /* Wrapped around, do full invalidation */
        init_tile_cache();
    }
}

/* Cached tile conversion with lookup */
uint8_t ConvertTile_cached(uint8_t* pCache, uint32_t TileAddr)
{
    /* Calculate cache index */
    uint32_t hash = (TileAddr >> 6) & TILE_CACHE_MASK;
    TileCacheEntry* entry = &tile_cache[hash];
    
    /* Check if cache hit */
    if (__builtin_expect(entry->vram_addr == TileAddr && 
                         entry->vram_gen == tile_cache_generation, 1)) {
        /* Cache hit - copy cached tile */
        tile_cache_hits++;
        __builtin_memcpy(pCache, entry->cached_tile, 64);
        return entry->flags;
    }
    
    /* Cache miss - convert and store */
    tile_cache_misses++;
    uint8_t flags = ConvertTile_wrapper(pCache, TileAddr);
    
    /* Store in cache */
    entry->vram_addr = TileAddr;
    entry->vram_gen = tile_cache_generation;
    entry->flags = flags;
    __builtin_memcpy(entry->cached_tile, pCache, 64);
    
    return flags;
}

/* Get cache statistics */
void tile_cache_get_stats(uint32_t* hits, uint32_t* misses, float* hit_rate)
{
    *hits = tile_cache_hits;
    *misses = tile_cache_misses;
    uint32_t total = tile_cache_hits + tile_cache_misses;
    *hit_rate = total > 0 ? ((float)tile_cache_hits / total) * 100.0f : 0.0f;
}

/* Reset cache statistics */
void tile_cache_reset_stats(void)
{
    tile_cache_hits = 0;
    tile_cache_misses = 0;
}
