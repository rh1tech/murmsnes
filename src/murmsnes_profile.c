#ifdef MURMSNES_PROFILE

#include "murmsnes_profile.h"

typedef enum {
    PROF_UPD_TOTAL = 0,
    PROF_RS_TOTAL,
    PROF_RS_OBJ,
    PROF_RS_BG0,
    PROF_RS_BG1,
    PROF_RS_BG2,
    PROF_RS_BG3,
    PROF_RS_MODE7,
    PROF_UPD_ZCLEAR,
    PROF_UPD_RENDER_SUB,
    PROF_UPD_RENDER_MAIN,
    PROF_UPD_COLORMATH,
    PROF_UPD_BACKDROP,
    PROF_UPD_SCALE,
    PROF_TILE_CONVERT,
    PROF__COUNT
} prof_slot_t;

static volatile uint64_t s_sum_us[PROF__COUNT];
static volatile uint32_t s_max_us[PROF__COUNT];
static volatile uint32_t s_count[PROF__COUNT];

static inline void max_u32(volatile uint32_t *dst, uint32_t v) {
    uint32_t cur = *dst;
    if (v > cur) {
        *dst = v;
    }
}

static inline void prof_add(prof_slot_t slot, uint32_t delta_us) {
    s_sum_us[slot] += delta_us;
    max_u32(&s_max_us[slot], delta_us);
    s_count[slot]++;
}

static inline void prof_take(prof_slot_t slot, uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    if (sum_us) *sum_us = s_sum_us[slot];
    if (max_us) *max_us = s_max_us[slot];
    if (count) *count = s_count[slot];
    s_sum_us[slot] = 0;
    s_max_us[slot] = 0;
    s_count[slot] = 0;
}

static inline void prof_inc(prof_slot_t slot) {
    s_count[slot]++;
}

void murmsnes_prof_reset_window(void) {
    for (int i = 0; i < (int)PROF__COUNT; i++) {
        s_sum_us[i] = 0;
        s_max_us[i] = 0;
        s_count[i] = 0;
    }
}

void murmsnes_prof_add_update_screen_us(uint32_t delta_us) {
    prof_add(PROF_UPD_TOTAL, delta_us);
}

void murmsnes_prof_add_render_screen_us(uint32_t delta_us) {
    prof_add(PROF_RS_TOTAL, delta_us);
}

void murmsnes_prof_add_rs_obj_us(uint32_t delta_us) { prof_add(PROF_RS_OBJ, delta_us); }
void murmsnes_prof_add_rs_bg0_us(uint32_t delta_us) { prof_add(PROF_RS_BG0, delta_us); }
void murmsnes_prof_add_rs_bg1_us(uint32_t delta_us) { prof_add(PROF_RS_BG1, delta_us); }
void murmsnes_prof_add_rs_bg2_us(uint32_t delta_us) { prof_add(PROF_RS_BG2, delta_us); }
void murmsnes_prof_add_rs_bg3_us(uint32_t delta_us) { prof_add(PROF_RS_BG3, delta_us); }
void murmsnes_prof_add_rs_mode7_us(uint32_t delta_us) { prof_add(PROF_RS_MODE7, delta_us); }

void murmsnes_prof_add_upd_zclear_us(uint32_t delta_us) { prof_add(PROF_UPD_ZCLEAR, delta_us); }
void murmsnes_prof_add_upd_render_sub_us(uint32_t delta_us) { prof_add(PROF_UPD_RENDER_SUB, delta_us); }
void murmsnes_prof_add_upd_render_main_us(uint32_t delta_us) { prof_add(PROF_UPD_RENDER_MAIN, delta_us); }
void murmsnes_prof_add_upd_colormath_us(uint32_t delta_us) { prof_add(PROF_UPD_COLORMATH, delta_us); }
void murmsnes_prof_add_upd_backdrop_us(uint32_t delta_us) { prof_add(PROF_UPD_BACKDROP, delta_us); }
void murmsnes_prof_add_upd_scale_us(uint32_t delta_us) { prof_add(PROF_UPD_SCALE, delta_us); }

void murmsnes_prof_inc_tile_convert(void) { prof_inc(PROF_TILE_CONVERT); }

void murmsnes_prof_take_update_screen(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_UPD_TOTAL, sum_us, max_us, count);
}

void murmsnes_prof_take_render_screen(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_RS_TOTAL, sum_us, max_us, count);
}

void murmsnes_prof_take_rs_obj(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_RS_OBJ, sum_us, max_us, count);
}

void murmsnes_prof_take_rs_bg0(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_RS_BG0, sum_us, max_us, count);
}

void murmsnes_prof_take_rs_bg1(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_RS_BG1, sum_us, max_us, count);
}

void murmsnes_prof_take_rs_bg2(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_RS_BG2, sum_us, max_us, count);
}

void murmsnes_prof_take_rs_bg3(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_RS_BG3, sum_us, max_us, count);
}

void murmsnes_prof_take_rs_mode7(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_RS_MODE7, sum_us, max_us, count);
}

void murmsnes_prof_take_upd_zclear(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_UPD_ZCLEAR, sum_us, max_us, count);
}

void murmsnes_prof_take_upd_render_sub(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_UPD_RENDER_SUB, sum_us, max_us, count);
}

void murmsnes_prof_take_upd_render_main(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_UPD_RENDER_MAIN, sum_us, max_us, count);
}

void murmsnes_prof_take_upd_colormath(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_UPD_COLORMATH, sum_us, max_us, count);
}

void murmsnes_prof_take_upd_backdrop(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_UPD_BACKDROP, sum_us, max_us, count);
}

void murmsnes_prof_take_upd_scale(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_UPD_SCALE, sum_us, max_us, count);
}

void murmsnes_prof_take_tile_convert(uint64_t *sum_us, uint32_t *max_us, uint32_t *count) {
    prof_take(PROF_TILE_CONVERT, sum_us, max_us, count);
}

#endif
