#include "board_config.h"
#include "HDMI.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdalign.h>
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"

// Globals expected by the driver - placed in scratch memory for fast ISR access
int graphics_buffer_width = 320;
int graphics_buffer_height = 240;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;
enum graphics_mode_t hdmi_graphics_mode = GRAPHICSMODE_DEFAULT;

// Graphics buffer pointer in scratch memory for fast DMA handler access
static uint8_t * __scratch_y("hdmi_ptr") graphics_buffer = NULL;

void graphics_set_buffer(uint8_t *buffer) {
    graphics_buffer = buffer;
}

uint8_t* graphics_get_buffer(void) {
    return graphics_buffer;
}

uint32_t graphics_get_width(void) {
    return graphics_buffer_width;
}

uint32_t graphics_get_height(void) {
    return graphics_buffer_height;
}

void graphics_set_res(int w, int h) {
    graphics_buffer_width = w;
    graphics_buffer_height = h;
}

void graphics_set_shift(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

uint8_t* get_line_buffer(int line) {
    if (!graphics_buffer) return NULL;
    if (line < 0 || line >= graphics_buffer_height) return NULL;
    return graphics_buffer + line * graphics_buffer_width;
}

static struct video_mode_t video_mode[] = {
    { // 640x480 60Hz
        .h_total = 524,
        .h_width = 480,
        .freq = 60,
        .vgaPxClk = 25175000
    }
};

struct video_mode_t graphics_get_video_mode(int mode) {
    return video_mode[0];
}

int get_video_mode() {
    return 0;
}

void vsync_handler() {
    // Optional: Add vsync callback if needed
}

// --- New HDMI Driver Code ---

//PIO параметры
static uint offs_prg0 = 0;
static uint offs_prg1 = 0;

//SM
static int SM_video = -1;
static int SM_conv = -1;

//буфер  палитры 256 цветов в формате R8G8B8
static uint32_t palette[256];

// Substitute map for HDMI reserved sync-control indices (BASE_HDMI_CTRL_INX..BASE_HDMI_CTRL_INX+3)
static uint8_t hdmi_color_substitute[4] = {0, 0, 0, 0};

// Assembly-optimized scanline copy function
extern void hdmi_copy_scanline_asm(uint8_t* dst, const uint16_t* src, uint32_t count, const uint8_t* subst);
extern void hdmi_memset_fast(uint8_t* dst, uint8_t val, uint32_t count);

// Palette update flag - set by emulator, checked during vblank
static volatile bool palette_dirty = false;
static volatile bool full_palette_update_pending = false;  // Request full re-conversion
static void apply_pending_palette(void);  // Forward declaration
void graphics_convert_all_palette(void);  // Convert all palette to TMDS

// HDMI sync control indices start at 251


#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)

// #define HDMI_WIDTH 480 //480 Default
// #define HDMI_HEIGHT 644 //524 Default
// #define HDMI_HZ 52 //60 Default

//DMA каналы
//каналы работы с первичным графическим буфером
static int dma_chan_ctrl;
static int dma_chan;
//каналы работы с конвертацией палитры
static int dma_chan_pal_conv_ctrl;
static int dma_chan_pal_conv;

//DMA буферы - placed in scratch memory for fast ISR access
//основные строчные данные
static uint32_t * __scratch_y("hdmi_ptr_3") dma_lines[2] = { NULL,NULL };
static uint32_t * __scratch_y("hdmi_ptr_4") DMA_BUF_ADDR[2];

//ДМА палитра для конвертации
//в хвосте этой памяти выделяется dma_data
alignas(4096) uint32_t conv_color[1224];

//индекс, проверяющий зависание
static uint32_t irq_inx = 0;

// External screen buffer and double-buffer index from main.c
extern uint16_t SCREEN[2][256 * 239];
extern volatile uint32_t current_buffer;

//функции и константы HDMI

#define BASE_HDMI_CTRL_INX (251)
//программа конвертации адреса

static inline uint32_t rgb_dist2(uint32_t a, uint32_t b) {
    int dr = (int)((a >> 16) & 0xff) - (int)((b >> 16) & 0xff);
    int dg = (int)((a >> 8) & 0xff) - (int)((b >> 8) & 0xff);
    int db = (int)(a & 0xff) - (int)(b & 0xff);
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

static void hdmi_recompute_color_substitute(void) {
    const int base = BASE_HDMI_CTRL_INX;
    for (int i = 0; i < 4; i++) {
        const uint8_t reserved = (uint8_t)(base + i);
        const uint32_t target = palette[reserved] & 0x00ffffff;

        uint8_t best = 0;
        uint32_t best_d = 0xffffffffu;
        for (int j = 0; j < 256; j++) {
            if (j >= base && j <= base + 3) continue; // don't map to sync-control indices
            const uint32_t cand = palette[j] & 0x00ffffff;
            const uint32_t d = rgb_dist2(target, cand);
            if (d < best_d) {
                best_d = d;
                best = (uint8_t)j;
                if (d == 0) break;
            }
        }

        hdmi_color_substitute[i] = best;
    }
}

uint16_t pio_program_instructions_conv_HDMI[] = {
    //         //     .wrap_target
    0x80a0, //  0: pull   block
    0x40e8, //  1: in     osr, 8
    0x4034, //  2: in     x, 20
    0x8020, //  3: push   block
    //     .wrap
};


const struct pio_program pio_program_conv_addr_HDMI = {
    .instructions = pio_program_instructions_conv_HDMI,
    .length = 4,
    .origin = -1,
};

//программа видеовывода
static const uint16_t instructions_PIO_HDMI[] = {
    0x7006, //  0: out    pins, 6         side 2
    0x7006, //  1: out    pins, 6         side 2
    0x7006, //  2: out    pins, 6         side 2
    0x7006, //  3: out    pins, 6         side 2
    0x7006, //  4: out    pins, 6         side 2
    0x6806, //  5: out    pins, 6         side 1
    0x6806, //  6: out    pins, 6         side 1
    0x6806, //  7: out    pins, 6         side 1
    0x6806, //  8: out    pins, 6         side 1
    0x6806, //  9: out    pins, 6         side 1
};

static const struct pio_program program_PIO_HDMI = {
    .instructions = instructions_PIO_HDMI,
    .length = 10,
    .origin = -1,
};

static uint64_t get_ser_diff_data(const uint16_t dataR, const uint16_t dataG, const uint16_t dataB) {
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;
#ifdef PICO_PC
        uint8_t bG = (dataR >> (9 - i)) & 1;
        uint8_t bR = (dataG >> (9 - i)) & 1;
#else
        uint8_t bR = (dataR >> (9 - i)) & 1;
        uint8_t bG = (dataG >> (9 - i)) & 1;
#endif
        uint8_t bB = (dataB >> (9 - i)) & 1;

        bR |= (bR ^ 1) << 1;
        bG |= (bG ^ 1) << 1;
        bB |= (bB ^ 1) << 1;

        if (HDMI_PIN_invert_diffpairs) {
            bR ^= 0b11;
            bG ^= 0b11;
            bB ^= 0b11;
        }
        uint8_t d6;
        if (HDMI_PIN_RGB_notBGR) {
            d6 = (bR << 4) | (bG << 2) | (bB << 0);
        }
        else {
            d6 = (bB << 4) | (bG << 2) | (bR << 0);
        }


        out64 |= d6;
    }
    return out64;
}

//конвертор TMDS
static uint tmds_encoder(const uint8_t d8) {
    int s1 = 0;
    for (int i = 0; i < 8; i++) s1 += (d8 & (1 << i)) ? 1 : 0;
    bool is_xnor = false;
    if ((s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0))) is_xnor = true;
    uint16_t d_out = d8 & 1;
    uint16_t qi = d_out;
    for (int i = 1; i < 8; i++) {
        d_out |= ((qi << 1) ^ (d8 & (1 << i))) ^ (is_xnor << i);
        qi = d_out & (1 << i);
    }

    if (is_xnor) d_out |= 1 << 9;
    else d_out |= 1 << 8;

    return d_out;
}

