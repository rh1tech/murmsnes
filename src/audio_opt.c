/*
 * Audio optimization functions - Step 1: C implementation
 * This matches the original logic exactly for validation
 */

#include "audio_opt.h"

// From main.c
static inline int16_t clamp16(int32_t v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return v;
}

static inline int16_t soft_limit16(int32_t v) {
    if (v > 30000) {
        v = 30000 + ((v - 30000) >> 2);
    } else if (v < -30000) {
        v = -30000 + ((v + 30000) >> 2);
    }
    return clamp16(v);
}

void audio_pack_opt(uint32_t* dst, const int16_t* src, uint32_t count,
                     int gain_num, int gain_den, bool use_soft_limit)
{
    for (uint32_t i = 0; i < count; i++) {
        int32_t left = (src[i * 2] * gain_num) / gain_den;
        int32_t right = (src[i * 2 + 1] * gain_num) / gain_den;
        
        if (use_soft_limit) {
            left = soft_limit16(left);
            right = soft_limit16(right);
        } else {
            left = clamp16(left);
            right = clamp16(right);
        }
        
        dst[i] = ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
    }
}
