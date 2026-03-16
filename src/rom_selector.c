/*
 * ROM Selector Implementation for murmsnes
 * Adapted from murmgenesis for 16-bit RGB565 256x239 display
 */
#include "rom_selector.h"
#include "menu_ui.h"
#include "settings.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "HDMI.h"
#include "board_config.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "nespad/nespad.h"
#include "ps2kbd/ps2kbd_wrapper.h"
#include <string.h>
#include <stdio.h>

#ifndef MURMSNES_VERSION
#define MURMSNES_VERSION "?"
#endif

// USB HID gamepad support
#ifdef USB_HID_ENABLED
#include "usbhid/usbhid.h"
#endif

// ROM list management - keep small to save RAM
#define MAX_ROMS 64
typedef struct {
    char filename[48];      // Shorter to save RAM
    char display_name[32];  // Shorter to save RAM
} rom_entry_t;

static rom_entry_t rom_list[MAX_ROMS];  // 64 * 80 = 5KB
static int rom_count = 0;

// Layout constants adjusted for 256x239 screen
#define VISIBLE_LINES 11  // Adjusted for smaller screen
#define HEADER_Y 12
#define HEADER_HEIGHT 32
#define MENU_X 16
#define MENU_Y 52
#define MAX_ROM_NAME_CHARS 32  // Shorter for narrower screen
#define SCROLLBAR_WIDTH 4
#define SCROLLBAR_X (MENU_SCREEN_WIDTH - 16)
#define SCROLLBAR_Y MENU_Y
#define SCROLLBAR_HEIGHT (VISIBLE_LINES * LINE_HEIGHT)
#define MENU_WIDTH (SCROLLBAR_X - MENU_X - 8)

#define FOOTER_Y (MENU_SCREEN_HEIGHT - (FONT_HEIGHT + 4))
#define INFO_BASE_Y (FOOTER_Y - LINE_HEIGHT)
#define HELP_Y (INFO_BASE_Y - LINE_HEIGHT)

// Smooth scrollbar animation
static int scrollbar_current_y = 0;
static int scrollbar_target_y = 0;

// Smooth cursor animation
static int cursor_current_y = 0;
static int cursor_target_y = 0;

// Track previous state for smart redraws
static int prev_scroll_offset = -1;
static int prev_cursor_y = -1;

static void draw_demostyle_header(uint8_t *screen, uint32_t phase) {
    // Calculate title dimensions
    const char *title = "MURMSNES";
    int title_width = menu_text_width_bold(title);
    int title_x = (MENU_SCREEN_WIDTH - title_width) / 2;
    int title_y = HEADER_Y + (HEADER_HEIGHT - BOLD_FONT_HEIGHT) / 2;
    int title_left = title_x - 6;
    int title_right = title_x + title_width + 6;
    int title_top = title_y - 4;
    int title_bottom = title_y + BOLD_FONT_HEIGHT + 4;

    // Draw animated background, skip title area
    for (int y = 0; y < HEADER_HEIGHT; ++y) {
        int yy = HEADER_Y + y;
        if (yy < 0 || yy >= MENU_SCREEN_HEIGHT) continue;

        uint8_t row_phase = (uint8_t)((phase + y * 5) & 0x3F);
        for (int x = 0; x < MENU_SCREEN_WIDTH; ++x) {
            // Skip title area
            if (yy >= title_top && yy < title_bottom && x >= title_left && x < title_right) {
                continue;
            }
            uint8_t wave = (uint8_t)(((x * 3) + phase) & 0x3F);
            uint8_t intensity = (uint8_t)(8 + ((wave + (row_phase >> 1)) & 0x1F));
            if (intensity > 31) intensity = 31;
            // Use palette index directly (dark range for gradient effect)
            screen[yy * MENU_SCREEN_WIDTH + x] = (uint8_t)(intensity >> 1);
        }

        // Horizontal stripes - brighten slightly
        if ((y & 3) == 0) {
            uint8_t *row = &screen[yy * MENU_SCREEN_WIDTH];
            for (int x = 0; x < MENU_SCREEN_WIDTH; ++x) {
                if (yy >= title_top && yy < title_bottom && x >= title_left && x < title_right) continue;
                uint8_t c = row[x];
                if (c < 250) c += 4;
                row[x] = c;
            }
        }
    }

    // Draw solid background for title
    menu_fill_rect(screen, title_left, title_top, title_right - title_left, title_bottom - title_top, COLOR_BLACK);

    // Draw bold title with shadow
    menu_draw_text_bold(screen, title_x + 1, title_y + 1, title, COLOR_DARK);
    menu_draw_text_bold(screen, title_x, title_y, title, COLOR_WHITE);
}