static void pio_set_x(PIO pio, const int sm, uint32_t v) {
    uint instr_shift = pio_encode_in(pio_x, 4);
    uint instr_mov = pio_encode_mov(pio_x, pio_isr);
    for (int i = 0; i < 8; i++) {
        const uint32_t nibble = (v >> (i * 4)) & 0xf;
        pio_sm_exec(pio, sm, pio_encode_set(pio_x, nibble));
        pio_sm_exec(pio, sm, instr_shift);
    }
    pio_sm_exec(pio, sm, instr_mov);
}

static void __scratch_y("hdmi_driver") dma_handler_HDMI() {
    static uint32_t inx_buf_dma;
    static uint line = 0;
    irq_inx++;

    dma_hw->ints0 = 1u << dma_chan_ctrl;
    dma_channel_set_read_addr(dma_chan_ctrl, &DMA_BUF_ADDR[inx_buf_dma & 1], false);

    // Increment line counter with wrap at 524 (same as pico-snes-master)
    line = line >= 524 ? 0 : line + 1;

    if ((line & 1) == 0) return;
    inx_buf_dma++;

    uint8_t* activ_buf = (uint8_t *)dma_lines[inx_buf_dma & 1];

    if (line < (239*2) ) {
        // Active video region - render SNES screen
        uint8_t* output_buffer = activ_buf + 72; // Align sync
        
        // Fill left margin (32 pixels for centering 256 in 320)
        hdmi_memset_fast(output_buffer, 0, 32);
        output_buffer += 32;
        
        // Read from the back buffer (not currently being drawn to)
        const uint16_t* input = (uint16_t*)&SCREEN[!current_buffer][(line / 2) * graphics_buffer_width];
        
        // Copy pixels using optimized assembly routine
        hdmi_copy_scanline_asm(output_buffer, input, graphics_buffer_width, hdmi_color_substitute);
        output_buffer += graphics_buffer_width;
        
        // Fill right margin
        hdmi_memset_fast(output_buffer, 0, 32);


        // memset(activ_buf,2,320);//test

        //ССИ
        //для выравнивания синхры

        // --|_|---|_|---|_|----
        //---|___________|-----
        hdmi_memset_fast(activ_buf + 48, BASE_HDMI_CTRL_INX, 24);
        hdmi_memset_fast(activ_buf, BASE_HDMI_CTRL_INX + 1, 48);
        hdmi_memset_fast(activ_buf + 392, BASE_HDMI_CTRL_INX, 8);

        //без выравнивания
        // --|_|---|_|---|_|----
        //------|___________|----
        //   memset(activ_buf+320,BASE_HDMI_CTRL_INX,8);
        //   memset(activ_buf+328,BASE_HDMI_CTRL_INX+1,48);
        //   memset(activ_buf+376,BASE_HDMI_CTRL_INX,24);
    }
    else {
        // VBlank area - apply pending palette at start of vblank
        if (line == (239*2 + 1)) {
            apply_pending_palette();
        }
        
        if ((line >= 490) && (line < 492)) {
            //кадровый синхроимпульс
            //для выравнивания синхры
            // --|_|---|_|---|_|----
            //---|___________|-----
            hdmi_memset_fast(activ_buf + 48, BASE_HDMI_CTRL_INX + 2, 352);
            hdmi_memset_fast(activ_buf, BASE_HDMI_CTRL_INX + 3, 48);
            //без выравнивания
            // --|_|---|_|---|_|----
            //-------|___________|----

            // memset(activ_buf,BASE_HDMI_CTRL_INX+2,328);
            // memset(activ_buf+328,BASE_HDMI_CTRL_INX+3,48);
            // memset(activ_buf+376,BASE_HDMI_CTRL_INX+2,24);
        }
        else {
            //ССИ без изображения
            //для выравнивания синхры

            hdmi_memset_fast(activ_buf + 48, BASE_HDMI_CTRL_INX, 352);
            hdmi_memset_fast(activ_buf, BASE_HDMI_CTRL_INX + 1, 48);

            // memset(activ_buf,BASE_HDMI_CTRL_INX,328);
            // memset(activ_buf+328,BASE_HDMI_CTRL_INX+1,48);
            // memset(activ_buf+376,BASE_HDMI_CTRL_INX,24);
        };
    }


    // y=(y==524)?0:(y+1);
    // inx_buf_dma++;
}


