/*
 * murmsnes - SNES Emulator for RP2350
 * Based on Snes9x and pico-snes
 */
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "board_config.h"
#include "HDMI.h"
#include "psram_init.h"
#include "psram_allocator.h"
#include "ff.h"

// Snes9x includes
#include "snes9x/snes9x.h"
#include "snes9x/soundux.h"
#include "snes9x/memmap.h"
#include "snes9x/apu.h"
#include "snes9x/display.h"
#include "snes9x/gfx.h"
#include "snes9x/cpuexec.h"
#include "snes9x/srtc.h"

// Audio driver (exact copy from pico-snes-master)
#include "audio.h"

// Audio optimizations
#include "audio_opt.h"

#ifdef MURMSNES_PROFILE
#include "murmsnes_profile.h"
#endif

//=============================================================================
// Configuration
//=============================================================================

#define SCREEN_WIDTH     SNES_WIDTH           // 256
#define SCREEN_HEIGHT    SNES_HEIGHT_EXTENDED // 239

// Audio sample rate
#define AUDIO_SAMPLE_RATE   (18000)
// Audio chunk size must match output rate: 60 chunks/sec
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60)

//=============================================================================
// Screen Buffers
//=============================================================================

// Screen buffers - 256x239 16-bit (low byte is palette index)
uint16_t __attribute__((aligned(4))) SCREEN[2][SNES_WIDTH * SNES_HEIGHT_EXTENDED];
static uint8_t __attribute__((aligned(4))) ZBuffer[SNES_WIDTH * SNES_HEIGHT_EXTENDED];
static uint8_t __attribute__((aligned(4))) SubZBuffer[SNES_WIDTH * SNES_HEIGHT_EXTENDED];

// Current display buffer (double buffering) - accessed by HDMI driver
volatile uint32_t current_buffer = 0;

//=============================================================================
// Audio - Core 0 mixes into buffer, Core 1 plays
//=============================================================================

// Audio handoff: Core 0 mixes int16 stereo, then applies gain/limiting and packs
// into 32-bit stereo frames for I2S (L in high 16, R in low 16). Core 1 then
// streams these packed frames to pico_audio_i2s.
//
// Key goal: keep Core 1 work minimal so HDMI activity doesn't starve audio.
// Deeper queue for better buffering during CPU spikes (digital sound effects in MK3)
// 80 frames (~1.33s @ 18kHz) - balance between buffering and SRAM usage
#define AUDIO_QUEUE_DEPTH 80
// NOTE: With fixed 60Hz emulation producing exactly one audio chunk per frame,
// the producer cannot stay "ahead" of the consumer by >1 chunk in steady state.
// Using queue-fill watermarks to decide frame skipping will therefore
// permanently starve video (e.g. render ~12fps with MAX_FRAME_SKIP=4).
// We keep this watermark only for choosing the cheaper limiter path.
#define AUDIO_LOW_WATERMARK 4
static uint32_t __attribute__((aligned(32))) audio_packed_buffer[AUDIO_QUEUE_DEPTH][AUDIO_BUFFER_LENGTH];
static uint32_t __attribute__((aligned(32))) audio_packed_discard[AUDIO_BUFFER_LENGTH];
static volatile uint32_t audio_prod_seq = 0; // total chunks produced
static volatile uint32_t audio_cons_seq = 0; // total chunks consumed

//=============================================================================
// Sync flags
//=============================================================================
static volatile bool core1_ready = false;

//=============================================================================
// FatFS
//=============================================================================
static FATFS fs;

//=============================================================================
// Flash timing configuration for overclocking
//=============================================================================
#define FLASH_MAX_FREQ_MHZ 88

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = FLASH_MAX_FREQ_MHZ * 1000000;
    
    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) {
        divisor = 2;
    }
    
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
        rxdelay += 1;
    }
    
    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

//=============================================================================
// Logging
//=============================================================================
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)

#ifdef MURMSNES_PROFILE
typedef struct {
    uint32_t last_report_us;
    uint32_t frames;
    uint32_t rendered;
    uint32_t skipped;

    uint64_t sum_emul_us;
    uint64_t sum_emul_render_us;
    uint64_t sum_emul_skip_us;
    uint64_t sum_mix_us;
    uint64_t sum_pack_us;

    uint32_t frames_render;
    uint32_t frames_skip;

    uint32_t max_emul_us;
    uint32_t max_emul_render_us;
    uint32_t max_emul_skip_us;
    uint32_t max_mix_us;
    uint32_t max_pack_us;

    int32_t max_late_us;
    uint32_t min_q_fill;
    uint32_t max_q_fill;
} perf_stats_t;

static perf_stats_t g_perf;

