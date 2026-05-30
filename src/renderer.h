#pragma once
#include <stdint.h>
#include <GL/glew.h>
#include <SDL2/SDL.h>

#define VERTEX_BUFFER_LEN (64 * 1024)

typedef struct {
    GLshort x, y;
} Position;

typedef struct {
    GLubyte r, g, b;
} Color;

Position position_from_gp0(uint32_t val);
Color    color_from_gp0(uint32_t val);

typedef struct {
    SDL_Window   *window;
    SDL_GLContext gl_context;

    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;

    Position positions[VERTEX_BUFFER_LEN];
    Color    colors[VERTEX_BUFFER_LEN];
    uint32_t nvertices;

    GLint uniform_offset;
} Renderer;

void renderer_init(Renderer *r, SDL_Window *window);
void renderer_push_triangle(Renderer *r, Position positions[3], Color colors[3]);
void renderer_push_quad(Renderer *r, Position positions[4], Color colors[4]);
void renderer_draw(Renderer *r);
void renderer_set_draw_offset(Renderer *r, int16_t x, int16_t y);
void renderer_display(Renderer *r);
void renderer_destroy(Renderer *r);
