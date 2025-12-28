#pragma once

#ifdef MURMSNES_PROFILE
#include <stdint.h>

// These helpers are intentionally tiny and lock-free.
// They run only on Core 0 from the emulation thread.

void murmsnes_prof_reset_window(void);

void murmsnes_prof_add_update_screen_us(uint32_t delta_us);
void murmsnes_prof_add_render_screen_us(uint32_t delta_us);

// RenderScreen sub-phases (measured inside RenderScreen)
void murmsnes_prof_add_rs_obj_us(uint32_t delta_us);
void murmsnes_prof_add_rs_bg0_us(uint32_t delta_us);
void murmsnes_prof_add_rs_bg1_us(uint32_t delta_us);
void murmsnes_prof_add_rs_bg2_us(uint32_t delta_us);
void murmsnes_prof_add_rs_bg3_us(uint32_t delta_us);
void murmsnes_prof_add_rs_mode7_us(uint32_t delta_us);

// UpdateScreen sub-phases (measured inside S9xUpdateScreen)
void murmsnes_prof_add_upd_zclear_us(uint32_t delta_us);
void murmsnes_prof_add_upd_render_sub_us(uint32_t delta_us);
void murmsnes_prof_add_upd_render_main_us(uint32_t delta_us);
void murmsnes_prof_add_upd_colormath_us(uint32_t delta_us);
void murmsnes_prof_add_upd_backdrop_us(uint32_t delta_us);
void murmsnes_prof_add_upd_scale_us(uint32_t delta_us);

// Tile-cache conversions (count-only; ConvertTile calls)
void murmsnes_prof_inc_tile_convert(void);

// Fetch-and-reset counters for the last window.
void murmsnes_prof_take_update_screen(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_render_screen(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);

void murmsnes_prof_take_rs_obj(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_rs_bg0(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_rs_bg1(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_rs_bg2(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_rs_bg3(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_rs_mode7(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);

void murmsnes_prof_take_upd_zclear(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_upd_render_sub(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_upd_render_main(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_upd_colormath(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_upd_backdrop(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);
void murmsnes_prof_take_upd_scale(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);

void murmsnes_prof_take_tile_convert(uint64_t *sum_us, uint32_t *max_us, uint32_t *count);

#endif