static void draw_info_text(uint8_t *screen) {
    char info_str[64];

    // Board label
#ifdef BOARD_M2
    const char *board_str = "M2";
#else
    const char *board_str = "M1";
#endif

    // Current system clock in MHz
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t cpu_mhz = (sys_hz + 500000) / 1000000;

    // PSRAM clock from settings
    uint32_t psram_mhz = g_settings.psram_freq;

    snprintf(info_str, sizeof(info_str), "V%s %s %lu/%lu MHZ", MURMSNES_VERSION, board_str, cpu_mhz, psram_mhz);

    menu_draw_text_centered(screen, INFO_BASE_Y - LINE_HEIGHT, info_str, COLOR_WHITE);
}

static void draw_help_text(uint8_t *screen) {
    const char *help = "D-PAD:SELECT  A/START:OK";
    menu_draw_text_centered(screen, INFO_BASE_Y, help, COLOR_WHITE);
}

static void draw_footer(uint8_t *screen) {
    const char *footer = "CODED BY MIKHAIL MATVEEV";
    menu_draw_text_centered(screen, FOOTER_Y, footer, COLOR_WHITE);
}

// Scan /snes directory for ROM files
static void scan_roms(void) {
    DIR dir;
    FILINFO fno;
    rom_count = 0;

    FRESULT res = f_opendir(&dir, "/snes");
    if (res != FR_OK) {
        // Try uppercase
        res = f_opendir(&dir, "/SNES");
        if (res != FR_OK) {
            return;
        }
    }

    while (rom_count < MAX_ROMS) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        // Skip directories and hidden files
        if (fno.fattrib & AM_DIR || fno.fname[0] == '.') continue;

        // Check for supported SNES extensions
        const char *ext = strrchr(fno.fname, '.');
        if (ext && (strcasecmp(ext, ".smc") == 0 ||
                    strcasecmp(ext, ".sfc") == 0 ||
                    strcasecmp(ext, ".fig") == 0)) {

            strncpy(rom_list[rom_count].filename, fno.fname, sizeof(rom_list[rom_count].filename) - 1);
            rom_list[rom_count].filename[sizeof(rom_list[rom_count].filename) - 1] = '\0';

            // Create display name (remove extension, uppercase)
            strncpy(rom_list[rom_count].display_name, fno.fname, sizeof(rom_list[rom_count].display_name) - 1);
            rom_list[rom_count].display_name[sizeof(rom_list[rom_count].display_name) - 1] = '\0';

            // Remove extension for display
            char *dot = strrchr(rom_list[rom_count].display_name, '.');
            if (dot) *dot = '\0';

            // Convert to uppercase for display
            for (char *p = rom_list[rom_count].display_name; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p = *p - 'a' + 'A';
            }

            rom_count++;
        }
    }

    f_closedir(&dir);
}

// Draw scrollbar with smooth animation
static void draw_scrollbar(uint8_t *screen, int scroll_offset, bool animate) {
    if (rom_count <= VISIBLE_LINES) return;

    // Calculate scrollbar thumb position and size
    int thumb_height = (SCROLLBAR_HEIGHT * VISIBLE_LINES) / rom_count;
    if (thumb_height < 8) thumb_height = 8;

    int thumb_max_y = SCROLLBAR_HEIGHT - thumb_height;
    scrollbar_target_y = (thumb_max_y * scroll_offset) / (rom_count - VISIBLE_LINES);

    // Smooth interpolation towards target
    if (animate) {
        int diff = scrollbar_target_y - scrollbar_current_y;
        if (diff != 0) {
            int step = diff / 2;
            if (step == 0) step = (diff > 0) ? 1 : -1;
            scrollbar_current_y += step;
        }
    } else {
        scrollbar_current_y = scrollbar_target_y;
    }

    // Draw scrollbar background (dark)
    menu_fill_rect(screen, SCROLLBAR_X, SCROLLBAR_Y, SCROLLBAR_WIDTH, SCROLLBAR_HEIGHT, COLOR_DARK);

    // Draw scrollbar thumb (bright)
    menu_fill_rect(screen, SCROLLBAR_X, SCROLLBAR_Y + scrollbar_current_y, SCROLLBAR_WIDTH, thumb_height, COLOR_WHITE);
}

// Check if any animation is in progress
static bool animation_in_progress(void) {
    return (cursor_current_y != cursor_target_y) || (scrollbar_current_y != scrollbar_target_y);
}

