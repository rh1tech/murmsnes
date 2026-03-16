/*
 * Settings Implementation for murmsnes
 * Simplified settings menu - placeholder for now
 */
#include "settings.h"
#include "menu_ui.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "board_config.h"
#include "nespad/nespad.h"
#include "ps2kbd/ps2kbd_wrapper.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef USB_HID_ENABLED
#include "usbhid/usbhid.h"
#endif

// Global settings instance with defaults
settings_t g_settings = {
    .cpu_freq = 504,
    .psram_freq = 166,
    .audio_enabled = true,
    .crt_effect = false,
    .crt_dim = 60,
    .frameskip = 3,
    .gamepad2_mode = GAMEPAD2_MODE_NES,
};

// Menu items
typedef enum {
    MENU_ITEM_CPU_FREQ,
    MENU_ITEM_PSRAM_FREQ,
    MENU_ITEM_AUDIO,
    MENU_ITEM_CRT_EFFECT,
    MENU_ITEM_FRAMESKIP,
    MENU_ITEM_GAMEPAD2,
    MENU_ITEM_SAVE_RESTART,
    MENU_ITEM_RESTART,
    MENU_ITEM_CANCEL,
    MENU_ITEM_COUNT
} menu_item_t;

static const char *menu_labels[MENU_ITEM_COUNT] = {
    "CPU FREQUENCY",
    "PSRAM FREQUENCY",
    "AUDIO",
    "CRT EFFECT",
    "FRAMESKIP",
    "GAMEPAD 2",
    "SAVE AND RESTART",
    "RESTART",
    "CANCEL"
};

// Layout constants
#define SETTINGS_MENU_Y 50
#define SETTINGS_LINE_HEIGHT 16
#define SETTINGS_VALUE_X 180

static void draw_menu_item(uint8_t *screen, int item, int selected) {
    int y = SETTINGS_MENU_Y + item * SETTINGS_LINE_HEIGHT;
    bool is_selected = (item == selected);

    // Draw background
    if (is_selected) {
        menu_fill_rect(screen, 8, y - 2, MENU_SCREEN_WIDTH - 16, SETTINGS_LINE_HEIGHT, COLOR_WHITE);
    } else {
        menu_fill_rect(screen, 8, y - 2, MENU_SCREEN_WIDTH - 16, SETTINGS_LINE_HEIGHT, COLOR_BLACK);
    }

    uint8_t text_color = is_selected ? COLOR_BLACK : COLOR_WHITE;

    // Draw label
    menu_draw_text(screen, 12, y, menu_labels[item], text_color);

    // Draw value (for items with values)
    char value_str[32] = "";
    switch (item) {
        case MENU_ITEM_CPU_FREQ:
            snprintf(value_str, sizeof(value_str), "%d MHZ", g_settings.cpu_freq);
            break;
        case MENU_ITEM_PSRAM_FREQ:
            snprintf(value_str, sizeof(value_str), "%d MHZ", g_settings.psram_freq);
            break;
        case MENU_ITEM_AUDIO:
            snprintf(value_str, sizeof(value_str), "%s", g_settings.audio_enabled ? "ON" : "OFF");
            break;
        case MENU_ITEM_CRT_EFFECT:
            if (g_settings.crt_effect) {
                snprintf(value_str, sizeof(value_str), "%d%%", g_settings.crt_dim);
            } else {
                snprintf(value_str, sizeof(value_str), "OFF");
            }
            break;
        case MENU_ITEM_FRAMESKIP:
            {
                const char *skip_names[] = {"NONE", "LOW", "MEDIUM", "HIGH", "EXTREME"};
                snprintf(value_str, sizeof(value_str), "%s", skip_names[g_settings.frameskip]);
            }
            break;
        case MENU_ITEM_GAMEPAD2:
            {
                const char *gp2_names[] = {"NES", "KEYBOARD", "USB", "DISABLED"};
                snprintf(value_str, sizeof(value_str), "%s", gp2_names[g_settings.gamepad2_mode]);
            }
            break;
        default:
            break;
    }

    if (value_str[0]) {
        menu_draw_text(screen, SETTINGS_VALUE_X, y, value_str, text_color);
    }
}

