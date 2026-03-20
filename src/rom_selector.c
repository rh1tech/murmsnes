/*
 * murmsnes ROM Selector - Cartridge-style display with cover art
 * Ported from murmnes, adapted for SNES (256x224 8-bit palette-indexed HDMI)
 */

#include "rom_selector.h"
#include "menu_ui.h"
#include "settings.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "HDMI.h"
#include "board_config.h"
#include "psram_allocator.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "nespad/nespad.h"
#include "ps2kbd/ps2kbd_wrapper.h"
#include <string.h>
#include <stdio.h>

#ifdef USB_HID_ENABLED
#include "usbhid/usbhid.h"
#endif

extern volatile uint32_t current_buffer;
extern uint8_t SCREEN[2][256 * 224];

#ifndef MURMSNES_VERSION
#define MURMSNES_VERSION "?"
#endif

#define SCREEN_W 256
#define SCREEN_H 224

/* ─── Fixed 6x6x6 RGB color cube palette (216 colors) ────────────── */

#define PAL_BLACK      0
#define PAL_CUBE_BASE  1
#define PAL_CART_BODY  217
#define PAL_CART_LIGHT 218
#define PAL_CART_DARK  219
#define PAL_CART_LABEL 220
#define PAL_CART_RIDGE 221
#define PAL_CART_SLOT  222
#define PAL_WHITE      223
#define PAL_GRAY       224
#define PAL_BG         225

static const uint8_t cube_levels[6] = {0, 51, 102, 153, 204, 255};

static uint8_t rgb555_to_pal(uint16_t p) {
    uint8_t r5 = (p >> 10) & 0x1F;
    uint8_t g5 = (p >> 5) & 0x1F;
    uint8_t b5 = p & 0x1F;
    int ri = (r5 * 5 + 15) / 31;
    int gi = (g5 * 5 + 15) / 31;
    int bi = (b5 * 5 + 15) / 31;
    return (uint8_t)(PAL_CUBE_BASE + ri * 36 + gi * 6 + bi);
}

static void setup_selector_palette(void) {
    graphics_set_palette(PAL_BLACK, 0x000000);

    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                int idx = PAL_CUBE_BASE + r * 36 + g * 6 + b;
                uint32_t rgb = ((uint32_t)cube_levels[r] << 16) |
                               ((uint32_t)cube_levels[g] << 8) |
                               (uint32_t)cube_levels[b];
                graphics_set_palette(idx, rgb);
            }

    graphics_set_palette(PAL_CART_BODY,  0xB0B0B8);
    graphics_set_palette(PAL_CART_LIGHT, 0xC8C8D0);
    graphics_set_palette(PAL_CART_DARK,  0x808088);
    graphics_set_palette(PAL_CART_LABEL, 0x202028);
    graphics_set_palette(PAL_CART_RIDGE, 0x989898);
    graphics_set_palette(PAL_CART_SLOT,  0x505058);
    graphics_set_palette(PAL_WHITE,      0xFFFFFF);
    graphics_set_palette(PAL_GRAY,       0x808080);
    graphics_set_palette(PAL_BG,         0x1A1A22);

    graphics_restore_sync_colors();
}

/* ─── Framebuffer helpers ─────────────────────────────────────────── */

static uint8_t *fb;
static int draw_buf;

static inline void fb_pixel(int x, int y, uint8_t color) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        fb[y * SCREEN_W + x] = color;
}

static void fb_fill(uint8_t color) {
    memset(fb, color, SCREEN_W * SCREEN_H);
}

static void fb_rect(int x, int y, int w, int h, uint8_t color) {
    for (int yy = y; yy < y + h && yy < SCREEN_H; yy++) {
        if (yy < 0) continue;
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
        if (x0 < x1) memset(&fb[yy * SCREEN_W + x0], color, x1 - x0);
    }
}

static void fb_hline(int x, int y, int w, uint8_t color) {
    if (y < 0 || y >= SCREEN_H) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
    if (x0 < x1) memset(&fb[y * SCREEN_W + x0], color, x1 - x0);
}

static void fb_vline(int x, int y, int h, uint8_t color) {
    if (x < 0 || x >= SCREEN_W) return;
    for (int yy = y; yy < y + h && yy < SCREEN_H; yy++)
        if (yy >= 0) fb[yy * SCREEN_W + x] = color;
}

static void present(void) {
    current_buffer = !draw_buf;
    draw_buf ^= 1;
    fb = SCREEN[draw_buf];
}

/* ─── 5x7 font ───────────────────────────────────────────────────── */

