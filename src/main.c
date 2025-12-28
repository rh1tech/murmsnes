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

//=============================================================================
// Configuration
//=============================================================================

#define SCREEN_WIDTH     SNES_WIDTH           // 256
#define SCREEN_HEIGHT    SNES_HEIGHT_EXTENDED // 239

#define AUDIO_SAMPLE_RATE   (24000)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)

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

// Double buffer for audio - Core 0 writes to one, Core 1 reads from other
// Aligned to 32 bytes to avoid cache line sharing
static int16_t __attribute__((aligned(32))) audio_mix_buffer[2][AUDIO_BUFFER_LENGTH * 2];
volatile uint32_t audio_write_idx = 0;  // Which buffer Core 0 is writing to
volatile uint32_t audio_read_idx = 1;   // Which buffer Core 1 is reading from
volatile bool audio_buffer_ready = false;  // Set by Core 0 after mixing

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
    Settings.DisableSoundEcho = false;  // Echo is important for music!
    Settings.InterpolatedSound = true;
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
    i2s_config.dma_trans_count = AUDIO_SAMPLE_RATE / 60;
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
    while (true) {
        // Get buffer index and play the ENTIRE buffer
        uint32_t buf_to_play = audio_read_idx;
        int16_t *samples16 = (int16_t *)audio_mix_buffer[buf_to_play];
        
        for (int i = 0; i < AUDIO_BUFFER_LENGTH; i++) {
            // Direct output - no volume modification
            int16_t left = samples16[i * 2];
            int16_t right = samples16[i * 2 + 1];
            uint32_t sample32 = ((uint32_t)(uint16_t)right << 16) | (uint16_t)left;
            pio_sm_put_blocking(i2s_config.pio, i2s_config.sm, sample32);
        }
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
#define MAX_FRAME_SKIP 2  // Maximum consecutive frames to skip

static void __time_critical_func(emulation_loop)(void) {
    LOG("Starting emulation loop...\n");
    
    uint32_t last_frame_time = time_us_32();
    uint32_t frames_skipped = 0;
    
    while (true) {
        uint32_t current_time = time_us_32();
        uint32_t elapsed = current_time - last_frame_time;
        
        // Auto frame skip: if we're behind schedule, skip rendering
        // but always render at least every MAX_FRAME_SKIP+1 frames
        bool skip_render = (elapsed < TARGET_FRAME_US) && (frames_skipped < MAX_FRAME_SKIP);
        
        // Force render if too many frames skipped
        if (frames_skipped >= MAX_FRAME_SKIP) {
            skip_render = false;
        }
        
        IPPU.RenderThisFrame = !skip_render;
        
        // Run one SNES frame
        S9xMainLoop();
        
        // Mix audio on Core 0 into write buffer
        S9xMixSamples((void *)audio_mix_buffer[audio_write_idx], AUDIO_BUFFER_LENGTH * 2);
        
        // Memory barrier to ensure all writes are visible
        __dmb();
        
        // Swap buffers
        uint32_t old_write = audio_write_idx;
        audio_write_idx = audio_read_idx;
        audio_read_idx = old_write;
        
        // Signal audio is ready to play
        audio_buffer_ready = true;
        __dmb();
        
        if (skip_render) {
            frames_skipped++;
        } else {
            frames_skipped = 0;
            last_frame_time = time_us_32();
            
            // Swap display buffers only when we rendered
            current_buffer = !current_buffer;
            GFX.SubScreen = GFX.Screen = (uint8_t *)SCREEN[current_buffer];
        }
        
        // Update palette if brightness changed during frame
        if (g_palette_needs_update) {
            S9xFixColourBrightness();
            g_palette_needs_update = false;
        }
        
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
