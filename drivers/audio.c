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

#define PWM_PIN0 (AUDIO_PWM_PIN&0xfe)
#define PWM_PIN1 (PWM_PIN0+1)

#include "audio.h"
#include <string.h>
#include <stdlib.h>

#ifdef AUDIO_PWM_PIN
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#endif

// Static DMA buffer in SRAM - avoids PSRAM allocation
// 22050/60 = 367 samples * 2 (stereo) = 734 uint16_t = 1468 bytes
// Allocate extra for safety - regular static array in BSS
static uint16_t audio_dma_buf_static[1024];

/**
 * return the default i2s context used to store information about the setup
 */
i2s_config_t i2s_get_default_config(void) {
    i2s_config_t i2s_config = {
		.sample_freq = 44100, 
		.channel_count = 2,
		.data_pin = AUDIO_DATA_PIN,
		.clock_pin_base = AUDIO_CLOCK_PIN,
		.pio = pio1,
		.sm = 0,
        .dma_channel = 0,
        .dma_buf = NULL,
        .dma_trans_count = 0,
        .volume = 0,
	};

    return i2s_config;
}

/**
 * Initialize the I2S driver. Must be called before calling i2s_write or i2s_dma_write
 * i2s_config: I2S context obtained by i2s_get_default_config()
 */
void i2s_init(i2s_config_t *i2s_config) {

#ifndef AUDIO_PWM_PIN

    uint8_t func=GPIO_FUNC_PIO1;
    gpio_set_function(i2s_config->data_pin, func);
    gpio_set_function(i2s_config->clock_pin_base, func);
    gpio_set_function(i2s_config->clock_pin_base+1, func);
    
    i2s_config->sm = pio_claim_unused_sm(i2s_config->pio, true);

    /* Set PIO clock */
    uint32_t system_clock_frequency = clock_get_hz(clk_sys);
    uint32_t divider = system_clock_frequency * 4 / i2s_config->sample_freq;

#ifdef I2S_CS4334
    uint offset = pio_add_program(i2s_config->pio, &audio_i2s_cs4334_program);
    audio_i2s_cs4334_program_init(i2s_config->pio, i2s_config->sm , offset, i2s_config->data_pin , i2s_config->clock_pin_base);
    divider >>= 3;
#else
    uint offset = pio_add_program(i2s_config->pio, &audio_i2s_program);
    audio_i2s_program_init(i2s_config->pio, i2s_config->sm , offset, i2s_config->data_pin , i2s_config->clock_pin_base);
#endif

    pio_sm_set_clkdiv_int_frac(i2s_config->pio, i2s_config->sm , divider >> 8u, divider & 0xffu);

    pio_sm_set_enabled(i2s_config->pio, i2s_config->sm, true);
#endif

    // For blocking-only mode, skip DMA setup entirely
    // DMA configuration is now optional and won't interfere
}

/**
 * Write samples to I2S directly and wait for completion (blocking)
 */
void i2s_write(const i2s_config_t *i2s_config,const int16_t *samples,const size_t len) {
    for(size_t i=0;i<len;i++) {
            pio_sm_put_blocking(i2s_config->pio, i2s_config->sm, (uint32_t)samples[i]);
    }
}

/**
 * Write samples to DMA buffer and initiate DMA transfer (non blocking)
 */
void i2s_dma_write(i2s_config_t *i2s_config,const int16_t *samples) {
    /* Wait the completion of the previous DMA transfer */
    dma_channel_wait_for_finish_blocking(i2s_config->dma_channel);
    /* Copy samples into the DMA buffer */

#ifdef AUDIO_PWM_PIN
    for(uint16_t i=0;i<i2s_config->dma_trans_count*2;i++) {
            i2s_config->dma_buf[i] = (65536/2+(samples[i]))>>(4+i2s_config->volume);
        }
#else

    if(i2s_config->volume==0) {
        memcpy(i2s_config->dma_buf,samples,i2s_config->dma_trans_count*sizeof(int32_t));
    } else {
        for(uint16_t i=0;i<i2s_config->dma_trans_count*2;i++) {
            i2s_config->dma_buf[i] = samples[i]>>i2s_config->volume;
        }
    }
#endif    

    /* Initiate the DMA transfer */
    dma_channel_transfer_from_buffer_now(i2s_config->dma_channel,
                                         i2s_config->dma_buf,
                                         i2s_config->dma_trans_count);
}

/**
 * Adjust the output volume
 */
void i2s_volume(i2s_config_t *i2s_config,uint8_t volume) {
    if(volume>16) volume=16;
    i2s_config->volume=volume;
}

/**
 * Increases the output volume
 */
void i2s_increase_volume(i2s_config_t *i2s_config) {
    if(i2s_config->volume>0) {
        i2s_config->volume--;
    }
}

/**
 * Decreases the output volume
 */
void i2s_decrease_volume(i2s_config_t *i2s_config) {
    if(i2s_config->volume<16) {
        i2s_config->volume++;
    }
}
