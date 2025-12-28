/*
 * Snes9x running on the ESP32-P4-Function-EV-Board
 *
 * Copyright (C) 2023 Daniel Kammer (daniel.kammer@web.de)
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

/* ============================== INCLUDES ===================================*/
// basics
#include <inttypes.h>
#include <cassert>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <mutex>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// platfrom
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_dma_utils.h"
#include "spi_flash_mmap.h"

// video
#include "disp_drv.h"

// audio
#include "i2s_audio.h"

// SNES
extern "C" {
#include "snes9x/snes9x.h"
#include "snes9x/soundux.h"
#include "snes9x/memmap.h"
#include "snes9x/apu.h"
#include "snes9x/display.h"
#include "snes9x/cpuexec.h"
#include "snes9x/srtc.h"
#include "snes9x/save.h"
#include "snes9x/gfx.h"
}

/* ============================== EMU COMPILE TIME SWITCHES ===================================*/
// video
#define PREFER_VSYNC_OVER_FPS (0) 
#define ENABLE_FRAMEDROPPING
#define TARGET_FPS (55)
#define TARGET_FRAME_DURATION (1000000 / TARGET_FPS * 100 / 100)  // add a little margin

// sound
#define AUDIO_SAMPLE_RATE (32040 * 60 / TARGET_FPS) // keep same sampling rate constant at lower fps
#define AUDIO_BUFFER_NUM_FRAMES (5)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / TARGET_FPS)

#ifdef NO_PSRAM
#include "rom_super_mario_world.h"
#endif

/* ============================== GLOBAL VARIABLES ===================================*/

static char *TAG = "SNES9x_ESP32-P4";

struct timeval _startTime;
  
typedef struct {
  // video
  uint16_t *fb[2];
  int brightness;

  // sound
  int16_t* audio_buf[2];
  SemaphoreHandle_t make_sound;
  i2s_audio *audio_output;
  int volume;

  // controls
  uint16_t dpad;
  uint16_t buts;

  // save state
  uint32_t just_saved_timer;
  int just_saved;
  volatile int sd_init;

} emulator_control_t;

emulator_control_t emulater_control;

// snes stuff
bool overclock_cycles = false;
int one_c = 4, slow_one_c = 5, two_c = 6;
extern SGFX GFX;       // snes9x API
char *savestate_name;  // = "save.sav";  // short file name dos

/* ============================== IMPLEMENTATION ===================================*/
/* ------------------------------ SNES callbacks -----------------------------------*/
static bool reset_handler(bool hard) {
  S9xReset();
  return true;
}

bool S9xInitDisplay(void) {
  GFX.Pitch = SNES_WIDTH * 2;  // 16 bbp
  GFX.ZPitch = SNES_WIDTH;
  GFX.Screen = (uint8_t *) emulater_control.fb[0];

  // seems crazy but works for Super Mario World et. al. and saves 112 KB of RAM
  GFX.SubScreen = GFX.Screen;//(uint8_t*) heap_caps_malloc(GFX.Pitch * SNES_HEIGHT_EXTENDED, MALLOC_CAP_INTERNAL); // 112 KB
  GFX.ZBuffer = (uint8_t *) heap_caps_malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED, MALLOC_CAP_INTERNAL);   // 56 KB
  GFX.SubZBuffer = (uint8_t *) heap_caps_malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED, MALLOC_CAP_SPIRAM);  // 56 KB
  return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void) {
}