static void draw_settings_menu(uint8_t *screen, int selected) {
    // Clear screen
    menu_clear_screen(screen, COLOR_BLACK);

    // Draw title
    menu_draw_text_bold_centered(screen, 16, "SETTINGS", COLOR_WHITE);

    // Draw menu items
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        draw_menu_item(screen, i, selected);
    }

    // Draw footer help
    menu_draw_text_centered(screen, MENU_SCREEN_HEIGHT - 16, "D-PAD:SELECT  L/R:CHANGE  B:BACK", COLOR_GRAY);
}

static void adjust_setting(int item, int direction) {
    switch (item) {
        case MENU_ITEM_CPU_FREQ:
            if (direction > 0) {
                if (g_settings.cpu_freq == 378) g_settings.cpu_freq = 504;
                else if (g_settings.cpu_freq < 378) g_settings.cpu_freq = 378;
            } else {
                if (g_settings.cpu_freq == 504) g_settings.cpu_freq = 378;
                else if (g_settings.cpu_freq > 378) g_settings.cpu_freq = 378;
            }
            break;

        case MENU_ITEM_PSRAM_FREQ:
            if (direction > 0) {
                if (g_settings.psram_freq == 133) g_settings.psram_freq = 166;
            } else {
                if (g_settings.psram_freq == 166) g_settings.psram_freq = 133;
            }
            break;

        case MENU_ITEM_AUDIO:
            g_settings.audio_enabled = !g_settings.audio_enabled;
            break;

        case MENU_ITEM_CRT_EFFECT:
            if (!g_settings.crt_effect) {
                g_settings.crt_effect = true;
                g_settings.crt_dim = 60;
            } else {
                if (direction > 0) {
                    g_settings.crt_dim += 10;
                    if (g_settings.crt_dim > 90) {
                        g_settings.crt_effect = false;
                        g_settings.crt_dim = 60;
                    }
                } else {
                    g_settings.crt_dim -= 10;
                    if (g_settings.crt_dim < 10) {
                        g_settings.crt_effect = false;
                        g_settings.crt_dim = 60;
                    }
                }
            }
            break;

        case MENU_ITEM_FRAMESKIP:
            if (direction > 0) {
                g_settings.frameskip++;
                if (g_settings.frameskip > 4) g_settings.frameskip = 0;
            } else {
                if (g_settings.frameskip == 0) g_settings.frameskip = 4;
                else g_settings.frameskip--;
            }
            break;

        case MENU_ITEM_GAMEPAD2:
            if (direction > 0) {
                g_settings.gamepad2_mode++;
                if (g_settings.gamepad2_mode > 3) g_settings.gamepad2_mode = 0;
            } else {
                if (g_settings.gamepad2_mode == 0) g_settings.gamepad2_mode = 3;
                else g_settings.gamepad2_mode--;
            }
            break;

        default:
            break;
    }
}

