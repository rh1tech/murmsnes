/*
 * Audio optimization functions
 * Step-by-step optimization approach
 */

#ifndef AUDIO_OPT_H
#define AUDIO_OPT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Optimized audio packing with gain and limiting
 * Step 1: C implementation for validation
 * Step 3: Assembly implementation for maximum performance
 * 
 * @param dst Output packed stereo (left<<16 | right)
 * @param src Input int16_t stereo samples
 * @param count Number of stereo frames
 * @param gain_num Gain numerator
 * @param gain_den Gain denominator
 * @param use_soft_limit Use soft limiter vs hard clipping
 */
#ifdef PICO_ON_DEVICE
void audio_pack_asm(uint32_t* dst, const int16_t* src, uint32_t count,
                    int gain_num, int gain_den, bool use_soft_limit);
#define audio_pack_opt audio_pack_asm
#else
void audio_pack_opt(uint32_t* dst, const int16_t* src, uint32_t count,
                    int gain_num, int gain_den, bool use_soft_limit);
#endif

/**
 * Optimized audio mixing for no-echo case
 * Step 2: C implementation matching S9xMixSamples no-echo path
 * 
 * @param buffer Output int16_t samples
 * @param sample_count Number of samples (stereo interleaved)
 * @param MixBuffer Input int32_t mix buffer
 * @param master_volume Two-element array [left_vol, right_vol]
 */
void audio_mix_noecho_opt(int16_t* buffer, int32_t sample_count,
                           const int32_t* MixBuffer, const int16_t* master_volume);

/**
 * Pack mono audio to stereo I2S format
 * Duplicates each mono sample to both L and R channels
 * 
 * @param dst Output packed stereo (mono<<16 | mono)
 * @param src Input int16_t mono samples
 * @param count Number of mono samples
 * @param gain_num Gain numerator
 * @param gain_den Gain denominator
 * @param use_soft_limit Use soft limiter vs hard clipping
 */
void audio_pack_mono_to_stereo(uint32_t* dst, const int16_t* src, uint32_t count,
                                int gain_num, int gain_den, bool use_soft_limit);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_OPT_H
