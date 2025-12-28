/*
 * Snes9x running on the ESP32-P4-Function-EV-Board
 *
 * This file contains some code from: https://github.com/espressif
 * /esp-idf/blob/master/examples/peripherals/lcd/mipi_dsi/main/mipi_dsi_lcd_example_main.c
 *
 * Copyright (C) 2025 Daniel Kammer (daniel.kammer@web.de)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
 
 
#include "driver/ledc.h"

#include "esp_cache.h"

#include "esp_log.h"

#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_ek79007.h"

#include "disp_drv.h"

#define TEST_LCD_H_RES (1024)
#define TEST_LCD_V_RES (600)
#define TEST_LCD_BIT_PER_PIXEL (16)
#define TEST_PIN_NUM_LCD_RST (GPIO_NUM_4)
#define TEST_PIN_NUM_BK_LIGHT (GPIO_NUM_4)  // set to -1 if not used
#define TEST_LCD_BK_LIGHT_ON_LEVEL (1)
#define TEST_LCD_BK_LIGHT_OFF_LEVEL !TEST_LCD_BK_LIGHT_ON_LEVEL
#define TEST_PIN_NUM_VER_FLIP (-1)
#define TEST_PIN_NUM_HOR_FLIP (-1)
#define TEST_LCD_ROTATE_LEVEL (1)

#if TEST_LCD_BIT_PER_PIXEL == 24
#define TEST_MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB888)
#elif TEST_LCD_BIT_PER_PIXEL == 18
#define TEST_MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB666)
#elif TEST_LCD_BIT_PER_PIXEL == 16
#define TEST_MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB565)
#endif

#define TEST_MIPI_DSI_PHY_PWR_LDO_CHAN (3)
#define TEST_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV (2500)

/* =========================================== LCD ==================================================*/
//#define LCD_ROTATION
#define LCD_SCALING (3)  // LINE_BUFFER_HEIGHT needs to be set in esp_lcd_panel_dpi accordingly
#define LINE_BUF_SIZE (LCD_SCALING)

static char *TAG = "DISP_DRV";
static esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
volatile uint16_t *fb_hw[2] = { NULL, NULL };

volatile uint32_t mipi_status_info;

int fb_buffer_width = 0;
int fb_buffer_height = 0;

int prefer_vsync_over_fps = 0;

volatile uint16_t *m_fb_back = NULL;
volatile int line_block_cnt = 2;  // WA for missing vsync, should be 0

SemaphoreHandle_t vsync_event = xSemaphoreCreateBinary();
SemaphoreHandle_t fb_ready = xSemaphoreCreateBinary();

int ofs_src_start;
int ofs_x;
int ofs_y;
int ofs_src;
int ofs_src_line_inc;
int linebuf_width;

void calculate_image_offset() {
  #ifdef LCD_ROTATION
  ofs_x = (TEST_LCD_V_RES - fb_buffer_width * LCD_SCALING) / 2;

  ofs_y = (TEST_LCD_H_RES - fb_buffer_height * LCD_SCALING) / 2;

  ofs_src_start = -ofs_x / LCD_SCALING;

  ofs_src_line_inc = 1 * LINE_BUF_SIZE / LCD_SCALING;  // increment on each line

  linebuf_width = fb_buffer_height;

  if (ofs_y < 0) {
    ofs_src_line_inc -= ofs_y / LCD_SCALING * fb_buffer_width;
    linebuf_width = TEST_LCD_H_RES / LCD_SCALING;
    ofs_y = 0;
  }

  #else
  ofs_x = (TEST_LCD_H_RES - fb_buffer_width * LCD_SCALING) / 2;

  ofs_y = (TEST_LCD_V_RES - fb_buffer_height * LCD_SCALING) / 2;

  ofs_src_start = -ofs_y / LCD_SCALING * fb_buffer_width;

  ofs_src_line_inc = 1 * LINE_BUF_SIZE / LCD_SCALING * fb_buffer_width;  // increment on each line

  linebuf_width = fb_buffer_width;

  if (ofs_x < 0) {
    ofs_src_start -= ofs_x / LCD_SCALING;
    linebuf_width = TEST_LCD_H_RES / LCD_SCALING;
    ofs_x = 0;
  }
  #endif

  ofs_src = ofs_src_start;
}


