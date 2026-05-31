#pragma once
#include <stdint.h>
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
    GLuint texture; /* GL_RGB5_A1 1024x512 */
} Renderer;

void renderer_init(Renderer *r, SDL_Window *window);

/* Upload VRAM and present it, cropped to the display window area */
void renderer_display(Renderer *r,
                      const uint16_t *vram,
                      uint16_t display_x, uint16_t display_y,
                      uint16_t display_w, uint16_t display_h);

void renderer_destroy(Renderer *r);
