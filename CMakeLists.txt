cmake_minimum_required(VERSION 3.13)

# Set board to Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

# Board configuration variant (M1, M2)
set(BOARD_VARIANT "M1" CACHE STRING "Board variant: M1 or M2")

# CPU overclock configuration (252, 378, 504 MHz)
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 252, 378, 504")

# PSRAM speed configuration (84, 100, 133, 166 MHz target)
set(PSRAM_SPEED "166" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 166")

# Lightweight on-device profiling (prints once per second over USB stdio)
option(MURMSNES_PROFILE "Enable periodic performance profiling output" OFF)

# Fast mode: aggressively reduce quality for higher framerate
# - Skips subscreen rendering entirely
# - Limits to single BG layer in Mode 1
# - Disables color math transparency effects
option(MURMSNES_FAST_MODE "Enable fast mode (reduced quality for higher FPS)" OFF)

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "Board: ${BOARD_VARIANT}, CPU: ${CPU_SPEED} MHz, PSRAM: ${PSRAM_SPEED} MHz, Voltage: ${CPU_VOLTAGE}")

include(pico_sdk_import.cmake)

# Import pico-extras for audio_i2s library (git submodule)
set(PICO_EXTRAS_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-extras CACHE PATH "Path to pico-extras")
include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

project(murmsnes C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Initialize pico-extras if available
if(COMMAND pico_extras_init)
    pico_extras_init()
endif()

# Add fatfs
add_subdirectory(src/fatfs)

# Add sdcard driver
add_subdirectory(drivers/sdcard)

# Create a library for drivers
add_library(drivers
    drivers/HDMI.c
    drivers/hdmi_scanline.S
    drivers/psram_init.c
    drivers/psram_allocator.c
    drivers/audio.c
    drivers/audio_i2s_opt.S
)
target_include_directories(drivers PUBLIC drivers src)
target_compile_definitions(drivers PRIVATE
    BOARD_${BOARD_VARIANT}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    # I2S Audio configuration - use PIO1 (HDMI uses PIO0)
    PICO_AUDIO_I2S_PIO=1
    PICO_AUDIO_I2S_DMA_IRQ=0
)

# Set I2S pins based on board variant for drivers lib
if(BOARD_VARIANT STREQUAL "M1")
    target_compile_definitions(drivers PRIVATE
        PICO_AUDIO_I2S_DATA_PIN=26
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=27
    )
elseif(BOARD_VARIANT STREQUAL "M2")
    target_compile_definitions(drivers PRIVATE
        PICO_AUDIO_I2S_DATA_PIN=9
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=10
    )
endif()

target_link_libraries(drivers pico_stdlib hardware_dma hardware_pio hardware_spi pico_audio_i2s)

# Snes9x emulator sources
set(SNES9X_SOURCES
    src/snes9x/apu.c
    src/snes9x/c4.c
    src/snes9x/c4emu.c
    src/snes9x/clip.c
    src/snes9x/cpu.c
    src/snes9x/cpuexec.c
    src/snes9x/cpuops.c
    src/snes9x/dma.c
    src/snes9x/dsp.c
    src/snes9x/getset.c
    src/snes9x/gfx.c
    src/snes9x/globals.c
    src/snes9x/memmap.c
    src/snes9x/obc1.c
    src/snes9x/ppu.c
    src/snes9x/snapshot.c
    src/snes9x/soundux.c
    src/snes9x/spc700.c
    src/snes9x/srtc.c
    src/snes9x/tile.c
    src/snes9x/cpu_asm.S
    src/snes9x/tile_asm.S
)

# Assembly optimizations
set(ASM_OPT_SOURCES
    src/asm/tile_convert_optimized.S
    src/tile_convert_opt.c
    src/audio_opt.c
    src/asm/audio_pack_asm.S
)

add_executable(murmsnes
    src/main.c
    src/murmsnes_profile.c
    ${SNES9X_SOURCES}
    ${ASM_OPT_SOURCES}
)

# Enable aggressive optimization for SNES9x hot path
# -O3: Maximum optimization
# -fno-strict-aliasing: Required for SNES9x pointer casting
# -funroll-loops: Unroll loops for speed
# -fomit-frame-pointer: Free up a register
set_source_files_properties(${SNES9X_SOURCES} PROPERTIES COMPILE_FLAGS 
    "-O3 -fno-strict-aliasing -funroll-loops -fomit-frame-pointer")

target_include_directories(murmsnes PRIVATE
    src
    src/snes9x
    src/fatfs
    drivers
    drivers/sdcard
)

target_compile_definitions(murmsnes PRIVATE
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    PICO_ON_DEVICE=1
    # I2S Audio configuration - use PIO1 (HDMI uses PIO0)
    PICO_AUDIO_I2S_PIO=1
    PICO_AUDIO_I2S_DMA_IRQ=0
    # Snes9x configuration - FAST_LSB_WORD_ACCESS is defined in port.h for ARM
    # Performance optimizations - disable expensive rendering features
    NO_WINDOW_CLIPPING=1
    SIMPLE_COLOR_MATH=1
)

if(MURMSNES_PROFILE)
    target_compile_definitions(murmsnes PRIVATE MURMSNES_PROFILE=1)
endif()

if(MURMSNES_FAST_MODE)
    target_compile_definitions(murmsnes PRIVATE MURMSNES_FAST_MODE=1)
    message(STATUS "FAST MODE enabled - reduced quality for higher FPS")
endif()

# Set peripheral pins based on board variant
if(BOARD_VARIANT STREQUAL "M1")
    target_compile_definitions(murmsnes PRIVATE
        # I2S Audio
        PICO_AUDIO_I2S_DATA_PIN=26
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=27
        # SD Card
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=2
        SDCARD_PIN_SPI0_MOSI=3
        SDCARD_PIN_SPI0_MISO=4
    )
elseif(BOARD_VARIANT STREQUAL "M2")
    target_compile_definitions(murmsnes PRIVATE
        # I2S Audio
        PICO_AUDIO_I2S_DATA_PIN=9
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=10
        # SD Card
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=6
        SDCARD_PIN_SPI0_MOSI=7
        SDCARD_PIN_SPI0_MISO=4
    )
endif()

target_link_libraries(murmsnes
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_irq
    sdcard
    fatfs
    drivers
)

# Use USB for stdio output
pico_enable_stdio_usb(murmsnes 1)
pico_enable_stdio_uart(murmsnes 0)

pico_add_extra_outputs(murmsnes)
