/**
 * MIT License
 * Copyright (c) 2022 Vincent Mistler
 */

// Map our pin defines to what pico-snes-master expects
#ifndef AUDIO_DATA_PIN
#define AUDIO_DATA_PIN PICO_AUDIO_I2S_DATA_PIN
#endif
#ifndef AUDIO_CLOCK_PIN
#define AUDIO_CLOCK_PIN PICO_AUDIO_I2S_CLOCK_PIN_BASE
#endif

#include "audio.h"

#include "pico/audio.h"
#include "pico/audio_i2s.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include <stdio.h>
#include <string.h>

// Minimal shim that routes the legacy i2s_* API to the murmdom pico_audio_i2s
// buffer-pool pipeline. We treat each frame as a packed stereo 32-bit word.

static audio_buffer_pool_t *producer_pool;
static audio_format_t audio_format;
static audio_buffer_format_t producer_format;
static bool audio_i2s_started;

i2s_config_t i2s_get_default_config(void) {
    i2s_config_t i2s_config = {
        .sample_freq = 44100,
        .channel_count = 2,
        .data_pin = AUDIO_DATA_PIN,
        .clock_pin_base = AUDIO_CLOCK_PIN,
        .pio = pio1,
        .sm = 0xFF, // sentinel meaning auto-pick
        .dma_channel = 0,
        .dma_buf = NULL,
        .dma_trans_count = 0,
        .volume = 0,
    };
    return i2s_config;
}

void i2s_init(i2s_config_t *i2s_config) {
    // Pick unused DMA channel/SM without claiming ahead of pico_audio_i2s
    if (i2s_config->dma_channel == 0) {
        uint ch = dma_claim_unused_channel(true);
        dma_channel_unclaim(ch);
        i2s_config->dma_channel = (uint8_t)ch;
    }
    if (i2s_config->sm == 0xFF) {
        int sm = pio_claim_unused_sm(i2s_config->pio, true);
        pio_sm_unclaim(i2s_config->pio, sm);
        i2s_config->sm = (uint8_t)sm;
    }

    audio_i2s_started = false;

    audio_format.sample_freq = i2s_config->sample_freq;
    audio_format.format = AUDIO_BUFFER_FORMAT_PCM_S16;
    audio_format.channel_count = i2s_config->channel_count;

    producer_format.format = &audio_format;
    producer_format.sample_stride = i2s_config->channel_count * sizeof(int16_t);

    // Allocate a small buffer pool sized to the emulator's frame chunk
    producer_pool = audio_new_producer_pool(&producer_format, 3, i2s_config->dma_trans_count);

    audio_i2s_config_t cfg = {
        .data_pin = i2s_config->data_pin,
        .clock_pin_base = i2s_config->clock_pin_base,
        .dma_channel = (uint8_t)i2s_config->dma_channel,
        .pio_sm = (uint8_t)i2s_config->sm,
    };

    printf("audio shim: dma_ch=%u sm=%u\n", (unsigned)i2s_config->dma_channel, (unsigned)i2s_config->sm);
    audio_i2s_setup(&audio_format, &cfg);
    printf("audio shim: setup ok\n");
    audio_i2s_connect_extra(producer_pool, false, 3, i2s_config->dma_trans_count, NULL);
    printf("audio shim: connect ok (will enable on first write)\n");
}

void i2s_write(const i2s_config_t *i2s_config, const int16_t *samples, const size_t len) {
    (void)i2s_config;
    (void)len;
    // Legacy blocking path is not used; reuse DMA path for compatibility
    i2s_dma_write((i2s_config_t *)i2s_config, samples);
}

void i2s_dma_write(i2s_config_t *i2s_config, const int16_t *samples) {
    if (!producer_pool) return;
    audio_buffer_t *ab = take_audio_buffer(producer_pool, true);
    if (!ab) return;

    // Apply simple right-shift volume if requested; input is packed stereo 32-bit words
    uint32_t *dst = (uint32_t *)ab->buffer->bytes;
    const uint32_t *src = (const uint32_t *)samples;
    const uint shift = i2s_config->volume;
    const uint32_t frames = i2s_config->dma_trans_count;

    if (shift == 0) {
        memcpy(dst, src, frames * sizeof(uint32_t));
    } else {
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t v = src[i];
            int16_t l = (int16_t)(v >> 16);
            int16_t r = (int16_t)(v & 0xFFFF);
            l >>= shift;
            r >>= shift;
            dst[i] = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
        }
    }

    ab->sample_count = frames;
    give_audio_buffer(producer_pool, ab);

    if (!audio_i2s_started) {
        audio_i2s_set_enabled(true);
        audio_i2s_started = true;
    }
}

void i2s_dma_write_direct(i2s_config_t *i2s_config, const uint32_t *samples) {
    i2s_dma_write(i2s_config, (const int16_t *)samples);
}

void i2s_volume(i2s_config_t *i2s_config, uint8_t volume) {
    if (volume > 16) volume = 16;
    i2s_config->volume = volume;
}

void i2s_increase_volume(i2s_config_t *i2s_config) {
    if (i2s_config->volume > 0) {
        i2s_config->volume--;
    }
}

void i2s_decrease_volume(i2s_config_t *i2s_config) {
    if (i2s_config->volume < 16) {
        i2s_config->volume++;
    }
}