// Draw a single ROM row at a given Y position
static void draw_single_row(uint8_t *screen, int row_idx, int scroll_offset, int cursor_y) {
    int rom_idx = scroll_offset + row_idx;
    if (rom_idx >= rom_count || row_idx < 0 || row_idx >= VISIBLE_LINES) return;

    int text_y = MENU_Y + row_idx * LINE_HEIGHT;
    int row_top = text_y - 1;
    int row_bottom = row_top + LINE_HEIGHT;

    // Check if this row overlaps with the highlight bar
    int cursor_top = cursor_y;
    int cursor_bottom = cursor_y + LINE_HEIGHT;
    bool overlaps_cursor = (row_bottom > cursor_top && row_top < cursor_bottom);

    // Clear row to black first
    menu_fill_rect(screen, MENU_X - 2, row_top, MENU_WIDTH, LINE_HEIGHT, COLOR_BLACK);

    // Draw highlight if overlapping
    if (overlaps_cursor) {
        int highlight_start = (cursor_top > row_top) ? cursor_top : row_top;
        int highlight_end = (cursor_bottom < row_bottom) ? cursor_bottom : row_bottom;
        menu_fill_rect(screen, MENU_X - 2, highlight_start, MENU_WIDTH, highlight_end - highlight_start, COLOR_WHITE);
    }

    // Truncate long names with ellipsis
    char display_name[MAX_ROM_NAME_CHARS + 4];
    size_t name_len = strlen(rom_list[rom_idx].display_name);
    if (name_len > MAX_ROM_NAME_CHARS) {
        strncpy(display_name, rom_list[rom_idx].display_name, MAX_ROM_NAME_CHARS - 3);
        display_name[MAX_ROM_NAME_CHARS - 3] = '\0';
        strcat(display_name, "...");
    } else {
        strncpy(display_name, rom_list[rom_idx].display_name, MAX_ROM_NAME_CHARS);
        display_name[MAX_ROM_NAME_CHARS] = '\0';
    }

    // Draw text - black if overlapping highlight, white otherwise
    menu_draw_text(screen, MENU_X, text_y, display_name, overlaps_cursor ? COLOR_BLACK : COLOR_WHITE);
}

// Render ROM menu with smooth cursor
static void render_rom_menu(uint8_t *screen, int selected, int scroll_offset, bool selection_changed) {
    if (rom_count == 0) {
        if (selection_changed) {
            menu_fill_rect(screen, MENU_X - 2, MENU_Y - 2, MENU_WIDTH + 4, (VISIBLE_LINES * LINE_HEIGHT) + 4, COLOR_BLACK);
            menu_draw_text(screen, MENU_X, MENU_Y, "NO ROMS FOUND", COLOR_WHITE);
        }
        return;
    }

    int visible_idx = selected - scroll_offset;

    // Update cursor target
    cursor_target_y = MENU_Y - 1 + visible_idx * LINE_HEIGHT;

    // Check if cursor needs to animate
    bool cursor_moved = (cursor_current_y != cursor_target_y);

    // Smooth cursor interpolation
    if (cursor_moved) {
        int diff = cursor_target_y - cursor_current_y;
        int step = diff / 3;
        if (step == 0) step = (diff > 0) ? 1 : -1;
        cursor_current_y += step;
    }

    // If scroll offset changed, do full redraw and snap cursor
    if (scroll_offset != prev_scroll_offset) {
        cursor_current_y = cursor_target_y;
        prev_scroll_offset = scroll_offset;
        prev_cursor_y = cursor_current_y;

        // Full redraw on scroll change
        for (int i = 0; i < VISIBLE_LINES && (scroll_offset + i) < rom_count; i++) {
            draw_single_row(screen, i, scroll_offset, cursor_current_y);
        }
        draw_scrollbar(screen, scroll_offset, true);
        draw_help_text(screen);
        draw_info_text(screen);
        draw_footer(screen);
        return;
    }

    // If cursor hasn't moved, just update scrollbar if needed
    if (!cursor_moved && prev_cursor_y == cursor_current_y) {
        if (scrollbar_current_y != scrollbar_target_y) {
            draw_scrollbar(screen, scroll_offset, true);
        }
        draw_help_text(screen);
        draw_info_text(screen);
        draw_footer(screen);
        return;
    }

    // Calculate which rows are affected by cursor movement
    int old_cursor_row_start = (prev_cursor_y - MENU_Y + 1) / LINE_HEIGHT;
    int old_cursor_row_end = (prev_cursor_y + LINE_HEIGHT - MENU_Y + 1) / LINE_HEIGHT;
    int new_cursor_row_start = (cursor_current_y - MENU_Y + 1) / LINE_HEIGHT;
    int new_cursor_row_end = (cursor_current_y + LINE_HEIGHT - MENU_Y + 1) / LINE_HEIGHT;

    // Clamp to valid range
    if (old_cursor_row_start < 0) old_cursor_row_start = 0;
    if (old_cursor_row_end >= VISIBLE_LINES) old_cursor_row_end = VISIBLE_LINES - 1;
    if (new_cursor_row_start < 0) new_cursor_row_start = 0;
    if (new_cursor_row_end >= VISIBLE_LINES) new_cursor_row_end = VISIBLE_LINES - 1;

    // Find the range of rows that need redrawing
    int min_row = (old_cursor_row_start < new_cursor_row_start) ? old_cursor_row_start : new_cursor_row_start;
    int max_row = (old_cursor_row_end > new_cursor_row_end) ? old_cursor_row_end : new_cursor_row_end;

    // Only redraw affected rows
    for (int i = min_row; i <= max_row && (scroll_offset + i) < rom_count; i++) {
        draw_single_row(screen, i, scroll_offset, cursor_current_y);
    }

    prev_cursor_y = cursor_current_y;

    draw_scrollbar(screen, scroll_offset, true);
    draw_help_text(screen);
    draw_info_text(screen);
    draw_footer(screen);
}