static const uint8_t glyphs[][7] = {
    [' '-' '] = {0,0,0,0,0,0,0},
    ['!'-' '] = {0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    ['"'-' '] = {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00},
    ['#'-' '] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    ['$'-' '] = {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    ['%'-' '] = {0x19,0x1A,0x04,0x08,0x0B,0x13,0x00},
    ['&'-' '] = {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
    ['\''-' '] = {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    ['('-' '] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    [')'-' '] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    ['*'-' '] = {0x00,0x04,0x15,0x0E,0x15,0x04,0x00},
    ['+'-' '] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    [','-' '] = {0x00,0x00,0x00,0x00,0x0C,0x04,0x08},
    ['-'-' '] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['.'-' '] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    ['/'-' '] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00},
    ['0'-' '] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'-' '] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'-' '] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'-' '] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    ['4'-' '] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'-' '] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    ['6'-' '] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
    ['7'-' '] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'-' '] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'-' '] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
    [':'-' '] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    [';'-' '] = {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},
    ['<'-' '] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    ['='-' '] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    ['>'-' '] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    ['?'-' '] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    ['@'-' '] = {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
    ['A'-' '] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'-' '] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'-' '] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'-' '] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'-' '] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'-' '] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'-' '] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    ['H'-' '] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'-' '] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},
    ['J'-' '] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
    ['K'-' '] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'-' '] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'-' '] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'-' '] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    ['O'-' '] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'-' '] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'-' '] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'-' '] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'-' '] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'-' '] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'-' '] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'-' '] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    ['W'-' '] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['X'-' '] = {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11},
    ['Y'-' '] = {0x11,0x0A,0x04,0x04,0x04,0x04,0x04},
    ['Z'-' '] = {0x1F,0x02,0x04,0x08,0x10,0x10,0x1F},
    ['['-' '] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    ['\\'-' '] = {0x10,0x08,0x04,0x02,0x01,0x00,0x00},
    [']'-' '] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    ['^'-' '] = {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    ['_'-' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    ['{'-' '] = {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    ['|'-' '] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
    ['}'-' '] = {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    ['~'-' '] = {0x00,0x00,0x08,0x15,0x02,0x00,0x00},
};

static void fb_char(int x, int y, char ch, uint8_t color) {
    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    int idx = c - ' ';
    if (idx < 0 || idx >= (int)(sizeof(glyphs)/sizeof(glyphs[0]))) return;
    const uint8_t *g = glyphs[idx];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1u << (4 - col)))
                fb_pixel(x + col, y + row, color);
        }
    }
}

static void fb_text(int x, int y, const char *s, uint8_t color) {
    for (; *s; s++) { fb_char(x, y, *s, color); x += 6; }
}

static void fb_text_center(int y, const char *s, uint8_t color) {
    int x = (SCREEN_W - (int)strlen(s) * 6) / 2;
    fb_text(x, y, s, color);
}

/* ─── CRC32 ───────────────────────────────────────────────────────── */

static uint32_t crc32_table[256];
static bool crc32_ready = false;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
        crc32_table[i] = c;
    }
    crc32_ready = true;
}

