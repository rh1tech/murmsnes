/*
 * murmsnes Settings
 * Runtime configuration with in-game settings menu
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

// Input mode values
#define INPUT_MODE_ANY      0
#define INPUT_MODE_NES1     1
#define INPUT_MODE_NES2     2
#define INPUT_MODE_USB1     3
#define INPUT_MODE_USB2     4
#define INPUT_MODE_KEYBOARD 5
#define INPUT_MODE_DISABLED 6
#define INPUT_MODE_COUNT    7

// Volume range (0=OFF, 10-100 step 10)
#define VOLUME_MIN  0
#define VOLUME_MAX  100
#define VOLUME_STEP 10

// CRT effect range (0=OFF, 10-100 step 10)
#define CRT_MIN  0
#define CRT_MAX  100
#define CRT_STEP 10

typedef struct {
    uint8_t p1_mode;              // Player 1 input mode (INPUT_MODE_*)
    uint8_t p2_mode;              // Player 2 input mode (INPUT_MODE_*)
    uint8_t volume;               // Master volume 0-100 (0=OFF)
    uint8_t crt_effect;           // CRT scanline intensity 0-100 (0=OFF)
    uint8_t frameskip;            // 0=none, 1=low, 2=medium, 3=high, 4=extreme

    // Video settings
    uint8_t bg_enabled;           // BG1-4 enable bits (bit 0=BG1, ..., bit 3=BG4)
    bool    sprites_enabled;
    bool    transparency_enabled;
    bool    hdma_enabled;

    // Audio settings
    bool    echo_enabled;         // Sound echo (reverb)
    bool    interpolation;        // Sound interpolation
} settings_t;

extern settings_t g_settings;

typedef enum {
    SETTINGS_RESULT_EXIT,         // Back to game / back to ROM selector
    SETTINGS_RESULT_ROM_SELECT,   // Return to ROM selector (no reboot)
} settings_result_t;

/**
 * Load settings from SD card (/snes/settings.ini)
 * Uses defaults if file doesn't exist.
 */
void settings_load(void);

/**
 * Save current settings to SD card
 */
bool settings_save(void);

/**
 * Apply settings that affect the emulator at runtime
 * (frameskip, echo, interpolation, BG/sprite/transparency/HDMA enables)
 */
void settings_apply_runtime(void);

/**
 * Check if menu hotkey is pressed (Start+Select, ESC, or F12)
 * Call during emulation loop or ROM selector.
 */
bool settings_check_hotkey(void);

/**
 * Display settings menu and block until user exits.
 * @param screen_buffer 256x224 byte buffer for menu rendering
 * @param in_game true when called from emulation, false from ROM selector
 * @return SETTINGS_RESULT_EXIT or SETTINGS_RESULT_ROM_SELECT
 */
settings_result_t settings_menu_show(uint8_t *screen_buffer, bool in_game);

#endif // SETTINGS_H