static inline void perf_reset_window(uint32_t now_us) {
    g_perf.last_report_us = now_us;
    g_perf.frames = 0;
    g_perf.rendered = 0;
    g_perf.skipped = 0;
    g_perf.sum_emul_us = 0;
    g_perf.sum_emul_render_us = 0;
    g_perf.sum_emul_skip_us = 0;
    g_perf.sum_mix_us = 0;
    g_perf.sum_pack_us = 0;
    g_perf.frames_render = 0;
    g_perf.frames_skip = 0;
    g_perf.max_emul_us = 0;
    g_perf.max_emul_render_us = 0;
    g_perf.max_emul_skip_us = 0;
    g_perf.max_mix_us = 0;
    g_perf.max_pack_us = 0;
    g_perf.max_late_us = 0;
    g_perf.min_q_fill = 0xFFFFFFFFu;
    g_perf.max_q_fill = 0;

    murmsnes_prof_reset_window();
}

static inline void perf_max_u32(uint32_t *dst, uint32_t v) {
    if (v > *dst) *dst = v;
}

static inline void perf_min_u32(uint32_t *dst, uint32_t v) {
    if (v < *dst) *dst = v;
}
#endif

//=============================================================================
// Snes9x Display Interface Implementation
//=============================================================================

bool S9xInitDisplay(void) {
    GFX.Pitch = SNES_WIDTH * sizeof(uint16_t);
    GFX.ZPitch = SNES_WIDTH;
    GFX.SubScreen = GFX.Screen = (uint8_t *)SCREEN[current_buffer];
    GFX.ZBuffer = (uint8_t *)ZBuffer;
    GFX.SubZBuffer = (uint8_t *)SubZBuffer;
    return true;
}

void S9xDeinitDisplay(void) {
}

uint32_t S9xReadJoypad(const int32_t port) {
    if (port != 0)
        return 0;
    
    // TODO: Implement gamepad reading
    uint32_t joypad = 0;
    return joypad;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons) {
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons) {
    return false;
}

bool JustifierOffscreen(void) {
    return true;
}

void JustifierButtons(uint32_t *justifiers) {
    (void)justifiers;
}

//=============================================================================
// Snes9x Initialization
//=============================================================================

static inline void snes9x_init(void) {
    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
    Settings.DisableSoundEcho = true;   // Disable echo to match user request / reduce mixing complexity
    Settings.InterpolatedSound = false; // Disable for performance - reduces mixing CPU cost
    // Keep APU enabled - some games need it even without audio output

    S9xInitDisplay();
    S9xInitMemory();
    S9xInitAPU();
    S9xInitSound(0, 0);
    S9xInitGFX();
    S9xSetPlaybackRate(Settings.SoundPlaybackRate);
    IPPU.RenderThisFrame = 1;
}

//=============================================================================
// ROM Loading from SD Card
//=============================================================================

static bool load_rom_from_sd(const char *filename) {
    FIL file;
    UINT bytes_read;
    
    LOG("Opening ROM: %s\n", filename);
    
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK) {
        LOG("Failed to open ROM file: %d\n", res);
        return false;
    }
    
    FSIZE_t file_size = f_size(&file);
    LOG("ROM size: %lu bytes\n", (unsigned long)file_size);
    
    // Maximum ROM size for SNES (6 MB for largest commercial games)
    const size_t MAX_ROM_SIZE = 6 * 1024 * 1024;
    
    if (file_size > MAX_ROM_SIZE) {
        LOG("ROM too large! Max: %lu bytes\n", (unsigned long)MAX_ROM_SIZE);
        f_close(&file);
        return false;
    }
    
    // Allocate ROM buffer in PSRAM
    size_t alloc_size = (file_size + 0xFFFF) & ~0xFFFF;  // Round up to 64KB boundary
    Memory.ROM = (uint8_t *)psram_malloc(alloc_size + 0x10200);  // Extra for mapping
    if (Memory.ROM == NULL) {
        LOG("Failed to allocate ROM buffer (%lu bytes)!\n", (unsigned long)alloc_size);
        f_close(&file);
        return false;
    }
    LOG("Allocated %lu bytes for ROM in PSRAM\n", (unsigned long)(alloc_size + 0x10200));
    
    Memory.ROM_AllocSize = file_size;
    
    // Read ROM into buffer
    res = f_read(&file, Memory.ROM, file_size, &bytes_read);
    f_close(&file);
    
    if (res != FR_OK || bytes_read != file_size) {
        LOG("Failed to read ROM: res=%d, read=%lu\n", res, (unsigned long)bytes_read);
        return false;
    }
    
    LOG("ROM loaded: %lu bytes\n", (unsigned long)bytes_read);
    return true;
}

//=============================================================================
// Render Core (Core 1) - HDMI & Audio output (exact copy from pico-snes-master)
//=============================================================================

// Test tone buffer in SRAM (not PSRAM)
static int16_t __attribute__((aligned(4))) test_tone[512];

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Gentle soft limiter to keep boosted peaks from hard-clipping
static inline int16_t soft_limit16(int32_t v) {
    const int32_t knee = 30000; // start soft limiting near full scale
    if (v > knee) {
        v = knee + (v - knee) / 4;
    } else if (v < -knee) {
        v = -knee + (v + knee) / 4;
    }
    return clamp16(v);
}