static uint32_t crc32_file(FIL *fil, int skip) {
    if (!crc32_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buf[512];
    f_lseek(fil, skip);
    while (1) {
        UINT br;
        if (f_read(fil, buf, sizeof(buf), &br) != FR_OK || br == 0) break;
        for (UINT i = 0; i < br; i++)
            crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ─── ROM list ────────────────────────────────────────────────────── */

#define MAX_ROMS 48

typedef struct {
    char filename[48];
    uint32_t crc;
    bool crc_valid;
} rom_entry_t;

static rom_entry_t *rom_list;  /* allocated in PSRAM */
#define IMG_BUF_BYTES (40 * 1024)
static uint8_t *img_buf;      /* allocated in PSRAM */
static int rom_count = 0;

static bool is_snes_ext(const char *fname) {
    const char *ext = strrchr(fname, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".smc") == 0 ||
            strcasecmp(ext, ".sfc") == 0 ||
            strcasecmp(ext, ".fig") == 0);
}

static int scan_roms(void) {
    rom_count = 0;
    DIR dir;
    if (f_opendir(&dir, "/snes") != FR_OK) {
        if (f_opendir(&dir, "/SNES") != FR_OK) return 0;
    }
    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0' && rom_count < MAX_ROMS) {
        if (fno.fattrib & AM_DIR) continue;
        if (!is_snes_ext(fno.fname)) continue;
        strncpy(rom_list[rom_count].filename, fno.fname, sizeof(rom_list[0].filename) - 1);
        rom_list[rom_count].filename[sizeof(rom_list[0].filename) - 1] = '\0';
        rom_list[rom_count].crc_valid = false;
        rom_count++;
    }
    f_closedir(&dir);
    return rom_count;
}

static void ensure_crc(int idx) {
    if (rom_list[idx].crc_valid) return;
    char path[MAX_ROM_PATH];
    snprintf(path, sizeof(path), "/snes/%s", rom_list[idx].filename);
    FIL fil;
    if (f_open(&fil, path, FA_READ) == FR_OK) {
        /* SNES ROMs may have a 512-byte copier header */
        FSIZE_t sz = f_size(&fil);
        int skip = (sz % 1024 == 512) ? 512 : 0;
        rom_list[idx].crc = crc32_file(&fil, skip);
        rom_list[idx].crc_valid = true;
        f_close(&fil);
        printf("CRC32(%s) = %08lX\n", rom_list[idx].filename, (unsigned long)rom_list[idx].crc);
    }
}

/* ─── CRC cache ───────────────────────────────────────────────────── */

#define CRC_CACHE_PATH "/snes/.crc_cache"
#define LAST_ROM_PATH  "/snes/.last_rom"

static void load_crc_cache(void) {
    FIL fil;
    if (f_open(&fil, CRC_CACHE_PATH, FA_READ) != FR_OK) return;
    char line[128];
    while (f_gets(line, sizeof(line), &fil)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        uint32_t crc = 0;
        for (const char *p = eq + 1; *p && *p != '\n' && *p != '\r'; p++) {
            crc <<= 4;
            if (*p >= '0' && *p <= '9') crc |= (*p - '0');
            else if (*p >= 'A' && *p <= 'F') crc |= (*p - 'A' + 10);
            else if (*p >= 'a' && *p <= 'f') crc |= (*p - 'a' + 10);
        }
        for (int i = 0; i < rom_count; i++) {
            if (strcmp(rom_list[i].filename, line) == 0) {
                rom_list[i].crc = crc;
                rom_list[i].crc_valid = true;
                break;
            }
        }
    }
    f_close(&fil);
}

static void save_crc_cache(void) {
    FIL fil;
    if (f_open(&fil, CRC_CACHE_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    for (int i = 0; i < rom_count; i++) {
        if (!rom_list[i].crc_valid) continue;
        char line[128];
        snprintf(line, sizeof(line), "%s=%08lX\n",
                 rom_list[i].filename, (unsigned long)rom_list[i].crc);
        UINT bw;
        f_write(&fil, line, strlen(line), &bw);
    }
    f_close(&fil);
}

/* ─── Last selected ROM ───────────────────────────────────────────── */

static int last_selected_rom = 0;

static void load_last_rom(void) {
    FIL fil;
    if (f_open(&fil, LAST_ROM_PATH, FA_READ) != FR_OK) return;
    char name[64];
    if (f_gets(name, sizeof(name), &fil)) {
        size_t len = strlen(name);
        while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r'))
            name[--len] = '\0';
        for (int i = 0; i < rom_count; i++) {
            if (strcmp(rom_list[i].filename, name) == 0) {
                last_selected_rom = i;
                break;
            }
        }
    }
    f_close(&fil);
}

static void save_last_rom(int selected) {
    FIL fil;
    if (f_open(&fil, LAST_ROM_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    f_puts(rom_list[selected].filename, &fil);
    f_puts("\n", &fil);
    f_close(&fil);
}

/* ─── Metadata ────────────────────────────────────────────────────── */

typedef struct {
    char title[64];
    char desc[256];
    char year[8];
    char genre[48];
    char players[8];
} rom_meta_t;

static rom_meta_t *rom_meta;  /* allocated in PSRAM */

static void extract_xml_tag(const char *buf, const char *tag, char *dst, int dst_size) {
    dst[0] = '\0';
    char open[32], close[32];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *start = strstr(buf, open);
    if (!start) return;
    start += strlen(open);
    const char *end = strstr(start, close);
    if (!end) return;
    int src_len = end - start;
    int di = 0;
    bool prev_space = false;
    int max_out = dst_size - 4;
    int si;
    for (si = 0; si < src_len && di < max_out; si++) {
        char c = start[si];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') {
            if (!prev_space && di > 0) { dst[di++] = ' '; prev_space = true; }
        } else {
            dst[di++] = c;
            prev_space = false;
        }
    }
    if (si < src_len) {
        while (di > 0 && dst[di - 1] == ' ') di--;
        dst[di++] = '.'; dst[di++] = '.'; dst[di++] = '.';
    }
    dst[di] = '\0';
}

static void load_rom_title(int idx) {
    memset(&rom_meta[idx], 0, sizeof(rom_meta[idx]));
    if (!rom_list[idx].crc_valid) return;
    uint32_t crc = rom_list[idx].crc;
    char hex_char = "0123456789ABCDEF"[(crc >> 28) & 0xF];

    char path[128];
    snprintf(path, sizeof(path), "/snes/metadata/descr/%c/%08lX.txt", hex_char, (unsigned long)crc);
    printf("[SEL] meta %s CRC=%08lX path=%s\n", rom_list[idx].filename, (unsigned long)crc, path);
    FIL fil;
    if (f_open(&fil, path, FA_READ) == FR_OK) {
        /* Reuse img_buf (not in use during metadata loading) */
        char *buf = (char *)img_buf;
        int buf_size = 1536;
        UINT br;
        if (f_read(&fil, buf, buf_size - 1, &br) == FR_OK) {
            buf[br] = '\0';
            extract_xml_tag(buf, "name", rom_meta[idx].title, sizeof(rom_meta[idx].title));
            extract_xml_tag(buf, "desc", rom_meta[idx].desc, sizeof(rom_meta[idx].desc));
            extract_xml_tag(buf, "genre", rom_meta[idx].genre, sizeof(rom_meta[idx].genre));
            extract_xml_tag(buf, "players", rom_meta[idx].players, sizeof(rom_meta[idx].players));
            char datestr[32];
            extract_xml_tag(buf, "releasedate", datestr, sizeof(datestr));
            if (datestr[0] && strlen(datestr) >= 4) {
                memcpy(rom_meta[idx].year, datestr, 4);
                rom_meta[idx].year[4] = '\0';
            }
        }
        f_close(&fil);
    }
}

/* ─── Cover art image ─────────────────────────────────────────────── */

static uint16_t cur_img_w, cur_img_h;
static uint16_t *cur_img_pixels;
static int cur_img_idx = -1;

static void load_rom_image(int idx) {
    cur_img_pixels = NULL;
    cur_img_w = 0;
    cur_img_h = 0;
    cur_img_idx = idx;

    if (!rom_list[idx].crc_valid) return;
    uint32_t crc = rom_list[idx].crc;
    char hex_char = "0123456789ABCDEF"[(crc >> 28) & 0xF];

    char path[128];
    snprintf(path, sizeof(path), "/snes/metadata/images/%c/%08lX.555", hex_char, (unsigned long)crc);

    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        printf("[SEL] image not found: %s\n", path);
        return;
    }
    printf("[SEL] image found: %s\n", path);

    uint8_t hdr[4];
    UINT br;
    if (f_read(&fil, hdr, 4, &br) != FR_OK || br != 4) { f_close(&fil); return; }

    uint16_t w = hdr[0] | (hdr[1] << 8);
    uint16_t h = hdr[2] | (hdr[3] << 8);
    if (w == 0 || w > 320 || h == 0 || h > 240) { f_close(&fil); return; }

    uint32_t data_size = (uint32_t)w * h * 2;
    if (data_size > IMG_BUF_BYTES) {
        printf("[SEL] image too large: %lux%lu = %lu bytes (max %d)\n",
               (unsigned long)w, (unsigned long)h, (unsigned long)data_size, IMG_BUF_BYTES);
        f_close(&fil); return;
    }

    if (f_read(&fil, img_buf, data_size, &br) == FR_OK && br == data_size) {
        cur_img_pixels = (uint16_t *)img_buf;
        cur_img_w = w;
        cur_img_h = h;
        printf("[SEL] image %dx%d, first pixels: %04X %04X %04X %04X\n",
               w, h, cur_img_pixels[0], cur_img_pixels[1], cur_img_pixels[2], cur_img_pixels[3]);
    } else {
        printf("[SEL] image read failed (wanted %lu, got %lu)\n", (unsigned long)data_size, (unsigned long)br);
    }
    f_close(&fil);
}

/* ─── SNES cartridge rendering ────────────────────────────────────── */
/*
 * SNES cart (landscape, wider than tall):
 *   ┌──────────────────────────────┐  ← rounded top corners
 *   │  ┌────────────────────────┐  │
 *   │  │                        │  │  ← label / cover art
 *   │  │       LABEL AREA       │  │
 *   │  │                        │  │
 *   │  └────────────────────────┘  │
 *   │                              │
 *   │      ┌────────────────┐      │  ← connector grooves
 *   │      │ ══════════════ │      │
 *   │      └────────────────┘      │
 *   └──────────────────────────────┘
 */

#define CART_W    170
#define CART_H    130
#define CART_X    ((SCREEN_W - CART_W) / 2)
#define CART_Y    8

/* Label area */
#define LABEL_MARGIN_X 12
#define LABEL_MARGIN_Y 7
#define LABEL_H   78

/* Connector groove (bottom center) */
#define GROOVE_W  100
#define GROOVE_H  16

static void draw_cart_at(int cx, int cy, int rom_idx) {
    int label_x = cx + LABEL_MARGIN_X;
    int label_y = cy + LABEL_MARGIN_Y;
    int label_w = CART_W - LABEL_MARGIN_X * 2;
    int label_h = LABEL_H;

    int groove_x = cx + (CART_W - GROOVE_W) / 2;
    int groove_y = cy + CART_H - GROOVE_H - 6;

    /* Main body */
    fb_rect(cx, cy, CART_W, CART_H, PAL_CART_BODY);

    /* Rounded top corners */
    fb_rect(cx, cy, 2, 1, PAL_BG);
    fb_rect(cx, cy, 1, 2, PAL_BG);
    fb_rect(cx + CART_W - 2, cy, 2, 1, PAL_BG);
    fb_rect(cx + CART_W - 1, cy, 1, 2, PAL_BG);

    /* 3D edges — light top/left, dark bottom/right */
    fb_hline(cx + 2, cy, CART_W - 4, PAL_CART_LIGHT);
    fb_vline(cx, cy + 2, CART_H - 2, PAL_CART_LIGHT);
    fb_hline(cx + 1, cy + CART_H - 1, CART_W - 2, PAL_CART_DARK);
    fb_vline(cx + CART_W - 1, cy + 2, CART_H - 2, PAL_CART_DARK);

    /* Label border (inset) */
    fb_rect(label_x - 1, label_y - 1, label_w + 2, label_h + 2, PAL_CART_DARK);

    /* Label fill */
    bool has_img = (cur_img_pixels && cur_img_idx == rom_idx);
    fb_rect(label_x, label_y, label_w, label_h, has_img ? PAL_CART_LABEL : PAL_CART_SLOT);

    /* Cover art */
    if (has_img) {
        int iw = cur_img_w;
        int ih = cur_img_h;
        /* Scale to fit label, preserving aspect ratio */
        if (iw * label_h > ih * label_w) {
            ih = ih * label_w / iw;
            iw = label_w;
        } else {
            iw = iw * label_h / ih;
            ih = label_h;
        }
        int ix = label_x + (label_w - iw) / 2;
        int iy = label_y + (label_h - ih) / 2;
        for (int y = 0; y < ih; y++) {
            int sy = y * cur_img_h / ih;
            for (int x = 0; x < iw; x++) {
                int sx = x * cur_img_w / iw;
                uint16_t px = cur_img_pixels[sy * cur_img_w + sx];
                fb_pixel(ix + x, iy + y, rgb555_to_pal(px));
            }
        }
    } else {
        /* No image: subtle inset border */
        int m = 6;
        fb_hline(label_x + m, label_y + m, label_w - m * 2, PAL_CART_DARK);
        fb_hline(label_x + m, label_y + label_h - m - 1, label_w - m * 2, PAL_CART_DARK);
        fb_vline(label_x + m, label_y + m, label_h - m * 2, PAL_CART_DARK);
        fb_vline(label_x + label_w - m - 1, label_y + m, label_h - m * 2, PAL_CART_DARK);
    }

    /* Top notches (2 small lines on top edge) */
    int notch1_x = cx + CART_W / 2 - 16;
    int notch2_x = cx + CART_W / 2 + 14;
    fb_vline(notch1_x, cy, 3, PAL_CART_DARK);
    fb_vline(notch2_x, cy, 3, PAL_CART_DARK);

    /* Corner ridges — 5 small horizontal lines on top-left and top-right */
    for (int i = 0; i < 5; i++) {
        int ry = cy + 3 + i * 3;
        fb_hline(cx + 2, ry, 5, PAL_CART_DARK);
        fb_hline(cx + CART_W - 7, ry, 5, PAL_CART_DARK);
    }

    /* Screws on bottom-left and bottom-right */
    int screw_y = cy + CART_H - 10;
    int screw_lx = cx + 10;
    int screw_rx = cx + CART_W - 13;
    /* Left screw */
    fb_rect(screw_lx, screw_y, 3, 3, PAL_CART_DARK);
    fb_pixel(screw_lx + 1, screw_y + 1, PAL_CART_LIGHT);
    /* Right screw */
    fb_rect(screw_rx, screw_y, 3, 3, PAL_CART_DARK);
    fb_pixel(screw_rx + 1, screw_y + 1, PAL_CART_LIGHT);

    /* Connector groove area */
    fb_rect(groove_x, groove_y, GROOVE_W, GROOVE_H, PAL_CART_RIDGE);
    /* Groove border */
    fb_hline(groove_x, groove_y, GROOVE_W, PAL_CART_DARK);
    fb_hline(groove_x, groove_y + GROOVE_H - 1, GROOVE_W, PAL_CART_LIGHT);
    fb_vline(groove_x, groove_y, GROOVE_H, PAL_CART_DARK);
    fb_vline(groove_x + GROOVE_W - 1, groove_y, GROOVE_H, PAL_CART_LIGHT);
    /* Horizontal ridges inside groove */
    for (int i = 0; i < 3; i++) {
        int ry = groove_y + 3 + i * 4;
        fb_hline(groove_x + 4, ry,     GROOVE_W - 8, PAL_CART_DARK);
        fb_hline(groove_x + 4, ry + 1, GROOVE_W - 8, PAL_CART_LIGHT);
    }
}

/* ─── Info panel ──────────────────────────────────────────────────── */

typedef enum {
    INFO_HIDDEN, INFO_SLIDING_IN, INFO_SHOWN, INFO_SLIDING_OUT,
} info_state_t;

#define INFO_ANIM_FRAMES 10
static info_state_t info_state = INFO_HIDDEN;
static int info_anim_frame = 0;
#define INFO_CART_X (-(CART_W * 70 / 100))

static int info_ease(int frame) {
    int t = frame * 256 / INFO_ANIM_FRAMES;
    return t * (512 - t) / 256;
}

static int info_cart_x(void) {
    switch (info_state) {
    case INFO_HIDDEN: return CART_X;
    case INFO_SLIDING_IN: return CART_X + (INFO_CART_X - CART_X) * info_ease(info_anim_frame) / 256;
    case INFO_SHOWN: return INFO_CART_X;
    case INFO_SLIDING_OUT: return INFO_CART_X + (CART_X - INFO_CART_X) * info_ease(info_anim_frame) / 256;
    }
    return CART_X;
}

static int fb_text_wrap(int x, int y, int max_w, const char *s, uint8_t color, int max_lines) {
    int max_chars = max_w / 6;
    if (max_chars < 1) max_chars = 1;
    int line = 0;
    const char *p = s;
    while (*p && line < max_lines) {
        int len = (int)strlen(p);
        if (len <= max_chars) { fb_text(x, y, p, color); break; }
        int brk = max_chars;
        for (int i = max_chars; i > 0; i--) {
            if (p[i] == ' ') { brk = i; break; }
        }
        char tmp[64];
        int copy = brk > 63 ? 63 : brk;
        memcpy(tmp, p, copy);
        tmp[copy] = '\0';
        if (line == max_lines - 1 && (int)strlen(p) > brk) {
            if (copy > 3) copy -= 3;
            tmp[copy] = '.'; tmp[copy+1] = '.'; tmp[copy+2] = '.'; tmp[copy+3] = '\0';
        }
        fb_text(x, y, tmp, color);
        y += 9;
        line++;
        p += brk;
        while (*p == ' ') p++;
    }
    return y;
}

static void draw_info_panel(int selected) {
    int text_x = INFO_CART_X + CART_W + 8;
    int text_w = SCREEN_W - text_x - 6;
    int ty = CART_Y + 4;

    const char *title = rom_meta[selected].title[0] ? rom_meta[selected].title : rom_list[selected].filename;
    char dt[40];
    int max_c = text_w / 6;
    if (max_c > 39) max_c = 39;
    int tlen = (int)strlen(title);
    if (tlen > max_c) {
        int cut = max_c > 3 ? max_c - 3 : 0;
        memcpy(dt, title, cut);
        dt[cut] = '.'; dt[cut+1] = '.'; dt[cut+2] = '.'; dt[cut+3] = '\0';
    } else {
        memcpy(dt, title, tlen); dt[tlen] = '\0';
    }
    fb_text(text_x, ty, dt, PAL_WHITE);
    ty += 14;

    fb_hline(text_x, ty, text_w, PAL_CART_RIDGE);
    ty += 6;

    if (rom_meta[selected].year[0]) {
        char line[48];
        snprintf(line, sizeof(line), "YEAR: %s", rom_meta[selected].year);
        fb_text(text_x, ty, line, PAL_GRAY);
        ty += 12;
    }
    if (rom_meta[selected].players[0]) {
        char line[48];
        snprintf(line, sizeof(line), "PLAYERS: %s", rom_meta[selected].players);
        fb_text(text_x, ty, line, PAL_GRAY);
        ty += 12;
    }
    if (rom_meta[selected].genre[0]) {
        char gline[48];
        int glen = (int)strlen(rom_meta[selected].genre);
        int gmax = text_w / 6;
        if (gmax > 47) gmax = 47;
        if (glen > gmax) {
            memcpy(gline, rom_meta[selected].genre, gmax - 3);
            gline[gmax-3] = '.'; gline[gmax-2] = '.'; gline[gmax-1] = '.';
            gline[gmax] = '\0';
        } else {
            strncpy(gline, rom_meta[selected].genre, 47);
            gline[47] = '\0';
        }
        fb_text(text_x, ty, gline, PAL_GRAY);
        ty += 12;
    }
    if (rom_meta[selected].desc[0]) {
        ty += 4;
        int max_desc_lines = (CART_Y + CART_H + 10 - ty) / 9;
        if (max_desc_lines > 12) max_desc_lines = 12;
        if (max_desc_lines > 0)
            fb_text_wrap(text_x, ty, text_w, rom_meta[selected].desc, PAL_GRAY, max_desc_lines);
    }
}

/* ─── Animation ───────────────────────────────────────────────────── */

static int bounce_offset(uint32_t frame) {
    int t = (int)(frame % 36);
    if (t < 6) return 0;
    if (t < 9) return 1;
    if (t < 15) return 2;
    if (t < 18) return 1;
    if (t < 24) return 0;
    if (t < 27) return -1;
    if (t < 33) return -2;
    return -1;
}

#define SCROLL_FRAMES 8
static int scroll_dir = 0;
static int scroll_frame = 0;
static int scroll_from = 0;

static int ease_out(int frame) {
    int t = frame * 256 / SCROLL_FRAMES;
    return t * (512 - t) / 256;
}

static void draw_selector_text(int selected) {
    bool info_visible = (info_state != INFO_HIDDEN);

    if (!info_visible) {
        const char *title = rom_meta[selected].title[0] ? rom_meta[selected].title : rom_list[selected].filename;
        char dt[40];
        int max_c = (SCREEN_W - 20) / 6;
        if (max_c > 39) max_c = 39;
        int tlen = (int)strlen(title);
        if (tlen > max_c) {
            int cut = max_c > 3 ? max_c - 3 : 0;
            memcpy(dt, title, cut);
            dt[cut] = '.'; dt[cut+1] = '.'; dt[cut+2] = '.'; dt[cut+3] = '\0';
        } else {
            memcpy(dt, title, tlen); dt[tlen] = '\0';
        }
        fb_text_center(CART_Y + CART_H + 10, dt, PAL_WHITE);

        char counter[16];
        snprintf(counter, sizeof(counter), "%d / %d", selected + 1, rom_count);
        fb_text_center(CART_Y + CART_H + 24, counter, PAL_GRAY);
    }

    if (info_state == INFO_SHOWN)
        fb_text_center(SCREEN_H - 14, "< DOWN >   A: START", PAL_GRAY);
    else if (info_state == INFO_HIDDEN)
        fb_text_center(SCREEN_H - 14, "< LEFT/RIGHT/UP >   A: START", PAL_GRAY);
}

static void draw_scene(int selected, uint32_t frame_count) {
    fb_fill(PAL_BG);

    if (scroll_dir != 0 && scroll_frame < SCROLL_FRAMES) {
        int progress = ease_out(scroll_frame);
        int travel = (SCREEN_W / 2 + CART_W);
        int out_x = CART_X + (-scroll_dir * travel * progress / 256);
        int in_x  = CART_X + (scroll_dir * travel * (256 - progress) / 256);
        draw_cart_at(out_x, CART_Y, scroll_from);
        draw_cart_at(in_x, CART_Y, selected);
    } else if (info_state != INFO_HIDDEN) {
        int cx = info_cart_x();
        draw_cart_at(cx, CART_Y, selected);
        if (info_state == INFO_SHOWN)
            draw_info_panel(selected);
    } else {
        int by = bounce_offset(frame_count);
        draw_cart_at(CART_X, CART_Y + by, selected);
    }

    draw_selector_text(selected);
}

/* ─── Input ───────────────────────────────────────────────────────── */

#define BTN_LEFT  0x01
#define BTN_RIGHT 0x02
#define BTN_A     0x04
#define BTN_START 0x08
#define BTN_UP    0x10
#define BTN_DOWN  0x20
#define BTN_SEL   0x40
#define BTN_F12   0x80

static int read_selector_buttons(void) {
    nespad_read();
    ps2kbd_tick();
    int buttons = 0;
    uint32_t pad = nespad_state | nespad_state2;
    if (pad & DPAD_LEFT)   buttons |= BTN_LEFT;
    if (pad & DPAD_RIGHT)  buttons |= BTN_RIGHT;
    if (pad & DPAD_UP)     buttons |= BTN_UP;
    if (pad & DPAD_DOWN)   buttons |= BTN_DOWN;
    if (pad & DPAD_A)      buttons |= BTN_A;
    if (pad & DPAD_START)  buttons |= BTN_START;
    if (pad & DPAD_SELECT) buttons |= BTN_SEL;
    uint16_t kbd = ps2kbd_get_state();
#ifdef USB_HID_ENABLED
    kbd |= usbhid_get_kbd_state();
#endif
    if (kbd & KBD_STATE_LEFT)  buttons |= BTN_LEFT;
    if (kbd & KBD_STATE_RIGHT) buttons |= BTN_RIGHT;
    if (kbd & KBD_STATE_UP)    buttons |= BTN_UP;
    if (kbd & KBD_STATE_DOWN)  buttons |= BTN_DOWN;
    if (kbd & KBD_STATE_A)     buttons |= BTN_A;
    if (kbd & KBD_STATE_START) buttons |= BTN_START;
    if (kbd & KBD_STATE_F12)   buttons |= BTN_F12;
    if (kbd & KBD_STATE_ESC)   buttons |= BTN_F12;
#ifdef USB_HID_ENABLED
    usbhid_task();
    if (usbhid_gamepad_connected()) {
        usbhid_gamepad_state_t gp;
        usbhid_get_gamepad_state(&gp);
        if (gp.dpad & 0x01) buttons |= BTN_UP;
        if (gp.dpad & 0x02) buttons |= BTN_DOWN;
        if (gp.dpad & 0x04) buttons |= BTN_LEFT;
        if (gp.dpad & 0x08) buttons |= BTN_RIGHT;
        if (gp.buttons & 0x01) buttons |= BTN_A;
        if (gp.buttons & 0x02) buttons |= BTN_A;
        if (gp.buttons & 0x40) buttons |= BTN_START;
        if (gp.buttons & 0x80) buttons |= BTN_SEL;
    }
#endif
    return buttons;
}

/* ─── Main selector ───────────────────────────────────────────────── */

bool rom_selector_show(char *selected_rom_path, size_t buffer_size, uint8_t *screen_buffer) {
    (void)screen_buffer;

    /* Allocate large buffers in PSRAM (reset to reclaim any prior session) */
    psram_reset();
    rom_list = (rom_entry_t *)psram_malloc(MAX_ROMS * sizeof(rom_entry_t));
    rom_meta = (rom_meta_t *)psram_malloc(MAX_ROMS * sizeof(rom_meta_t));
    img_buf = (uint8_t *)psram_malloc(IMG_BUF_BYTES);
    memset(rom_list, 0, MAX_ROMS * sizeof(rom_entry_t));
    memset(rom_meta, 0, MAX_ROMS * sizeof(rom_meta_t));

    /* Set up palette and show loading screen immediately */
    printf("[SEL] palette setup\n");
    setup_selector_palette();
    draw_buf = 0;
    fb = SCREEN[draw_buf];
    fb_fill(PAL_BG);
    fb_text_center(SCREEN_H / 2 - 4, "LOADING...", PAL_WHITE);
    present();           /* show SCREEN[0], set draw_buf=1 */
    sleep_ms(100);       /* let TV sync */
    printf("[SEL] loading screen shown\n");

    /* Scan ROMs (SD access may take time) */
    scan_roms();
    printf("[SEL] found %d ROMs\n", rom_count);
    if (rom_count == 0) return false;

    /* CRC cache */
    printf("[SEL] loading CRC cache\n");
    load_crc_cache();
    bool cache_dirty = false;
    for (int i = 0; i < rom_count; i++) {
        if (!rom_list[i].crc_valid) {
            printf("[SEL] computing CRC for %s\n", rom_list[i].filename);
            ensure_crc(i);
            cache_dirty = true;
        }
    }
    if (cache_dirty) {
        printf("[SEL] saving CRC cache\n");
        save_crc_cache();
    }

    /* Load metadata */
    printf("[SEL] loading metadata\n");
    for (int i = 0; i < rom_count; i++)
        load_rom_title(i);

    printf("[SEL] loading last ROM\n");
    load_last_rom();

    int selected = last_selected_rom;
    if (selected >= rom_count) selected = 0;

    int prev_buttons = read_selector_buttons();
    uint32_t hold_counter = 0;
    uint32_t frame_count = 0;
    cur_img_idx = -1;
    scroll_dir = 0;
    scroll_frame = 0;
    info_state = INFO_HIDDEN;
    info_anim_frame = 0;

    printf("[SEL] loading initial image\n");
    load_rom_image(selected);
    printf("[SEL] entering main loop\n");

    while (1) {
        draw_scene(selected, frame_count);
        present();
        frame_count++;
        sleep_ms(16);

        /* Advance scroll animation */
        if (scroll_dir != 0) {
            scroll_frame++;
            if (scroll_frame >= SCROLL_FRAMES) {
                scroll_dir = 0;
                scroll_frame = 0;
            }
        }

        /* Advance info panel animation */
        if (info_state == INFO_SLIDING_IN) {
            info_anim_frame++;
            if (info_anim_frame >= INFO_ANIM_FRAMES) {
                info_state = INFO_SHOWN;
                info_anim_frame = 0;
            }
        } else if (info_state == INFO_SLIDING_OUT) {
            info_anim_frame++;
            if (info_anim_frame >= INFO_ANIM_FRAMES) {
                info_state = INFO_HIDDEN;
                info_anim_frame = 0;
            }
        }

        /* Input */
        int buttons = read_selector_buttons();
        int pressed = buttons & ~prev_buttons;
        if (buttons != 0 && buttons == prev_buttons) {
            hold_counter++;
            if (hold_counter > 20 && (hold_counter % 5) == 0)
                pressed = buttons;
        } else {
            hold_counter = 0;
        }
        prev_buttons = buttons;

        /* Settings hotkey: Start+Select or F12 */
        bool settings_hotkey = ((buttons & BTN_START) && (buttons & BTN_SEL)) || (buttons & BTN_F12);
        if (settings_hotkey) {
            /* Wait for release */
            for (int w = 0; w < 60; w++) {
                int b = read_selector_buttons();
                if (b == 0) break;
                sleep_ms(16);
            }

            settings_menu_show(SCREEN[0], false);

            /* Restore selector */
            setup_selector_palette();
            draw_buf = 0;
            fb = SCREEN[draw_buf];
            cur_img_idx = -1;
            load_rom_image(selected);
            prev_buttons = read_selector_buttons();
            continue;
        }

        /* Info panel: UP opens, DOWN closes */
        if (info_state == INFO_HIDDEN && scroll_dir == 0 && (pressed & BTN_UP)) {
            info_state = INFO_SLIDING_IN;
            info_anim_frame = 0;
        }
        if (info_state == INFO_SHOWN && (pressed & BTN_DOWN)) {
            info_state = INFO_SLIDING_OUT;
            info_anim_frame = 0;
        }

        /* Navigation (only when idle) */
        bool can_navigate = (scroll_dir == 0 && info_state == INFO_HIDDEN);
        if (can_navigate) {
            if (pressed & BTN_LEFT) {
                scroll_from = selected;
                selected = (selected - 1 + rom_count) % rom_count;
                scroll_dir = -1;
                scroll_frame = 0;
                load_rom_image(selected);
            }
            if (pressed & BTN_RIGHT) {
                scroll_from = selected;
                selected = (selected + 1) % rom_count;
                scroll_dir = 1;
                scroll_frame = 0;
                load_rom_image(selected);
            }
        }

        /* Select ROM */
        if (pressed & (BTN_A | BTN_START)) {
            if (!(buttons & BTN_SEL)) {
                /* Close info panel first */
                if (info_state != INFO_HIDDEN) {
                    info_state = INFO_SLIDING_OUT;
                    info_anim_frame = 0;
                    while (info_state == INFO_SLIDING_OUT) {
                        draw_scene(selected, frame_count);
                        present();
                        frame_count++;
                        sleep_ms(16);
                        info_anim_frame++;
                        if (info_anim_frame >= INFO_ANIM_FRAMES) {
                            info_state = INFO_HIDDEN;
                            info_anim_frame = 0;
                        }
                    }
                }
                if (scroll_dir != 0) continue;

                /* Wait for button release */
                for (int w = 0; w < 60; w++) {
                    if (read_selector_buttons() == 0) break;
                    sleep_ms(16);
                }

                save_last_rom(selected);
                snprintf(selected_rom_path, buffer_size, "/snes/%s", rom_list[selected].filename);
                return true;
            }
        }
    }

    return false;
}

/* ─── SD error screen ─────────────────────────────────────────────── */

void rom_selector_show_sd_error(uint8_t *screen_buffer, int error_code) {
    (void)screen_buffer;
    setup_selector_palette();

    draw_buf = 0;
    fb = SCREEN[draw_buf];
    fb_fill(PAL_BG);
    fb_text_center(SCREEN_H / 2 - 20, "SD CARD ERROR", PAL_WHITE);
    fb_text_center(SCREEN_H / 2, "NO SD CARD DETECTED", PAL_GRAY);

    char code_str[32];
    snprintf(code_str, sizeof(code_str), "ERROR CODE: %d", error_code);
    fb_text_center(SCREEN_H / 2 + 16, code_str, PAL_GRAY);
    fb_text_center(SCREEN_H / 2 + 36, "INSERT FAT32 SD CARD", PAL_WHITE);
    fb_text_center(SCREEN_H / 2 + 48, "AND RESET THE DEVICE", PAL_WHITE);
    present();

    while (1) { tight_loop_contents(); }
}