void rom_selector_show_sd_error(uint8_t *screen_buffer, int error_code) {
    // Clear screen
    menu_clear_screen(screen_buffer, COLOR_BLACK);

    // Draw header
    draw_demostyle_header(screen_buffer, 0);

    // Error message
    menu_draw_text_centered(screen_buffer, MENU_Y, "SD CARD ERROR", COLOR_RED);
    menu_draw_text_centered(screen_buffer, MENU_Y + 20, "NO SD CARD DETECTED", COLOR_WHITE);
    menu_draw_text_centered(screen_buffer, MENU_Y + 32, "OR CARD NOT FORMATTED", COLOR_WHITE);

    // Error code
    char code_str[32];
    snprintf(code_str, sizeof(code_str), "ERROR CODE: %d", error_code);
    menu_draw_text_centered(screen_buffer, MENU_Y + 52, code_str, COLOR_GRAY);

    // Instructions
    menu_draw_text_centered(screen_buffer, MENU_Y + 80, "INSERT A FAT32 SD CARD", COLOR_YELLOW);
    menu_draw_text_centered(screen_buffer, MENU_Y + 92, "AND RESET THE DEVICE", COLOR_YELLOW);

    draw_footer(screen_buffer);

    // Block forever
    while (1) {
        tight_loop_contents();
    }
}