uint32_t S9xReadJoypad(int32_t port) {
  if (port != 0)
    return 0;

  uint32_t joypad = 0;

#if 0
  if (emulater_control.dpad & DPAD_LEFT) joypad |= SNES_LEFT_MASK;
  if (emulater_control.dpad & DPAD_RIGHT) joypad |= SNES_RIGHT_MASK;
  if (emulater_control.dpad & DPAD_UP) joypad |= SNES_UP_MASK;
  if (emulater_control.dpad & DPAD_DOWN) joypad |= SNES_DOWN_MASK;

  if ((emulater_control.buts & BUTTON_1) && (emulater_control.buts & BUTTON_2) && (emulater_control.buts & BUTTON_3)) {
    joypad |= SNES_START_MASK;
  } else {
    if (emulater_control.buts & BUTTON_1) joypad |= SNES_A_MASK;
    if (emulater_control.buts & BUTTON_2) joypad |= SNES_B_MASK;
    if (emulater_control.buts & BUTTON_3) joypad |= SNES_X_MASK;
  }
#endif

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

static void update_keymap(int id) {
}

static bool screenshot_handler(const char *filename, int width, int height) {
  //return rg_display_save_frame(filename, currentUpdate, width, height);
  return true;
}

static bool save_state_handler(char *filename) {
  bool ret = false;
#ifdef SD_ENABLED
  char filename_local[ROM_NAME_LEN + 2];

  filename_local[0] = '/';
  memcpy(&filename_local[1], filename, ROM_NAME_LEN);
  filename_local[ROM_NAME_LEN + 1] = 0;

  File savestate_file;

  //Serial.println("Savestate handler called...");

  if (!emulater_control.sd_init) {
    if (!SD.begin(PIN_SD_CS, SPI, 10000000)) {
      //Serial.println("Unable to init SD card");
      goto bailout;
    }

    emulater_control.sd_init = 1;
  }

  if (SD.exists(filename)) {
    if (!SD.remove(filename)) {
      //Serial.println("Unable to delete game state file");
      goto bailout;
    }
  }

  savestate_file = SD.open(filename_local, FILE_WRITE);

  if (!savestate_file) {
    //Serial.println("Unable to open save state file for writing.");
    goto bailout;
  }

  savestate_file.write((uint8_t *)&CPU, sizeof(CPU));
  savestate_file.write((uint8_t *)&ICPU, sizeof(ICPU));
  savestate_file.write((uint8_t *)&PPU, sizeof(PPU));
  savestate_file.write((uint8_t *)&DMA, sizeof(DMA));
  savestate_file.write((uint8_t *)Memory.VRAM, VRAM_SIZE);
  savestate_file.write((uint8_t *)Memory.RAM, RAM_SIZE);
  savestate_file.write((uint8_t *)Memory.SRAM, SRAM_SIZE);
  savestate_file.write((uint8_t *)Memory.FillRAM, FILLRAM_SIZE);
  savestate_file.write((uint8_t *)&APU, sizeof(APU));
  savestate_file.write((uint8_t *)&IAPU, sizeof(IAPU));
  savestate_file.write((uint8_t *)IAPU.RAM, 0x10000);
  savestate_file.write((uint8_t *)&SoundData, sizeof(SoundData));

  savestate_file.close();

  //Serial.println("Game state saved.");

  ret = true;

bailout:

  if (ret)
    emulater_control.just_saved = 1;
  else
    emulater_control.just_saved = 2;

  emulater_control.just_saved_timer = millis();
#endif
  return ret;
}

static bool load_state_handler(char *filename) {
#ifdef SD_ENABLED
  char filename_local[ROM_NAME_LEN + 2];

  filename_local[0] = '/';
  memcpy(&filename_local[1], filename, ROM_NAME_LEN);
  filename_local[ROM_NAME_LEN + 1] = 0;

  //Serial.println("Loadstate handler called...");

  if (!emulater_control.sd_init) {
    if (!SD.begin(PIN_SD_CS, SPI, 10000000)) {
      //Serial.println("Unable to init SD card");
      emulater_control.just_saved_timer = millis();
      emulater_control.just_saved = 2;
      return false;
    }

    emulater_control.sd_init = 1;
  }

  File readstate_file = SD.open(filename_local, FILE_READ);

  if (!readstate_file) {
    //Serial.println("Failed to load game state:");
    //Serial.print("*");
    //Serial.print(filename_local);
    //Serial.println("*");
    emulater_control.just_saved_timer = millis();
    emulater_control.just_saved = 2;
    return false;
  }

  S9xReset();

  // At this point we can't go back and a failure will corrupt the state anyway
  //Serial.println("Now reading to memory.");

  readstate_file.read((uint8_t *)&CPU, sizeof(CPU));
  readstate_file.read((uint8_t *)&ICPU, sizeof(ICPU));
  readstate_file.read((uint8_t *)&PPU, sizeof(PPU));
  readstate_file.read((uint8_t *)&DMA, sizeof(DMA));
  readstate_file.read((uint8_t *)Memory.VRAM, VRAM_SIZE);
  readstate_file.read((uint8_t *)Memory.RAM, RAM_SIZE);
  readstate_file.read((uint8_t *)Memory.SRAM, SRAM_SIZE);
  readstate_file.read((uint8_t *)Memory.FillRAM, FILLRAM_SIZE);
  readstate_file.read((uint8_t *)&APU, sizeof(APU));
  readstate_file.read((uint8_t *)&IAPU, sizeof(IAPU));
  readstate_file.read((uint8_t *)IAPU.RAM, 0x10000);
  readstate_file.read((uint8_t *)&SoundData, sizeof(SoundData));

  readstate_file.close();

  //Serial.println("Game state loaded.");

  return S9xLoadState();
#else
  return false;
#endif
}

/* ------------------------- TIMING ------------------------------*/
uint32_t micros() {
  struct timeval currentTime;
  gettimeofday(&currentTime, NULL);
  return (uint32_t) (((currentTime.tv_sec - _startTime.tv_sec) * 1000000) + currentTime.tv_usec - _startTime.tv_usec);  // - _timeSuspended;
}

uint32_t millis() {
  struct timeval currentTime;
  gettimeofday(&currentTime, NULL);
  return (uint32_t)(((currentTime.tv_sec - _startTime.tv_sec) * 1000) + ((currentTime.tv_usec - _startTime.tv_usec) / 1000));  // - _timeSuspended;
}

/* ------------------------- MISC. ------------------------------*/
void check_load_save(void) {
}

void emu_panic(char *str) {
  printf("%s\n", str);

  vTaskDelay( 5000 / portTICK_PERIOD_MS );

  abort();
}

/* ------------------------- HARDWARE ------------------------------*/
bool copy_rom_to_psram() {
  uint8_t *tmp_rom;

  // load a ROM from flash
  spi_flash_mmap_handle_t out_handle;
  assert(spi_flash_mmap(0x400000 /* size_t src_addr */,
                        0x400000 /* size_t size */,
                        SPI_FLASH_MMAP_DATA /* spi_flash_mmap_memory_t memory */,
                        (const void **)&(tmp_rom) /* const void **out_ptr */,
                        &out_handle /* spi_flash_mmap_handle_t *out_handle */)
         == ESP_OK);

  //Memory.ROM_Size = 2621440;  // All Stars + Super Mario World
  //Memory.ROM_Size = 512 * 1024;  // Super Mario World
  //Memory.ROM_Size = 0x400000;  // Donkey Kong Country
  //Memory.ROM_Size = 0x300000;  // The Lion King, Warning, there's two versions with different filesizes
  Memory.ROM_Size = 1024 * 1024;  // TMNT
  //Memory.ROM_Size = 2 * 1024 * 1024;  // TMNT - Tournament

  Memory.ROM = (uint8_t*) heap_caps_calloc(1, Memory.ROM_Size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  assert(Memory.ROM);

  for (uint32_t i = 0; i < Memory.ROM_Size; i++)
    Memory.ROM[i] = tmp_rom[i];

  spi_flash_munmap(out_handle);

  return true;
}

/* ------------------------- SETUP ------------------------------*/
void snes_init() {
  Settings.CyclesPercentage = 100;
  Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
  Settings.FrameTimePAL = 20000;
  Settings.FrameTimeNTSC = 16667;
  Settings.ControllerOption = SNES_JOYPAD;
  Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
  Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
  Settings.DisableSoundEcho = false;
  Settings.InterpolatedSound = true;
#ifdef USE_BLARGG_APU
  Settings.SoundInputRate = AUDIO_SAMPLE_RATE;
#endif

  if (!S9xInitMemory())
    emu_panic("Memory init failed!");

  if (!S9xInitAPU())
    emu_panic("APU init failed!");

  if (!S9xInitSound(0, 0))
    emu_panic("Sound init failed!");

  if (!S9xInitGFX())
    emu_panic("Graphics init failed!");

  // load rom
  #ifdef NO_PSRAM
  Memory.ROM_Size = ROM_SIZE;
  Memory.ROM = (uint8_t*) ROM_DATA;
  #else
  copy_rom_to_psram();
  #endif

  if (!LoadROM(NULL))
    emu_panic("ROM loading failed!");

#ifdef USE_BLARGG_APU
    //S9xSetSamplesAvailableCallback(S9xAudioCallback);
#else
  S9xSetPlaybackRate(Settings.SoundPlaybackRate);
#endif
}

void audio_init() {
  emulater_control.make_sound = xSemaphoreCreateBinary();
  assert(emulater_control.make_sound);
  xSemaphoreGive(emulater_control.make_sound);
  xSemaphoreTake(emulater_control.make_sound, 0);

  for (int i = 0; i < 2; i++) {
    emulater_control.audio_buf[i] = (int16_t *) heap_caps_calloc(1, AUDIO_BUFFER_LENGTH * AUDIO_BUFFER_NUM_FRAMES * 2 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(emulater_control.audio_buf[i]);
  }

  // needs to be called prior to anything else, because "i2s_driver_install" sets up some
  // "default" GPIOs, potentially overwriting the GPIO config.
  i2s_pin_config_t i2sPins = {
      .bck_io_num = 5, //PIN_SND_BCK,
      .ws_io_num = 6, //PIN_SND_WS,
      .data_out_num = 7, //PIN_SND_DOUT,
      .data_in_num = -1
  };

  emulater_control.audio_output = new i2s_audio();
  (emulater_control.audio_output)->start(I2S_NUM_0, i2sPins, AUDIO_SAMPLE_RATE, emulater_control.audio_buf[0], AUDIO_BUFFER_LENGTH * AUDIO_BUFFER_NUM_FRAMES * 2);
}

void gfx_init() {
  for (int i = 0; i < 2; i++) {
    emulater_control.fb[i] = (uint16_t *) heap_caps_calloc(1, SNES_WIDTH * SNES_HEIGHT_EXTENDED * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(emulater_control.fb[i]);
  }

  if (!S9xInitDisplay())
    emu_panic("Display init failed!");

  set_fb_back((uint16_t *)emulater_control.fb[1]);

}

void setup(void) {
  gettimeofday(&_startTime, NULL);

  gfx_init();

  audio_init();

#if 0
  // SD
  gpio_set_direction(PIN_SD_MOSI, GPIO_MODE_INPUT);
  gpio_set_direction(PIN_SD_MISO, GPIO_MODE_INPUT);
  gpio_set_direction(PIN_SD_SCK, GPIO_MODE_INPUT);
#endif

#if 0
  ctrl_init();
#endif

#ifdef SD_ENABLED
  //Serial.println("Initializing SPI...");
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, -1);
#endif

  snes_init();

  emulater_control.sd_init = 0;
  emulater_control.just_saved = 0;
  savestate_name = (char *) Memory.ROMName;
  load_state_handler(savestate_name);  // try and load a saved state
}

/* ------------------------- EMULATION ------------------------------*/
void emulation_loop(void *parameter) {

  bool menuCancelled = false;
  bool menuPressed = false;

  int frame_no = 0;
  unsigned long fps_timer = millis();
  
#ifdef ENABLE_FRAMEDROPPING
  int framedrop_occurred = 0;
  int32_t framedrop_balance = 0;
  uint32_t framedrop_timer = micros();
#endif

  while (1) {
#if 0
      emulater_control.dpad = ctrl_dpad_state();
      emulater_control.buts = ctrl_button_state();
#endif
    check_load_save();

    S9xMainLoop();
    xSemaphoreGive(emulater_control.make_sound);

    /* ---------- LCD ------------*/
    if (IPPU.RenderThisFrame) {
      if (emulater_control.just_saved) {
        uint16_t *scr = (uint16_t *)GFX.Screen;
        uint16_t col = emulater_control.just_saved == 1 ? 0b0000011111100000 : 0b1111100000000000;
        for (int x = 230; x < 240; x++)
          for (int y = 12; y < 22; y++) {
            scr[x + y * SNES_WIDTH] = col;
          }
        if (millis() - emulater_control.just_saved_timer > 2000)
          emulater_control.just_saved = 0;
      }

      lcd_set_fb_ready();
      lcd_wait_vsync();

      if ((void *)GFX.Screen == (void *)emulater_control.fb[0]) {
        GFX.Screen = (uint8_t *)emulater_control.fb[1];
        set_fb_back((uint16_t *)emulater_control.fb[0]);
      } else {
        GFX.Screen = (uint8_t *)emulater_control.fb[0];
        set_fb_back((uint16_t *)emulater_control.fb[1]);
      }

      GFX.SubScreen = GFX.Screen;
    }

    /* ---------- LCD END------------*/

#ifdef ENABLE_FRAMEDROPPING
    IPPU.RenderThisFrame = true;
    framedrop_balance += (micros() - framedrop_timer) - TARGET_FRAME_DURATION;
    framedrop_timer = micros();

    if (framedrop_balance < 550) //  (a little more to not accidentally trigger framedrop by calculation inaccuracies)
      framedrop_balance = 0;

    if (framedrop_balance > TARGET_FRAME_DURATION) {
      // We're now a whole frame behind, so skip the next frame
      IPPU.RenderThisFrame = false;
      framedrop_occurred++;
    }   
#endif

    frame_no++;
    if (millis() - fps_timer > 1000) {
      ESP_LOGI(TAG, "fps: %d", (int)(frame_no * 1000 / (millis() - fps_timer)));
      frame_no = 0;
      fps_timer = millis();
#ifdef ENABLE_FRAMEDROPPING
      if (framedrop_occurred) {
        ESP_LOGI(TAG, "Frame drop occurred: %d frames dropped.", framedrop_occurred);
        framedrop_occurred = 0;
      }
#endif
    }
  }  
}

void audio_loop() {

  int cur_audio_buf = 0;

  uint32_t audio_timer = millis();
  int audio_frame_cnt = 0;
  int audio_frame_no = 0;

  while(1) {
    while (xSemaphoreTake(emulater_control.make_sound, portMAX_DELAY) != pdTRUE)
      ;
    
    S9xMixSamples(&(emulater_control.audio_buf[cur_audio_buf][AUDIO_BUFFER_LENGTH * 2 * audio_frame_no]), AUDIO_BUFFER_LENGTH * 2);

    int16_t* tmp_audio_buf = &(emulater_control.audio_buf[cur_audio_buf][AUDIO_BUFFER_LENGTH * 2 * audio_frame_no]);

    for (int i = 0; i < AUDIO_BUFFER_LENGTH * 2; i++) {
      *tmp_audio_buf /= 2; // volume "control"
      tmp_audio_buf++;
    }

    audio_frame_no++;
    if (audio_frame_no == AUDIO_BUFFER_NUM_FRAMES) {
      (emulater_control.audio_output)->set_next_buffer(emulater_control.audio_buf[cur_audio_buf]);
      cur_audio_buf++;
      cur_audio_buf = cur_audio_buf % 2;
      audio_frame_no = 0;
    }

    audio_frame_cnt++;

    if (millis() - audio_timer > 1000) {
      ESP_LOGI(TAG, "audio fps: %d", (int)(audio_frame_cnt * 1000 / (millis() - audio_timer)));
      audio_frame_cnt = 0;
      audio_timer = millis();
    }
  }  
}

/* ------------------------- MAIN ------------------------------*/

// freeRTOS calls app_main from C
extern "C" {
  extern void app_main();
}

void app_main(void) {
  setup();

  xTaskCreatePinnedToCore(
    emulation_loop,   /* Function to implement the task */
    "emulation_loop", /* Name of the task */
    4096,          /* Stack size in words */
    NULL,          /* Task input parameter */
    18,             /* Priority of the task */
    NULL,          /* Task handle. */
    1);            /* Core where the task should run */

  lcd_config_t lcd_config = {
    .buffer_width = SNES_WIDTH,
    .buffer_height = SNES_HEIGHT,
    .target_fps = TARGET_FPS,
    .prefer_vsync_over_fps = PREFER_VSYNC_OVER_FPS,
  };

  // needs to take place this late to prevent a race condition
  // on core 1 - cause unknown, needs further investigation
  // Only a workaround until a clean vsync end signal is available
  // from the MIPI driver
  test_init_lcd(lcd_config);

  audio_loop();
}