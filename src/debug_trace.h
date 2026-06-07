#pragma once

#include <stdbool.h>
#include <stdint.h>

void debug_trace_frame(uint32_t frame, uint16_t display_x, uint16_t display_y,
                       uint16_t display_w, uint16_t display_h, bool display_24bit,
                       bool display_disabled, uint64_t pixels_written);
void debug_trace_gp0(uint32_t frame, const uint32_t *words, uint8_t len,
                     int16_t draw_x_offset, int16_t draw_y_offset,
                     uint16_t draw_left, uint16_t draw_top,
                     uint16_t draw_right, uint16_t draw_bottom);
void debug_trace_gte(uint32_t cmd, const uint32_t before_dr[32], const uint32_t before_cr[32],
                     const uint32_t after_dr[32], const uint32_t after_cr[32]);