void __time_critical_func(render_core)(void) {
    // Pre-generate test tone - 440Hz square wave
    for (int i = 0; i < 256; i++) {
        int16_t sample = ((i / 25) & 1) ? 8000 : -8000;
        test_tone[i * 2] = sample;      // Left
        test_tone[i * 2 + 1] = sample;  // Right
    }
    
    // Initialize audio
    static i2s_config_t i2s_config;
    i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_BUFFER_LENGTH;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);
    
    // Initialize HDMI AFTER audio
    graphics_init(g_out_HDMI);
    graphics_set_buffer((uint8_t *)SCREEN[0]);
    graphics_set_res(SCREEN_WIDTH, SCREEN_HEIGHT);
    graphics_set_shift(32, 0);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    
    // Signal ready with memory barrier
    __dmb();
    core1_ready = true;
    __dmb();
    
    // Audio playback - continuously play from read buffer
    static uint32_t __attribute__((aligned(32))) replay_buf[AUDIO_BUFFER_LENGTH];
    static bool replay_is_silence = true;
    const uint32_t fade_frames = 128;
    uint32_t last_displayed_buffer = 0;
    while (true) {
        // Update HDMI buffer pointer if Core 0 swapped buffers (double buffering sync)
        uint32_t current_buf = current_buffer;
        if (current_buf != last_displayed_buffer) {
            graphics_set_buffer((uint8_t *)SCREEN[current_buf]);
            last_displayed_buffer = current_buf;
        }
        
        // Consume next mixed chunk if available; otherwise replay the last chunk.
        uint32_t prod = audio_prod_seq;
        uint32_t cons = audio_cons_seq;
        if (prod != cons) {
            uint32_t idx = cons % AUDIO_QUEUE_DEPTH;
            // Ensure we see the producer's writes before copying.
            __dmb();
            memcpy(replay_buf, audio_packed_buffer[idx], sizeof(replay_buf));
            __dmb();
            audio_cons_seq = cons + 1;

            // If we were in underrun-silence mode, fade in the first few samples
            // to avoid a click when audio resumes.
            if (replay_is_silence) {
                uint32_t n = AUDIO_BUFFER_LENGTH;
                uint32_t f = fade_frames;
                if (f > n) f = n;
                for (uint32_t i = 0; i < f; i++) {
                    int32_t g = (int32_t)i; // 0 .. f-1
                    uint32_t v = replay_buf[i];
                    int16_t l = (int16_t)(v >> 16);
                    int16_t r = (int16_t)(v & 0xFFFF);
                    l = (int16_t)(((int32_t)l * g) / (int32_t)f);
                    r = (int16_t)(((int32_t)r * g) / (int32_t)f);
                    replay_buf[i] = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
                }
                replay_is_silence = false;
            }
        } else {
            // Underrun: do NOT repeat old audio (can sound like slowdown).
            // Instead, fade out to silence (once) and then output silence.
            if (!replay_is_silence) {
                uint32_t n = AUDIO_BUFFER_LENGTH;
                uint32_t f = fade_frames;
                if (f > n) f = n;
                for (uint32_t i = 0; i < f; i++) {
                    int32_t g = (int32_t)(f - 1 - i); // f-1 .. 0
                    uint32_t v = replay_buf[i];
                    int16_t l = (int16_t)(v >> 16);
                    int16_t r = (int16_t)(v & 0xFFFF);
                    l = (int16_t)(((int32_t)l * g) / (int32_t)f);
                    r = (int16_t)(((int32_t)r * g) / (int32_t)f);
                    replay_buf[i] = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
                }
                memset(&replay_buf[f], 0, (AUDIO_BUFFER_LENGTH - f) * sizeof(replay_buf[0]));
                replay_is_silence = true;
            } else {
                memset(replay_buf, 0, sizeof(replay_buf));
            }
        }

        // Stream packed stereo frames directly to I2S.
        i2s_dma_write(&i2s_config, (const int16_t *)replay_buf);
    }
}

//=============================================================================
// Main Emulation Loop
//=============================================================================

// Deferred palette update flag from PPU
extern volatile bool g_palette_needs_update;
extern void S9xFixColourBrightness(void);

// Auto frame skip - target ~60fps (16.67ms per frame)
#define TARGET_FRAME_US 16667

//=============================================================================
// Constant Frameskip Configuration (from murmgenesis)
//=============================================================================
// Frameskip pattern:
// - Pattern length is in frames
// - Bit i (LSB=frame 0) indicates whether to render that frame (1) or skip (0)
// Configurable via -DFRAMESKIP_LEVEL=N where:
//   0 = render all frames (60 fps target)
//   1 = render 5/6 frames (~50 fps)
//   2 = render 4/6 frames (~40 fps)
//   3 = render 3/6 frames (~30 fps) - DEFAULT
//   4 = render 2/6 frames (~20 fps)
#ifndef FRAMESKIP_LEVEL
#ifdef MURMSNES_FAST_MODE
#define FRAMESKIP_LEVEL 2  // Fast mode: 40fps with reduced quality
#else
#define FRAMESKIP_LEVEL 3  // Normal mode: 30fps with full quality
#endif
#endif

