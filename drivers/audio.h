/*
 * murmsnes - Audio driver for RP2350
 * Uses pico_audio_i2s for I2S output
 * Adapted for SNES emulation (22050 Hz sample rate)
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations to avoid pulling in pico_audio headers
struct audio_buffer_pool;
struct audio_buffer;

// SNES audio sample rate (22050 Hz standard for SNES emulation)
#define AUDIO_SAMPLE_RATE 22050

// Audio buffer size - samples per buffer
// SNES runs at ~60 FPS, so ~368 samples per frame (22050/60)
#define AUDIO_BUFFER_SAMPLES 512

// Initialize audio system
bool audio_init(void);

// Shutdown audio system
void audio_shutdown(void);

// Check if audio is initialized
bool audio_is_initialized(void);

// Update audio - call this to process audio buffers
void audio_update(void);

// Get producer pool for direct buffer access
struct audio_buffer_pool *audio_get_producer_pool(void);

// Fill an I2S buffer directly from sound buffer
void audio_fill_buffer(struct audio_buffer *buffer);

// Set master volume (0-128)
void audio_set_volume(int volume);

// Get current master volume
int audio_get_volume(void);

// Enable/disable audio
void audio_set_enabled(bool enabled);

// Check if audio is enabled
bool audio_is_enabled(void);

#endif // AUDIO_H
