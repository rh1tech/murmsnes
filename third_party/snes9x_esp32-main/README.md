# This is Snes9x running on the ESP32-P4-Function-EV-Board by Espressif.

This is based on [this](https://github.com/ducalex/retro-go/) repo.

The ESP-IDF MIPI driver allocates a frame buffer in PSRAM. That is great for content where most of the screen basically won't change from frame to frame but is very slow when using dynamic content (like video games) where the screen content is usually regenerated every single frame.
Therefore the MIPI driver must be modified to use an IRAM line buffer instead.
The MIPI drivers: copies the frame buffer to a line buffer while rescaling the pixels (factor 1x, 2x or 3x supported). It can also rotate the screen. All that happens on the fly "ahead of the beam".

You can watch a comparison video on [Youtube](https://youtu.be/osw1QMM4Avs)

In order to run a ROM, you need to flash the ROM to `0x400000` of the SPI flash using esptool:

`esptool.py -p /dev/ttyUSB0 write_flash 0x400000 YourROM.smc`

Then, in `main.cpp`, adjust `Memory.ROM_Size` accordingly. 

I built it using ESP-IDF v.5.4.0:

`idf.py build flash`

Sound support for generic I2S sound chips has been added. Automatic frame dropping can be controlled using the compiler directive `ENABLE_FRAMEDROPPING` (disabling is not recommended). In some situations the CPU might not be able to keep up with the LCD's refresh rate. Vertical tearing may then occur (if the screen is rotated, then horizontal tearing would occur). You may use the compiler directive `PREFER_VSYNC_OVER_FPS` to decide whether you prefer a (massive) frame drop over this tearing (usually this is about a factor of 2 in FPS).



Snes9x also runs on the ESP32-S3 with approx. 45 fps. You can watch it [here](https://www.youtube.com/watch?v=lVLDIexSZ18).
