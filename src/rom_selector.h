/*
 * ROM Selector - Start screen for murmsnes
 * Allows user to browse and select SNES ROMs from SD card
 */
#ifndef ROM_SELECTOR_H
#define ROM_SELECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum length of ROM filename (including path)
#define MAX_ROM_PATH 128

/**
 * Display ROM selection screen and wait for user to select a ROM
 * @param selected_rom_path Buffer to store the selected ROM path
 * @param buffer_size Size of the buffer
 * @param screen_buffer Pointer to the screen buffer (256x239 16-bit RGB565)
 * @return true if ROM was selected, false if user canceled or no ROMs found
 */
bool rom_selector_show(char *selected_rom_path, size_t buffer_size, uint16_t *screen_buffer);

/**
 * Display SD card error screen (blocks forever)
 * @param screen_buffer Pointer to the screen buffer (256x239 16-bit RGB565)
 * @param error_code The FRESULT error code from f_mount
 */
void rom_selector_show_sd_error(uint16_t *screen_buffer, int error_code);

#endif // ROM_SELECTOR_H