static inline void irq_remove_handler_DMA_core1() {
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    irq_remove_handler(VIDEO_DMA_IRQ, irq_get_exclusive_handler(VIDEO_DMA_IRQ));
}

static inline void irq_set_exclusive_handler_DMA_core1() {
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_handler_HDMI);
    irq_set_priority(VIDEO_DMA_IRQ, 0);  // Highest priority for HDMI
    irq_set_enabled(VIDEO_DMA_IRQ, true);
}

void graphics_set_palette_hdmi(const uint8_t i, const uint32_t color888);

//деинициализация - инициализация ресурсов
static inline bool hdmi_init() {
    //выключение прерывания DMA
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(dma_chan_ctrl, false);
    }
    else {
        dma_channel_set_irq1_enabled(dma_chan_ctrl, false);
    }

    irq_remove_handler_DMA_core1();


    //остановка всех каналов DMA
    dma_hw->abort = (1 << dma_chan_ctrl) | (1 << dma_chan) | (1 << dma_chan_pal_conv) | (
                        1 << dma_chan_pal_conv_ctrl);
    while (dma_hw->abort) tight_loop_contents();

    //выключение SM основной и конвертора

#if ZERO2
    pio_set_gpio_base(PIO_VIDEO, 16);
    pio_set_gpio_base(PIO_VIDEO_ADDR, 16);