// Frameskip patterns: [len, mask] for each level
static const uint8_t frameskip_patterns[5][2] = {
    {1, 0x01},  // 0: none - render every frame (60fps)
    {6, 0x1F},  // 1: low - render frames 0-4, skip frame 5 (~50fps)
    {6, 0x15},  // 2: medium - render frames 0,2,4 (~40fps)
    {6, 0x09},  // 3: high - render frames 0,3 (~30fps) - DEFAULT
    {6, 0x05},  // 4: extreme - render frames 0,2 (~20fps)
};

// Runtime frameskip settings
static uint32_t frameskip_pattern_len = 6;
static uint32_t frameskip_pattern_mask = 0x09;  // Default: level 3 (30fps)

// Set frameskip level at runtime
void set_frameskip_level(uint8_t level) {
    if (level > 4) level = 3;  // Clamp to valid range
    frameskip_pattern_len = frameskip_patterns[level][0];
    frameskip_pattern_mask = frameskip_patterns[level][1];
}

// Safety: always render at least once every N frames even if pattern says skip
#define FRAMESKIP_MAX_CONSECUTIVE 4

// Don't treat tiny overshoots (scheduler jitter) as being "behind".
#define LATE_TOLERANCE_US 1000
// If we fall too far behind, resync the deadline instead of accumulating lateness.
#define LATE_RESYNC_US (TARGET_FRAME_US * 4)