bool rom_selector_show(char *selected_rom_path, size_t buffer_size, uint8_t *screen_buffer) {
#if ENABLE_LOGGING
    printf("ROM Selector: Starting...\n");
#endif

    // Clear screen and scan ROMs
    menu_clear_screen(screen_buffer, COLOR_BLACK);
    scan_roms();

#if ENABLE_LOGGING
    printf("ROM Selector: Found %d ROMs\n", rom_count);
#endif

    int selected = 0;
    int scroll_offset = 0;
    uint32_t header_phase = 0;

    draw_demostyle_header(screen_buffer, header_phase);
    draw_info_text(screen_buffer);

    // Initialize animation positions
    scrollbar_current_y = 0;
    scrollbar_target_y = 0;
    cursor_current_y = MENU_Y - 1;
    cursor_target_y = MENU_Y - 1;
    prev_scroll_offset = -1;
    prev_cursor_y = cursor_current_y;
    render_rom_menu(screen_buffer, selected, scroll_offset, true);

    // If no ROMs found, wait and return false
    if (rom_count == 0) {
#if ENABLE_LOGGING
        printf("ROM Selector: No ROMs found!\n");
#endif
        sleep_ms(3000);
        return false;
    }

    // Wait for input
    int prev_selected = -1;
    int prev_scroll = -1;
    uint32_t prev_buttons = 0;
    uint32_t hold_counter = 0;
    const uint32_t REPEAT_DELAY = 10;
    const uint32_t REPEAT_RATE = 3;

    sleep_ms(100);

    while (true) {
        draw_demostyle_header(screen_buffer, header_phase);
        header_phase = (header_phase + 2) & 0x3F;

        bool selection_changed = (selected != prev_selected || scroll_offset != prev_scroll);
        if (selection_changed) {
            prev_selected = selected;
            prev_scroll = scroll_offset;
        }

        render_rom_menu(screen_buffer, selected, scroll_offset, selection_changed);

        // Read gamepad
        nespad_read();
        uint32_t buttons = nespad_state;

        // Poll PS/2 keyboard
        ps2kbd_tick();
        uint16_t kbd_state = ps2kbd_get_state();

#ifdef USB_HID_ENABLED
        kbd_state |= usbhid_get_kbd_state();
#endif

        // Merge keyboard state into buttons
        if (kbd_state & KBD_STATE_UP)    buttons |= DPAD_UP;
        if (kbd_state & KBD_STATE_DOWN)  buttons |= DPAD_DOWN;
        if (kbd_state & KBD_STATE_LEFT)  buttons |= DPAD_LEFT;
        if (kbd_state & KBD_STATE_RIGHT) buttons |= DPAD_RIGHT;
        if (kbd_state & KBD_STATE_A)     buttons |= DPAD_A;
        if (kbd_state & KBD_STATE_B)     buttons |= DPAD_B;
        if (kbd_state & KBD_STATE_START) buttons |= DPAD_START;
        if (kbd_state & KBD_STATE_ESC)   buttons |= (DPAD_SELECT | DPAD_START);

#ifdef USB_HID_ENABLED
        usbhid_task();
        if (usbhid_gamepad_connected()) {
            usbhid_gamepad_state_t gp;
            usbhid_get_gamepad_state(&gp);

            if (gp.dpad & 0x01) buttons |= DPAD_UP;
            if (gp.dpad & 0x02) buttons |= DPAD_DOWN;
            if (gp.dpad & 0x04) buttons |= DPAD_LEFT;
            if (gp.dpad & 0x08) buttons |= DPAD_RIGHT;
            if (gp.buttons & 0x01) buttons |= DPAD_A;
            if (gp.buttons & 0x02) buttons |= DPAD_B;
            if (gp.buttons & 0x40) buttons |= DPAD_START;
        }
#endif

        // Detect button press
        uint32_t buttons_pressed = buttons & ~prev_buttons;

        // Key repeat logic
        bool up_repeat = false;
        bool down_repeat = false;
        if (buttons & (DPAD_UP | DPAD_DOWN)) {
            hold_counter++;
            if (hold_counter > REPEAT_DELAY && ((hold_counter - REPEAT_DELAY) % REPEAT_RATE == 0)) {
                if (buttons & DPAD_UP) up_repeat = true;
                if (buttons & DPAD_DOWN) down_repeat = true;
            }
        } else {
            hold_counter = 0;
        }
        prev_buttons = buttons;

        // Handle input
        if ((buttons_pressed & DPAD_UP) || up_repeat) {
            selected--;
            if (selected < 0) {
                selected = rom_count - 1;
                scroll_offset = (rom_count > VISIBLE_LINES) ? (rom_count - VISIBLE_LINES) : 0;
            } else if (selected < scroll_offset) {
                scroll_offset = selected;
            }
        }

        if ((buttons_pressed & DPAD_DOWN) || down_repeat) {
            selected++;
            if (selected >= rom_count) {
                selected = 0;
                scroll_offset = 0;
            } else if (selected >= scroll_offset + VISIBLE_LINES) {
                scroll_offset = selected - VISIBLE_LINES + 1;
            }
        }

        // Check for Start+Select combo (for settings menu)
        if ((buttons & DPAD_SELECT) && (buttons & DPAD_START)) {
            // Wait for buttons to be released
            while ((nespad_state & DPAD_SELECT) && (nespad_state & DPAD_START)) {
                nespad_read();
                sleep_ms(50);
            }

            // Show settings menu
            settings_result_t result = settings_menu_show(screen_buffer);

            switch (result) {
                case SETTINGS_RESULT_SAVE_RESTART:
                    settings_save();
                    watchdog_reboot(0, 0, 10);
                    while(1) tight_loop_contents();
                    break;

                case SETTINGS_RESULT_RESTART:
                    watchdog_reboot(0, 0, 10);
                    while(1) tight_loop_contents();
                    break;

                case SETTINGS_RESULT_CANCEL:
                default:
                    // Redraw the ROM selector
                    menu_clear_screen(screen_buffer, COLOR_BLACK);
                    draw_demostyle_header(screen_buffer, header_phase);
                    prev_scroll_offset = -1;  // Force full redraw
                    prev_buttons = 0;
                    break;
            }
            continue;
        }

        // Confirm ROM selection
        if (buttons_pressed & (DPAD_A | DPAD_START)) {
            if (!(buttons & DPAD_SELECT)) {
                if (rom_count > 0) {
                    snprintf(selected_rom_path, buffer_size, "/snes/%s", rom_list[selected].filename);
                    return true;
                }
            }
        }

        sleep_ms(animation_in_progress() ? 16 : 50);
    }

    return false;
}
