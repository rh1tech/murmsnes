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
 
 #pragma once

typedef struct {
  int buffer_width;
  int buffer_height;
  float target_fps;
  int prefer_vsync_over_fps;
} lcd_config_t;

void calculate_image_offset();
void test_init_lcd(lcd_config_t lcd_config);
bool set_brightness(int level);
void set_fb_back(uint16_t* fb_back);
void lcd_wait_vsync();
void lcd_set_fb_ready();