static void __time_critical_func(emulation_loop)(void) {
    LOG("Starting emulation loop...\n");
    LOG("[build] %s %s | TARGET_FRAME_US=%u FRAMESKIP_LEVEL=%u LATE_RESYNC_US=%u\n",
        __DATE__, __TIME__,
        (unsigned)TARGET_FRAME_US,
        (unsigned)FRAMESKIP_LEVEL,
        (unsigned)LATE_RESYNC_US);
#ifdef MURMSNES_PROFILE
    LOG("[perf] enabled\n");
#else
    LOG("[perf] disabled (rebuild with MURMSNES_PROFILE=ON)\n");
#endif

    // Fixed-timestep scheduling: keep emulation/audio running at ~60Hz.
    // If rendering is slow, we skip video frames to catch up rather than slowing audio.
    uint32_t next_frame_deadline = time_us_32() + TARGET_FRAME_US;
    uint32_t frame_num = 0;
    uint32_t consecutive_skipped_frames = 0;

    // Initialize frameskip from compile-time level
    set_frameskip_level(FRAMESKIP_LEVEL);
    static const char* frameskip_level_names[] = {"NONE (60fps)", "LOW (50fps)", "MEDIUM (40fps)", "HIGH (30fps)", "EXTREME (20fps)"};
    LOG("[frameskip] level=%d (%s) pattern_len=%u mask=0x%02X\n",
        FRAMESKIP_LEVEL, frameskip_level_names[FRAMESKIP_LEVEL],
        (unsigned)frameskip_pattern_len, (unsigned)frameskip_pattern_mask);

#ifdef MURMSNES_PROFILE
    perf_reset_window(time_us_32());
#endif

    while (true) {
        uint32_t now = time_us_32();
        int32_t late_us = (int32_t)(now - next_frame_deadline);

        // If we're way behind, drop accumulated lateness and realign.
        // This prevents the loop from going into a long "catch up" phase where
        // it never sleeps and video stays permanently in skip mode.
        if (late_us > (int32_t)LATE_RESYNC_US) {
            next_frame_deadline = now + TARGET_FRAME_US;
            late_us = 0;
            consecutive_skipped_frames = 0;
        }

        // If we're ahead of schedule, wait until it's time for the next emulated frame.
        if (late_us < 0) {
            busy_wait_us_32((uint32_t)(-late_us));
            now = time_us_32();
            late_us = (int32_t)(now - next_frame_deadline);
        }

        // Clamp tiny "late" values caused by wake-up jitter.
        if (late_us > 0 && late_us <= (int32_t)LATE_TOLERANCE_US) {
            late_us = 0;
        }

        // Audio queue depth (before producing this frame's chunk).
        uint32_t q_prod = audio_prod_seq;
        uint32_t q_cons = audio_cons_seq;
        uint32_t q_fill = q_prod - q_cons;

        // Deterministic render/skip pattern (from murmgenesis).
        // Uses constant pattern based on FRAMESKIP_LEVEL setting.
        bool render_this_frame = true;
        const uint32_t pat_idx = (frameskip_pattern_len ? (frame_num % frameskip_pattern_len) : 0u);
        render_this_frame = ((frameskip_pattern_mask >> pat_idx) & 1u) != 0u;

        // Safety: always render at least once every FRAMESKIP_MAX_CONSECUTIVE frames
        if (consecutive_skipped_frames >= FRAMESKIP_MAX_CONSECUTIVE) {
            render_this_frame = true;
        }

        bool skip_render = !render_this_frame;

        IPPU.RenderThisFrame = !skip_render;

        // Run one SNES frame of emulation.
    #ifdef MURMSNES_PROFILE
        uint32_t t0 = time_us_32();
    #endif
        S9xMainLoop();
    #ifdef MURMSNES_PROFILE
        uint32_t t1 = time_us_32();
    #endif

        // Mix audio on Core 0 (always, even when skipping render), then apply
        // gain/limiting and pack to 32-bit stereo frames.
        static int16_t __attribute__((aligned(32))) mix16[AUDIO_BUFFER_LENGTH * 2];
    #ifdef MURMSNES_PROFILE
        uint32_t t2 = time_us_32();
    #endif
    #ifdef MURMSNES_FAST_MODE
        // FAST MODE: Mix mono only (half the samples), then duplicate to stereo in packing
        S9xMixSamplesMono((void *)mix16, AUDIO_BUFFER_LENGTH);
    #else
        S9xMixSamples((void *)mix16, AUDIO_BUFFER_LENGTH * 2);
    #endif
    #ifdef MURMSNES_PROFILE
        uint32_t t3 = time_us_32();
    #endif

        uint32_t prod = audio_prod_seq;
        uint32_t cons = audio_cons_seq;
        bool ring_full = (prod - cons) >= AUDIO_QUEUE_DEPTH;
        uint32_t *dst32 = ring_full ? audio_packed_discard : audio_packed_buffer[prod % AUDIO_QUEUE_DEPTH];

        // Gain factor ~1.6x (8/5) with soft limiter
        const int gain_num = 8;
        const int gain_den = 5;
        // When we're late or audio is low, avoid the more expensive soft limiter.
        const bool use_soft_limiter = (late_us <= (int32_t)LATE_TOLERANCE_US) && (q_fill >= AUDIO_LOW_WATERMARK);
#ifdef MURMSNES_PROFILE
        uint32_t t4 = time_us_32();
#endif
        // Use optimized audio packing
#ifdef MURMSNES_FAST_MODE
        // FAST MODE: Pack mono to stereo (duplicate each sample)
        audio_pack_mono_to_stereo(dst32, mix16, AUDIO_BUFFER_LENGTH, gain_num, gain_den, use_soft_limiter);
#else
        audio_pack_opt(dst32, mix16, AUDIO_BUFFER_LENGTH, gain_num, gain_den, use_soft_limiter);
#endif
#ifdef MURMSNES_PROFILE
        uint32_t t5 = time_us_32();
#endif

        if (!ring_full) {
            // Publish the new chunk after the samples are fully written.
            __dmb();
            audio_prod_seq = prod + 1;
            __dmb();
        }

        if (skip_render) {
            consecutive_skipped_frames++;
        } else {
            consecutive_skipped_frames = 0;

            // Swap display buffers only when we rendered
            current_buffer = !current_buffer;
            GFX.SubScreen = GFX.Screen = (uint8_t *)SCREEN[current_buffer];
        }

        // Update palette if brightness changed during frame
        if (g_palette_needs_update) {
            S9xFixColourBrightness();
            g_palette_needs_update = false;
        }

        // Advance deadline and frame counter for the next emulated frame.
        next_frame_deadline += TARGET_FRAME_US;
        frame_num++;

#ifdef MURMSNES_PROFILE
        // Update stats (keep overhead tiny; print at most once/sec)
        uint32_t now_us = time_us_32();
        uint32_t emul_us = (uint32_t)(t1 - t0);
        g_perf.frames++;
        if (skip_render) g_perf.skipped++; else g_perf.rendered++;

        g_perf.sum_emul_us += emul_us;
        if (skip_render) {
            g_perf.sum_emul_skip_us += emul_us;
            g_perf.frames_skip++;
            perf_max_u32(&g_perf.max_emul_skip_us, emul_us);
        } else {
            g_perf.sum_emul_render_us += emul_us;
            g_perf.frames_render++;
            perf_max_u32(&g_perf.max_emul_render_us, emul_us);
        }
        g_perf.sum_mix_us  += (uint32_t)(t3 - t2);
        g_perf.sum_pack_us += (uint32_t)(t5 - t4);
        perf_max_u32(&g_perf.max_emul_us, emul_us);
        perf_max_u32(&g_perf.max_mix_us,  (uint32_t)(t3 - t2));
        perf_max_u32(&g_perf.max_pack_us, (uint32_t)(t5 - t4));
        if (late_us > g_perf.max_late_us) g_perf.max_late_us = late_us;
        perf_min_u32(&g_perf.min_q_fill, q_fill);
        perf_max_u32(&g_perf.max_q_fill, q_fill);

        if ((uint32_t)(now_us - g_perf.last_report_us) >= 1000000u) {
            uint32_t frames = g_perf.frames ? g_perf.frames : 1;
            uint32_t avg_emul = (uint32_t)(g_perf.sum_emul_us / frames);
            uint32_t fr_r = g_perf.frames_render ? g_perf.frames_render : 1;
            uint32_t fr_s = g_perf.frames_skip ? g_perf.frames_skip : 1;
            uint32_t avg_emul_r = (uint32_t)(g_perf.sum_emul_render_us / fr_r);
            uint32_t avg_emul_s = (uint32_t)(g_perf.sum_emul_skip_us / fr_s);
            uint32_t avg_mix  = (uint32_t)(g_perf.sum_mix_us / frames);
            uint32_t avg_pack = (uint32_t)(g_perf.sum_pack_us / frames);

            uint64_t upd_sum = 0;
            uint32_t upd_max = 0;
            uint32_t upd_cnt = 0;
            murmsnes_prof_take_update_screen(&upd_sum, &upd_max, &upd_cnt);
            uint32_t upd_avg = (upd_cnt ? (uint32_t)(upd_sum / upd_cnt) : 0);

            uint64_t uz_sum = 0;
            uint32_t uz_max = 0;
            uint32_t uz_cnt = 0;
            murmsnes_prof_take_upd_zclear(&uz_sum, &uz_max, &uz_cnt);
            uint32_t uz_avg = (uz_cnt ? (uint32_t)(uz_sum / uz_cnt) : 0);

            uint64_t usub_sum = 0;
            uint32_t usub_max = 0;
            uint32_t usub_cnt = 0;
            murmsnes_prof_take_upd_render_sub(&usub_sum, &usub_max, &usub_cnt);
            uint32_t usub_avg = (usub_cnt ? (uint32_t)(usub_sum / usub_cnt) : 0);

            uint64_t umain_sum = 0;
            uint32_t umain_max = 0;
            uint32_t umain_cnt = 0;
            murmsnes_prof_take_upd_render_main(&umain_sum, &umain_max, &umain_cnt);
            uint32_t umain_avg = (umain_cnt ? (uint32_t)(umain_sum / umain_cnt) : 0);

            uint64_t ucm_sum = 0;
            uint32_t ucm_max = 0;
            uint32_t ucm_cnt = 0;
            murmsnes_prof_take_upd_colormath(&ucm_sum, &ucm_max, &ucm_cnt);
            uint32_t ucm_avg = (ucm_cnt ? (uint32_t)(ucm_sum / ucm_cnt) : 0);

            uint64_t ubd_sum = 0;
            uint32_t ubd_max = 0;
            uint32_t ubd_cnt = 0;
            murmsnes_prof_take_upd_backdrop(&ubd_sum, &ubd_max, &ubd_cnt);
            uint32_t ubd_avg = (ubd_cnt ? (uint32_t)(ubd_sum / ubd_cnt) : 0);

            uint64_t usc_sum = 0;
            uint32_t usc_max = 0;
            uint32_t usc_cnt = 0;
            murmsnes_prof_take_upd_scale(&usc_sum, &usc_max, &usc_cnt);
            uint32_t usc_avg = (usc_cnt ? (uint32_t)(usc_sum / usc_cnt) : 0);

            uint64_t rs_sum = 0;
            uint32_t rs_max = 0;
            uint32_t rs_cnt = 0;
            murmsnes_prof_take_render_screen(&rs_sum, &rs_max, &rs_cnt);
            uint32_t rs_avg = (rs_cnt ? (uint32_t)(rs_sum / rs_cnt) : 0);

            uint64_t ro_sum = 0;
            uint32_t ro_max = 0;
            uint32_t ro_cnt = 0;
            murmsnes_prof_take_rs_obj(&ro_sum, &ro_max, &ro_cnt);
            uint32_t ro_avg = (ro_cnt ? (uint32_t)(ro_sum / ro_cnt) : 0);

            uint64_t r0_sum = 0;
            uint32_t r0_max = 0;
            uint32_t r0_cnt = 0;
            murmsnes_prof_take_rs_bg0(&r0_sum, &r0_max, &r0_cnt);
            uint32_t r0_avg = (r0_cnt ? (uint32_t)(r0_sum / r0_cnt) : 0);

            uint64_t r1_sum = 0;
            uint32_t r1_max = 0;
            uint32_t r1_cnt = 0;
            murmsnes_prof_take_rs_bg1(&r1_sum, &r1_max, &r1_cnt);
            uint32_t r1_avg = (r1_cnt ? (uint32_t)(r1_sum / r1_cnt) : 0);

            uint64_t r2_sum = 0;
            uint32_t r2_max = 0;
            uint32_t r2_cnt = 0;
            murmsnes_prof_take_rs_bg2(&r2_sum, &r2_max, &r2_cnt);
            uint32_t r2_avg = (r2_cnt ? (uint32_t)(r2_sum / r2_cnt) : 0);

            uint64_t r3_sum = 0;
            uint32_t r3_max = 0;
            uint32_t r3_cnt = 0;
            murmsnes_prof_take_rs_bg3(&r3_sum, &r3_max, &r3_cnt);
            uint32_t r3_avg = (r3_cnt ? (uint32_t)(r3_sum / r3_cnt) : 0);

            uint64_t r7_sum = 0;
            uint32_t r7_max = 0;
            uint32_t r7_cnt = 0;
            murmsnes_prof_take_rs_mode7(&r7_sum, &r7_max, &r7_cnt);
            uint32_t r7_avg = (r7_cnt ? (uint32_t)(r7_sum / r7_cnt) : 0);

            uint64_t tc_sum = 0;
            uint32_t tc_max = 0;
            uint32_t tc_cnt = 0;
            murmsnes_prof_take_tile_convert(&tc_sum, &tc_max, &tc_cnt);

            // Snapshot a few PPU regs for correlation (cheap: 1 load each)
            uint8_t ppu_bgm = (uint8_t)PPU.BGMode;
            uint8_t r2106 = Memory.FillRAM[0x2106];
            uint8_t r2107 = Memory.FillRAM[0x2107];
            uint8_t r2108 = Memory.FillRAM[0x2108];
            uint8_t r2109 = Memory.FillRAM[0x2109];
            uint8_t r210a = Memory.FillRAM[0x210a];
            uint8_t r210b = Memory.FillRAM[0x210b];
            uint8_t r210c = Memory.FillRAM[0x210c];

            uint8_t r2123 = Memory.FillRAM[0x2123];
            uint8_t r2124 = Memory.FillRAM[0x2124];
            uint8_t r2125 = Memory.FillRAM[0x2125];
            uint8_t r2126 = Memory.FillRAM[0x2126];
            uint8_t r2127 = Memory.FillRAM[0x2127];
            uint8_t r2128 = Memory.FillRAM[0x2128];
            uint8_t r2129 = Memory.FillRAM[0x2129];
            uint8_t r212a = Memory.FillRAM[0x212a];
            uint8_t r212b = Memory.FillRAM[0x212b];

            uint8_t r212c = Memory.FillRAM[0x212c];
            uint8_t r212d = Memory.FillRAM[0x212d];
            uint8_t r212e = Memory.FillRAM[0x212e];
            uint8_t r212f = Memory.FillRAM[0x212f];
            uint8_t r2130 = Memory.FillRAM[0x2130];
            uint8_t r2131 = Memory.FillRAM[0x2131];
            uint8_t r2133 = Memory.FillRAM[0x2133];

            LOG("[perf] emu_fps=%lu rend_fps=%lu skip_fps=%lu late_max=%ldus qmin=%lu qmax=%lu | tilec=%lu | bgm=%u 2106=%02x 2107=%02x 2108=%02x 2109=%02x 210a=%02x 210b=%02x 210c=%02x | 2123=%02x 2124=%02x 2125=%02x 2126=%02x 2127=%02x 2128=%02x 2129=%02x 212a=%02x 212b=%02x | 212c=%02x 212d=%02x 212e=%02x 212f=%02x 2130=%02x 2131=%02x 2133=%02x | emu avg/max=%lu/%lu us | emuR avg/max=%lu/%lu us | emuS avg/max=%lu/%lu us | mix avg/max=%lu/%lu us | pack avg/max=%lu/%lu us | upd avg/max=%lu/%lu us (%lu) | uz avg/max=%lu/%lu us (%lu) | uSub avg/max=%lu/%lu us (%lu) | uMain avg/max=%lu/%lu us (%lu) | uMath avg/max=%lu/%lu us (%lu) | uBack avg/max=%lu/%lu us (%lu) | uScale avg/max=%lu/%lu us (%lu) | rs avg/max=%lu/%lu us (%lu) | ro avg/max=%lu/%lu us (%lu) | r0 avg/max=%lu/%lu us (%lu) | r1 avg/max=%lu/%lu us (%lu) | r2 avg/max=%lu/%lu us (%lu) | r3 avg/max=%lu/%lu us (%lu) | r7 avg/max=%lu/%lu us (%lu)\n",
                (unsigned long)frames,
                (unsigned long)g_perf.rendered,
                (unsigned long)g_perf.skipped,
                (long)g_perf.max_late_us,
                (unsigned long)g_perf.min_q_fill,
                (unsigned long)g_perf.max_q_fill,
                (unsigned long)tc_cnt,
                (unsigned)ppu_bgm,
                (unsigned)r2106,
                (unsigned)r2107,
                (unsigned)r2108,
                (unsigned)r2109,
                (unsigned)r210a,
                (unsigned)r210b,
                (unsigned)r210c,
                (unsigned)r2123,
                (unsigned)r2124,
                (unsigned)r2125,
                (unsigned)r2126,
                (unsigned)r2127,
                (unsigned)r2128,
                (unsigned)r2129,
                (unsigned)r212a,
                (unsigned)r212b,
                (unsigned)r212c,
                (unsigned)r212d,
                (unsigned)r212e,
                (unsigned)r212f,
                (unsigned)r2130,
                (unsigned)r2131,
                (unsigned)r2133,
                (unsigned long)avg_emul, (unsigned long)g_perf.max_emul_us,
                (unsigned long)avg_emul_r, (unsigned long)g_perf.max_emul_render_us,
                (unsigned long)avg_emul_s, (unsigned long)g_perf.max_emul_skip_us,
                (unsigned long)avg_mix,  (unsigned long)g_perf.max_mix_us,
                (unsigned long)avg_pack, (unsigned long)g_perf.max_pack_us,
                (unsigned long)upd_avg, (unsigned long)upd_max, (unsigned long)upd_cnt,
                (unsigned long)uz_avg, (unsigned long)uz_max, (unsigned long)uz_cnt,
                (unsigned long)usub_avg, (unsigned long)usub_max, (unsigned long)usub_cnt,
                (unsigned long)umain_avg, (unsigned long)umain_max, (unsigned long)umain_cnt,
                (unsigned long)ucm_avg, (unsigned long)ucm_max, (unsigned long)ucm_cnt,
                (unsigned long)ubd_avg, (unsigned long)ubd_max, (unsigned long)ubd_cnt,
                (unsigned long)usc_avg, (unsigned long)usc_max, (unsigned long)usc_cnt,
                (unsigned long)rs_avg, (unsigned long)rs_max, (unsigned long)rs_cnt,
                (unsigned long)ro_avg, (unsigned long)ro_max, (unsigned long)ro_cnt,
                (unsigned long)r0_avg, (unsigned long)r0_max, (unsigned long)r0_cnt,
                (unsigned long)r1_avg, (unsigned long)r1_max, (unsigned long)r1_cnt,
                (unsigned long)r2_avg, (unsigned long)r2_max, (unsigned long)r2_cnt,
                (unsigned long)r3_avg, (unsigned long)r3_max, (unsigned long)r3_cnt,
                (unsigned long)r7_avg, (unsigned long)r7_max, (unsigned long)r7_cnt);
            perf_reset_window(now_us);
        }
#endif

        tight_loop_contents();
    }
}