void settings_load(void) {
    FIL file;
    char line[128];

    FRESULT res = f_open(&file, "/snes/settings.ini", FA_READ);
    if (res != FR_OK) {
        // Try uppercase path
        res = f_open(&file, "/SNES/settings.ini", FA_READ);
        if (res != FR_OK) {
            // Use defaults
            return;
        }
    }

    while (f_gets(line, sizeof(line), &file)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        // Trim whitespace
        while (*key == ' ') key++;
        while (*value == ' ') value++;
        char *end = value + strlen(value) - 1;
        while (end > value && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';

        // Parse settings
        if (strcmp(key, "cpu_freq") == 0) {
            g_settings.cpu_freq = (uint16_t)atoi(value);
        } else if (strcmp(key, "psram_freq") == 0) {
            g_settings.psram_freq = (uint16_t)atoi(value);
        } else if (strcmp(key, "audio_enabled") == 0) {
            g_settings.audio_enabled = (atoi(value) != 0);
        } else if (strcmp(key, "crt_effect") == 0) {
            g_settings.crt_effect = (atoi(value) != 0);
        } else if (strcmp(key, "crt_dim") == 0) {
            g_settings.crt_dim = (uint8_t)atoi(value);
        } else if (strcmp(key, "frameskip") == 0) {
            g_settings.frameskip = (uint8_t)atoi(value);
        } else if (strcmp(key, "gamepad2_mode") == 0) {
            g_settings.gamepad2_mode = (uint8_t)atoi(value);
        }
    }

    f_close(&file);
}

bool settings_save(void) {
    FIL file;

    FRESULT res = f_open(&file, "/snes/settings.ini", FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        // Try creating directory first
        f_mkdir("/snes");
        res = f_open(&file, "/snes/settings.ini", FA_WRITE | FA_CREATE_ALWAYS);
        if (res != FR_OK) {
            return false;
        }
    }

    f_printf(&file, "cpu_freq=%d\n", g_settings.cpu_freq);
    f_printf(&file, "psram_freq=%d\n", g_settings.psram_freq);
    f_printf(&file, "audio_enabled=%d\n", g_settings.audio_enabled ? 1 : 0);
    f_printf(&file, "crt_effect=%d\n", g_settings.crt_effect ? 1 : 0);
    f_printf(&file, "crt_dim=%d\n", g_settings.crt_dim);
    f_printf(&file, "frameskip=%d\n", g_settings.frameskip);
    f_printf(&file, "gamepad2_mode=%d\n", g_settings.gamepad2_mode);

    f_close(&file);
    return true;
}

void settings_apply_runtime(void) {
    // Apply settings that can be changed at runtime
    // This is a placeholder - actual implementation depends on emulator integration
}

settings_result_t settings_menu_show(uint8_t *screen_buffer) {
    int selected = 0;
    uint32_t prev_buttons = 0;

    draw_settings_menu(screen_buffer, selected);

    // Wait for button release
    sleep_ms(100);

    while (true) {
        // Read gamepad
        nespad_read();
        uint32_t buttons = nespad_state;

        // Poll PS/2 keyboard
        ps2kbd_tick();
        uint16_t kbd_state = ps2kbd_get_state();

#ifdef USB_HID_ENABLED
        kbd_state |= usbhid_get_kbd_state();
        usbhid_task();
#endif

        // Merge keyboard state
        if (kbd_state & KBD_STATE_UP)    buttons |= DPAD_UP;
        if (kbd_state & KBD_STATE_DOWN)  buttons |= DPAD_DOWN;
        if (kbd_state & KBD_STATE_LEFT)  buttons |= DPAD_LEFT;
        if (kbd_state & KBD_STATE_RIGHT) buttons |= DPAD_RIGHT;
        if (kbd_state & KBD_STATE_A)     buttons |= DPAD_A;
        if (kbd_state & KBD_STATE_B)     buttons |= DPAD_B;
        if (kbd_state & KBD_STATE_L)     buttons |= DPAD_LT;
        if (kbd_state & KBD_STATE_R)     buttons |= DPAD_RT;
        if (kbd_state & KBD_STATE_START) buttons |= DPAD_START;
        if (kbd_state & KBD_STATE_ESC)   buttons |= DPAD_B;

        // Detect button press
        uint32_t buttons_pressed = buttons & ~prev_buttons;
        prev_buttons = buttons;

        bool redraw = false;

        // Navigation
        if (buttons_pressed & DPAD_UP) {
            selected--;
            if (selected < 0) selected = MENU_ITEM_COUNT - 1;
            redraw = true;
        }

        if (buttons_pressed & DPAD_DOWN) {
            selected++;
            if (selected >= MENU_ITEM_COUNT) selected = 0;
            redraw = true;
        }

        // Adjust values with left/right or L/R
        if (buttons_pressed & (DPAD_LEFT | DPAD_LT)) {
            adjust_setting(selected, -1);
            redraw = true;
        }

        if (buttons_pressed & (DPAD_RIGHT | DPAD_RT)) {
            adjust_setting(selected, 1);
            redraw = true;
        }

        // Action buttons
        if (buttons_pressed & (DPAD_A | DPAD_START)) {
            switch (selected) {
                case MENU_ITEM_SAVE_RESTART:
                    return SETTINGS_RESULT_SAVE_RESTART;
                case MENU_ITEM_RESTART:
                    return SETTINGS_RESULT_RESTART;
                case MENU_ITEM_CANCEL:
                    return SETTINGS_RESULT_CANCEL;
                default:
                    // Toggle/adjust the setting
                    adjust_setting(selected, 1);
                    redraw = true;
                    break;
            }
        }

        // Cancel with B
        if (buttons_pressed & DPAD_B) {
            return SETTINGS_RESULT_CANCEL;
        }

        if (redraw) {
            draw_settings_menu(screen_buffer, selected);
        }

        sleep_ms(50);
    }

    return SETTINGS_RESULT_CANCEL;
}

bool settings_check_hotkey(void) {
    nespad_read();
    return (nespad_state & DPAD_SELECT) && (nespad_state & DPAD_START);
}