IRAM_ATTR static bool test_notify_refresh_ready(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
  int fb_num = ((uint32_t) user_ctx) & 0xff;

  if ((((uint32_t) user_ctx) & 0xff00) != 0) {
    // if a buffer unterrun of the MIPI tx buffer occured, the MIPI driver will issue a 
    // sync event and the whole frame has to be scanned out again, however the first
    // line will have already been scanned out.
    line_block_cnt = 1;
    ofs_src = ofs_src_start + ofs_src_line_inc;
  }

  uint16_t *fb_line = (uint16_t *) fb_hw[fb_num];
  assert(fb_line);

  #ifdef LCD_ROTATION
  if ((ofs_src >= 0) && (ofs_src < fb_buffer_width)) {
  #else
   if ((ofs_src >= 0) && (ofs_src < fb_buffer_height * fb_buffer_width)) {
  #endif

    #ifdef LCD_ROTATION
    uint16_t *dst = (uint16_t*) &fb_line[ofs_y];
    uint16_t *src = (uint16_t*) &m_fb_back[ofs_src + (linebuf_width - 1) * fb_buffer_width];
    #else
    uint16_t *dst = (uint16_t*) &fb_line[ofs_x];
    uint16_t *src = (uint16_t*) &m_fb_back[ofs_src];
    #endif

    for (int x = 0; x < linebuf_width; x++) {
      *dst = *src;
      dst++;
      #if (LCD_SCALING>=2)
      *dst = *src;
      dst++;
      #endif
      #if (LCD_SCALING>=3)
      *dst = *src;
      dst++;
      #endif

      #ifdef LCD_ROTATION
      src -= fb_buffer_width;
      #else
      src++;
      #endif
    }
  } else {
    uint32_t *dst = (uint32_t*) &fb_line[0];

    for (int x = 0; x < TEST_LCD_H_RES / 2; x++) {
      *dst = 0;  // black
      dst++;
    }
  }

  esp_cache_msync((void *) fb_line, TEST_LCD_H_RES * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

  ofs_src += ofs_src_line_inc;

  line_block_cnt++;

  if (line_block_cnt == TEST_LCD_V_RES / LINE_BUF_SIZE) {
    line_block_cnt = 0;
    ofs_src = ofs_src_start;

    if (prefer_vsync_over_fps) {
      // If the emulation loop has not yet acked the vsync event, then now it's too late
      // and the emulator has to wait another frame. This prevents vertical tearing 
      // (horiz. tearing when roatated) but will cause low framerates. (Everyone get's to choose)
      if (xSemaphoreTakeFromISR(fb_ready, NULL) != pdTRUE)
        goto bailout;
    }

    xSemaphoreGiveFromISR(vsync_event, NULL);
  }

bailout:

  return (pdFALSE);
}

void test_init_lcd(lcd_config_t lcd_config) {
  fb_buffer_width = lcd_config.buffer_width;
  fb_buffer_height = lcd_config.buffer_height;
  prefer_vsync_over_fps = lcd_config.prefer_vsync_over_fps;

  calculate_image_offset();

#if TEST_PIN_NUM_BK_LIGHT >= 0
  ESP_LOGI(TAG, "Turn on LCD backlight");

  gpio_reset_pin((gpio_num_t) TEST_PIN_NUM_BK_LIGHT);

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = (100000),
        .clk_cfg          = LEDC_USE_XTAL_CLK
    };

    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .gpio_num       = TEST_PIN_NUM_BK_LIGHT,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 75, // max: 255
        .hpoint         = 0
    };

    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
#endif

  // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
#ifdef TEST_MIPI_DSI_PHY_PWR_LDO_CHAN
  ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
  esp_ldo_channel_config_t ldo_mipi_phy_config = {
    .chan_id = TEST_MIPI_DSI_PHY_PWR_LDO_CHAN,
    .voltage_mv = TEST_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
  };
  assert(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy) == ESP_OK);
#endif

  ESP_LOGI(TAG, "Initialize MIPI DSI bus");
  esp_lcd_dsi_bus_config_t bus_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
  assert(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus) == ESP_OK);

  ESP_LOGI(TAG, "Install panel IO");
  esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
  assert(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io) == ESP_OK);

  ESP_LOGI(TAG, "Install LCD driver of ek79007");
  esp_lcd_dpi_panel_config_t dpi_config = EK79007_1024_600_PANEL_55HZ_CONFIG(TEST_MIPI_DPI_PX_FORMAT);
  dpi_config.num_fbs = 2;
  ek79007_vendor_config_t vendor_config = {
    .mipi_config = {
      .dsi_bus = mipi_dsi_bus,
      .dpi_config = &dpi_config,
    },
    .flags = {
      .use_mipi_interface = 1,
    },
  };
  const esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = TEST_PIN_NUM_LCD_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = TEST_LCD_BIT_PER_PIXEL,
    .vendor_config = &vendor_config,
  };
  assert(esp_lcd_new_panel_ek79007(mipi_dbi_io, &panel_config, &panel_handle) == ESP_OK);
  assert(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 2, (void **) &fb_hw[0], (void **) &fb_hw[1]) == ESP_OK);

  esp_lcd_dpi_panel_event_callbacks_t cbs = {
    .on_refresh_done = test_notify_refresh_ready,
  };
  assert(esp_lcd_dpi_panel_register_event_callbacks(panel_handle, &cbs, (void*) &mipi_status_info) == ESP_OK);

  assert(esp_lcd_panel_reset(panel_handle) == ESP_OK);

  assert(esp_lcd_panel_init(panel_handle) == ESP_OK);

  assert(vsync_event);
  xSemaphoreGive(vsync_event);

  assert(fb_ready);
  xSemaphoreGive(fb_ready);
  xSemaphoreTake(fb_ready, 0);
}

void test_deinit_lcd(void) {
  assert(esp_lcd_panel_del(panel_handle) == ESP_OK);
  assert(esp_lcd_panel_io_del(mipi_dbi_io) == ESP_OK);
  assert(esp_lcd_del_dsi_bus(mipi_dsi_bus) == ESP_OK);
  panel_handle = NULL;
  mipi_dbi_io = NULL;
  mipi_dsi_bus = NULL;

  if (ldo_mipi_phy) {
    assert(esp_ldo_release_channel(ldo_mipi_phy) == ESP_OK);
    ldo_mipi_phy = NULL;
  }


#if TEST_PIN_NUM_BK_LIGHT >= 0
  assert(gpio_reset_pin(TEST_PIN_NUM_BK_LIGHT) == ESP_OK);
#endif
}

bool set_brightness(int level) {
  if ((level < 0) || (level > 255))
    return false;

  // ledcWrite(1 /* PWM channel */, level /* duty cycle */);
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));

  return true;
}

void set_fb_back(uint16_t* fb_back) {
  m_fb_back = fb_back;
}

void lcd_set_fb_ready() {
  xSemaphoreGive(fb_ready);
}

void lcd_wait_vsync() {

  while (xSemaphoreTake(vsync_event, 0) != pdTRUE)
    ;  // sync to LCD

}