#endif

    // pio_sm_restart(PIO_VIDEO, SM_video);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, false);

    //pio_sm_restart(PIO_VIDEO_ADDR, SM_conv);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, false);


    //удаление программ из соответствующих PIO
    pio_remove_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI, offs_prg1);
    pio_remove_program(PIO_VIDEO, &program_PIO_HDMI, offs_prg0);


    offs_prg1 = pio_add_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI);
    offs_prg0 = pio_add_program(PIO_VIDEO, &program_PIO_HDMI);
    pio_set_x(PIO_VIDEO_ADDR, SM_conv, ((uint32_t)conv_color >> 12));

    // Initialize palette conversion (skip HDMI sync-control indices, but initialize all others)
    for (int ci = 0; ci < BASE_HDMI_CTRL_INX; ci++) graphics_set_palette_hdmi(ci, palette[ci]);
    for (int ci = BASE_HDMI_CTRL_INX + 4; ci < 256; ci++) {
        if (palette[ci] == 0) palette[ci] = 0x000000;
        graphics_set_palette_hdmi(ci, palette[ci]);
    }

    //240-243 служебные данные(синхра) напрямую вносим в массив -конвертер
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;
    const int base_inx = BASE_HDMI_CTRL_INX;

    conv_color64[2 * base_inx + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * base_inx + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * (base_inx + 1) + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * (base_inx + 1) + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * (base_inx + 2) + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * (base_inx + 2) + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * (base_inx + 3) + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * (base_inx + 3) + 1] = get_ser_diff_data(b0, b0, b0);

    //настройка PIO SM для конвертации

    pio_sm_config c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg1, offs_prg1 + (pio_program_conv_addr_HDMI.length - 1));
    sm_config_set_in_shift(&c_c, true, false, 32);

    pio_sm_init(PIO_VIDEO_ADDR, SM_conv, offs_prg1, &c_c);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, true);

    //настройка PIO SM для вывода данных
    c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg0, offs_prg0 + (program_PIO_HDMI.length - 1));

    //настройка side set
    sm_config_set_sideset_pins(&c_c,beginHDMI_PIN_clk);
    sm_config_set_sideset(&c_c, 2,false,false);
    for (int i = 0; i < 2; i++) {
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_clk + i);
        gpio_set_drive_strength(beginHDMI_PIN_clk + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_clk + i, GPIO_SLEW_RATE_FAST);
    }