//=============================================================================
// Main Entry Point
//=============================================================================

int main(void) {
    // Overclock support
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ);
    sleep_ms(100);
#endif
    
    // Set system clock
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        set_sys_clock_khz(252 * 1000, true);
    }
    
    // Initialize stdio (USB serial)
    stdio_init_all();
    
    // Startup delay for USB serial console (4 seconds)
    for (int i = 0; i < 8; i++) {
        sleep_ms(500);
    }
    
    LOG("\n\n");
    LOG("========================================\n");
    LOG("   murmsnes - SNES for RP2350\n");
    LOG("========================================\n");
    LOG("System Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    
    // Initialize LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    
    // Initialize PSRAM
    LOG("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    LOG("PSRAM pin: %u\n", psram_pin);
    psram_init(psram_pin);
    psram_reset();
    LOG("PSRAM initialized (8 MB)\n");
    
    // Mount SD card
    LOG("Mounting SD card...\n");
    FRESULT res = f_mount(&fs, "", 1);
    if (res != FR_OK) {
        LOG("Failed to mount SD card: %d\n", res);
        // Blink LED to indicate error
        while (1) {
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
            sleep_ms(100);
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            sleep_ms(100);
        }
    }
    LOG("SD card mounted\n");
    
    // Launch Core 1 (HDMI + Audio)
    // HDMI DMA IRQ runs on Core 1, freeing Core 0 for emulation
    LOG("Starting render core (HDMI + Audio)...\n");
    multicore_launch_core1(render_core);
    
    // Wait for Core 1 to initialize HDMI and audio
    LOG("[Core0] Waiting for Core 1 to initialize...\n");
    while (!core1_ready) {
        tight_loop_contents();
    }
    LOG("[Core0] Render core started (HDMI + Audio on Core 1)\n");
    
    // Try to load ROM from SD card
    LOG("Loading ROM...\n");
    bool rom_loaded = false;
    
    // Try various paths and extensions
    const char *rom_paths[] = {
        "/snes/test.smc",
        "/snes/test.sfc",
        "/SNES/test.smc",
        "/SNES/test.sfc",
        "/test.smc",
        "/test.sfc",
        NULL
    };
    
    for (const char **path = rom_paths; *path != NULL; path++) {
        if (load_rom_from_sd(*path)) {
            rom_loaded = true;
            break;
        }
    }
    
    if (!rom_loaded) {
        LOG("Could not find ROM file!\n");
        LOG("Please place a ROM at /snes/test.smc or /snes/test.sfc\n");
        // Blink LED slowly to indicate no ROM
        while (1) {
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
            sleep_ms(500);
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            sleep_ms(500);
        }
    }
    
    // Initialize SNES emulator
    LOG("Initializing SNES emulator...\n");
    snes9x_init();
    
    // Load the ROM into SNES memory map
    LOG("Setting up ROM mapping...\n");
    if (!LoadROM(NULL)) {
        LOG("Failed to initialize ROM!\n");
        while (1) {
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
            sleep_ms(200);
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            sleep_ms(200);
        }
    }
    
    LOG("ROM loaded successfully!\n");
    LOG("ROM Name: %s\n", Memory.ROMName);
    LOG("ROM Size: %lu KB\n", (unsigned long)(Memory.CalculatedSize / 1024));
    
    gpio_put(PICO_DEFAULT_LED_PIN, 0);  // LED off = running
    
    // Run emulation
    emulation_loop();
    
    return 0;
}
