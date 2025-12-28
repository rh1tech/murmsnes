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

#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"

#include "i2s_audio.h"

void i2s_audio::set_next_buffer(int16_t* pcm_buffer) {
  while (xSemaphoreTake(m_i2s_ready, portMAX_DELAY) != pdTRUE)
    ;

  m_pcm_next_buffer = pcm_buffer;
  xSemaphoreGive(m_next_buffer_ready);
}

void writer_task(void *param)
{
    i2s_audio *output = (i2s_audio *) param;
    int availableBytes = 0;
    int buffer_position = 0;

    while (true)
    {
        // wait for some data to be requested
        i2s_event_t evt;
        if (xQueueReceive(output->m_i2sQueue, &evt, portMAX_DELAY) == pdPASS)
        {
            if (evt.type == I2S_EVENT_TX_DONE)
            {
                if (availableBytes == 0) {
                    xSemaphoreGive(output->m_i2s_ready);

                    while (xSemaphoreTake(output->m_next_buffer_ready, portMAX_DELAY) != pdTRUE)
                    ;
                    
                    output->m_pcm_buffer = output->m_pcm_next_buffer;

                    // how many bytes do we now have to send
                    availableBytes = output->m_pcm_buffer_len * sizeof(uint16_t);
                    // reset the buffer position back to the start
                    buffer_position = 0;
                }

                while (availableBytes > 0) {
                    size_t bytesWritten = 0;
                    // write data to the i2s peripheral
                    i2s_write(output->m_i2sPort, buffer_position + (uint8_t *) output->m_pcm_buffer,
                                availableBytes, &bytesWritten, portMAX_DELAY);
                    availableBytes -= bytesWritten;
                    buffer_position += bytesWritten;
                } 
            }
        }
    }
}

void i2s_audio::start(i2s_port_t i2sPort, i2s_pin_config_t &i2sPins, int sample_rate,int16_t* pcm_buffer, uint32_t pcm_buffer_len)
{
    m_sample_rate = sample_rate;
    m_current_position = 0;
    m_pcm_buffer = pcm_buffer;
    m_pcm_next_buffer = pcm_buffer;
    m_pcm_buffer_len = pcm_buffer_len;

    m_next_buffer_ready = xSemaphoreCreateBinary();
    assert(m_next_buffer_ready);
    xSemaphoreGive(m_next_buffer_ready);
    xSemaphoreTake(m_next_buffer_ready, 0);

    m_i2s_ready = xSemaphoreCreateBinary();
    assert(m_i2s_ready);
    xSemaphoreGive(m_i2s_ready);
    xSemaphoreTake(m_i2s_ready, 0);

    // i2s config for writing both channels of I2S
    i2s_config_t i2sConfig = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = (uint32_t) m_sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format =  I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 2,
        .dma_buf_len = 1024,
        .tx_desc_auto_clear = true,
    };

    m_i2sPort = i2sPort;
    //install and start i2s driver
    i2s_driver_install(m_i2sPort, &i2sConfig, 4, &m_i2sQueue);
    // set up the i2s pins
    i2s_set_pin(m_i2sPort, &i2sPins);
    // clear the DMA buffers
    i2s_zero_dma_buffer(m_i2sPort);
    // start a task to write samples to the i2s peripheral

    xTaskCreatePinnedToCore(
        writer_task,   /* Function to implement the task */
        "i2s Writer Task", /* Name of the task */
        2048,          /* Stack size in words */
        this,          /* Task input parameter */
        1,             /* Priority of the task */
        NULL,          /* Task handle. */
        0);            /* Core where the task should run */

}