#if ZERO2
    // Настройка направлений пинов для state machines
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, HDMI_BASE_PIN, 8, true);
    pio_sm_set_consecutive_pindirs(PIO_VIDEO_ADDR, SM_conv, HDMI_BASE_PIN, 8, true);

    uint64_t mask64 = (uint64_t)(3u << beginHDMI_PIN_clk);
    pio_sm_set_pins_with_mask64(PIO_VIDEO, SM_video, mask64, mask64);
    pio_sm_set_pindirs_with_mask64(PIO_VIDEO, SM_video, mask64, mask64);
    // пины
#else
    pio_sm_set_pins_with_mask(PIO_VIDEO, SM_video, 3u << beginHDMI_PIN_clk, 3u << beginHDMI_PIN_clk);
    pio_sm_set_pindirs_with_mask(PIO_VIDEO, SM_video, 3u << beginHDMI_PIN_clk, 3u << beginHDMI_PIN_clk);
    // пины
#endif

    for (int i = 0; i < 6; i++) {
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_data + i);
        gpio_set_drive_strength(beginHDMI_PIN_data + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, beginHDMI_PIN_data, 6, true);
    //конфигурация пинов на выход
    sm_config_set_out_pins(&c_c, beginHDMI_PIN_data, 6);

    //
    sm_config_set_out_shift(&c_c, true, true, 30);
    sm_config_set_fifo_join(&c_c, PIO_FIFO_JOIN_TX);

    int hdmi_hz = graphics_get_video_mode(get_video_mode()).freq;
    sm_config_set_clkdiv(&c_c, (clock_get_hz(clk_sys) / 252000000.0f) * (60 / hdmi_hz));
    pio_sm_init(PIO_VIDEO, SM_video, offs_prg0, &c_c);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, true);

    //настройки DMA
    dma_lines[0] = &conv_color[1024];
    dma_lines[1] = &conv_color[1124];

    //основной рабочий канал
    dma_channel_config cfg_dma = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);


    uint dreq = DREQ_PIO1_TX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_TX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan,
        &cfg_dma,
        &PIO_VIDEO_ADDR->txf[SM_conv], // Write address
        &dma_lines[0][0], // read address
        400, //
        false // Don't start yet
    );

    //контрольный канал для основного
    cfg_dma = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    DMA_BUF_ADDR[0] = &dma_lines[0][0];
    DMA_BUF_ADDR[1] = &dma_lines[1][0];

    dma_channel_configure(
        dma_chan_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan].read_addr, // Write address
        &DMA_BUF_ADDR[0], // read address
        1, //
        false // Don't start yet
    );

    //канал - конвертер палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv_ctrl); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_TX0 + SM_video;
    if (PIO_VIDEO == pio0) dreq = DREQ_PIO0_TX0 + SM_video;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv,
        &cfg_dma,
        &PIO_VIDEO->txf[SM_video], // Write address
        &conv_color[0], // read address
        4, //
        false // Don't start yet
    );

    //канал управления конвертером палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_RX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_RX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan_pal_conv].read_addr, // Write address
        &PIO_VIDEO_ADDR->rxf[SM_conv], // read address
        1, //
        true // start yet
    );

    //стартуем прерывание и канал
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_acknowledge_irq0(dma_chan_ctrl);
        dma_channel_set_irq0_enabled(dma_chan_ctrl, true);
    }
    else {
        dma_channel_acknowledge_irq1(dma_chan_ctrl);
        dma_channel_set_irq1_enabled(dma_chan_ctrl, true);
    }

    irq_set_exclusive_handler_DMA_core1();

    dma_start_channel_mask((1u << dma_chan_ctrl));

    return true;
};

void graphics_set_palette_hdmi(uint8_t i, uint32_t color888) {
    // Store color and update TMDS immediately
    // (This is safe because S9xFixColourBrightness is called between frames)
    color888 &= 0x00ffffff;
    palette[i] = color888;
    
    // Don't write to hardware palette for HDMI control indices (251-254), but allow 255 (bgcolor)
    if ((i >= BASE_HDMI_CTRL_INX) && (i != 255)) return;
    
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint8_t R = (color888 >> 16) & 0xff;
    const uint8_t G = (color888 >> 8) & 0xff;
    const uint8_t B = (color888 >> 0) & 0xff;
    conv_color64[i * 2] = get_ser_diff_data(tmds_encoder(R), tmds_encoder(G), tmds_encoder(B));
    conv_color64[i * 2 + 1] = conv_color64[i * 2] ^ 0x0003ffffffffffffl;
}

