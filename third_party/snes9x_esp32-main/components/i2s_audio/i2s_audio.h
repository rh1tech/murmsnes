/*
 * Snes9x running on the ESP32-P4-Function-EV-Board
 *
 * This file is based on github.com/atomic14/esp32_audio
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
 
 #pragma once

#include <cstdint>
#include "driver/i2s.h"

typedef struct
{
    int16_t left;
    int16_t right;
} Frame_t;

class i2s_audio
{
private:
    // I2S write task
    TaskHandle_t m_i2sWriterTaskHandle;
    // i2s writer queue
    QueueHandle_t m_i2sQueue;
    // i2s port
    i2s_port_t m_i2sPort;

    int m_sample_rate;
    int m_frequency;
    int m_current_position;
    int16_t* m_pcm_buffer;
    int16_t* m_pcm_next_buffer;
    uint32_t m_pcm_buffer_len;
    volatile SemaphoreHandle_t m_next_buffer_ready;
    volatile SemaphoreHandle_t m_i2s_ready;

    friend void writer_task(void *param);

public:
    void start(i2s_port_t i2sPort, i2s_pin_config_t &i2sPins, int sample_rate, int16_t* pcm_buffer, uint32_t pcm_buffer_len);
    virtual int get_sample_rate() { return m_sample_rate; }
    virtual void set_next_buffer(int16_t* pcm_buffer);
};
