/*
 * murmsnes - Graphics header wrapper
 * Maps graphics functions to HDMI driver
 */
#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "HDMI.h"

// Convert R, G, B (0-255) to 24-bit color value
#define RGB888(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#endif // GRAPHICS_H