// Mark that a full palette update is needed (no longer used - kept for API compatibility)
void graphics_request_palette_update(void) {
    // Immediate conversion now happens in graphics_set_palette_hdmi()
}

// Convert all palette entries to TMDS format (called during vblank)
void graphics_convert_all_palette(void) {
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    
    // Convert first 251 colors (0-250) - skip HDMI control indices
    for (int i = 0; i < BASE_HDMI_CTRL_INX; i++) {
        uint32_t color888 = palette[i];
        const uint8_t R = (color888 >> 16) & 0xff;
        const uint8_t G = (color888 >> 8) & 0xff;
        const uint8_t B = (color888 >> 0) & 0xff;
        conv_color64[i * 2] = get_ser_diff_data(tmds_encoder(R), tmds_encoder(G), tmds_encoder(B));
        conv_color64[i * 2 + 1] = conv_color64[i * 2] ^ 0x0003ffffffffffffl;
    }
    
    // Also update color 255 (bgcolor)
    uint32_t color888 = palette[255];
    const uint8_t R = (color888 >> 16) & 0xff;
    const uint8_t G = (color888 >> 8) & 0xff;
    const uint8_t B = (color888 >> 0) & 0xff;
    conv_color64[255 * 2] = get_ser_diff_data(tmds_encoder(R), tmds_encoder(G), tmds_encoder(B));
    conv_color64[255 * 2 + 1] = conv_color64[255 * 2] ^ 0x0003ffffffffffffl;
    
    // Restore sync colors
    graphics_restore_sync_colors();

    // Keep substitute map up to date for any accidental use of reserved indices
    hdmi_recompute_color_substitute();
}

// Restore sync colors after palette update (called during vblank)
// Apply pending palette during vblank
static void apply_pending_palette(void) {
    if (!full_palette_update_pending) return;
    
    // Convert all software palette to TMDS
    graphics_convert_all_palette();
    
    full_palette_update_pending = false;
};

#define RGB888(r, g, b) ((r<<16) | (g << 8 ) | b )

void graphics_init_hdmi() {
    // PIO и DMA
    SM_video = pio_claim_unused_sm(PIO_VIDEO, true);
    SM_conv = pio_claim_unused_sm(PIO_VIDEO_ADDR, true);
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    dma_chan_pal_conv_ctrl = dma_claim_unused_channel(true);
    dma_chan_pal_conv = dma_claim_unused_channel(true);

    hdmi_init();
    
    // Initialize palette to all black and immediately convert to TMDS
    for (int i = 0; i < 256; i++) {
        palette[i] = 0;
    }
    graphics_convert_all_palette();
}

void graphics_set_bgcolor_hdmi(uint32_t color888) //определяем зарезервированный цвет в палитре
{
    graphics_set_palette_hdmi(255, color888);
};

void graphics_restore_sync_colors(void) {
    // Restore HDMI sync control colors after palette updates
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;
    const int base_inx = BASE_HDMI_CTRL_INX;

    conv_color64[2 * base_inx + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * base_inx + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * (base_inx + 1) + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * (base_inx + 1) + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * (base_inx + 2) + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * (base_inx + 2) + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * (base_inx + 3) + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * (base_inx + 3) + 1] = get_ser_diff_data(b0, b0, b0);
}

void graphics_set_mode(enum graphics_mode_t mode) {
    hdmi_graphics_mode = mode;
}

// Debug function to get palette value
uint32_t graphics_get_palette(uint8_t i) {
    return palette[i];
}

// Wrappers for existing API
void graphics_init(g_out g_out) {
    graphics_init_hdmi();
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    graphics_set_palette_hdmi(i, color888);
}

void graphics_set_bgcolor(uint32_t color888) {
    graphics_set_bgcolor_hdmi(color888);
}

void startVIDEO(uint8_t vol) {
    // Stub
}

void set_palette(uint8_t n) {
    // Stub
}
