#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <GL/glew.h>
#include <SDL2/SDL.h>

typedef struct
{
    SDL_Window *window;
    SDL_GLContext gl_context;

    /* fullscreen-quad program for blitting VRAM */
    GLuint program;
    GLuint vao;
    GLuint vbo;
    GLuint texture; /* GL_RGB8 1024x512 */
    uint8_t *rgb_buffer;
} Renderer;

void renderer_init(Renderer *r, SDL_Window *window);

/* Upload VRAM and draw the PSX frame — does NOT call SwapWindow.
   Call renderer_present() after compositing any overlay (e.g. ImGui). */
void renderer_upload_frame(Renderer *r,
                           const uint16_t *vram,
                           uint16_t display_x, uint16_t display_y,
                           uint16_t display_w, uint16_t display_h,
                           bool display_24bit);

/* Swap the window buffer — call once per frame after all drawing is done. */
void renderer_present(Renderer *r);

/* Legacy: upload + present in one call (used in headless / non-UI builds). */
void renderer_display(Renderer *r,
                      const uint16_t *vram,
                      uint16_t display_x, uint16_t display_y,
                      uint16_t display_w, uint16_t display_h,
                      bool display_24bit);

void renderer_destroy(Renderer *r);
