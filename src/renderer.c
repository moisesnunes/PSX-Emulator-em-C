#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * The renderer's only job: upload gpu->vram as an OpenGL texture and
 * blit the display window area to the SDL window via a fullscreen quad.
 *
 * All rasterization is done in software directly into vram[].
 */

static const char *VERT_SRC =
    "#version 330 core\n"
    "in  vec2 pos;\n"
    "in  vec2 uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "  v_uv = uv;\n"
    "}\n";

static const char *FRAG_SRC =
    "#version 330 core\n"
    "in  vec2      v_uv;\n"
    "out vec4      frag;\n"
    "uniform sampler2D vram_tex;\n"
    "void main() {\n"
    "  frag = texture(vram_tex, v_uv);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        fprintf(stderr, "Shader compile error\n");
        exit(1);
    }
    return s;
}

static GLuint make_program(void)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        fprintf(stderr, "Program link error\n");
        exit(1);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

void renderer_init(Renderer *r, SDL_Window *window)
{
    r->window = window;
    r->rgb_buffer = NULL;
    r->gl_context = SDL_GL_CreateContext(window);
    if (!r->gl_context)
    {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        exit(1);
    }
    /* Disable vsync — we throttle via nanosleep in the main loop. */
    SDL_GL_SetSwapInterval(0);
    GLenum err = glewInit();
    if (err != GLEW_OK)
    {
        fprintf(stderr, "glewInit: %s\n", glewGetErrorString(err));
        exit(1);
    }

    r->program = make_program();

    /* fullscreen quad: two triangles, NDC coords + UV */
    static const float quad[] = {
        /* x      y      u      v  */
        -1.0f,
        1.0f,
        0.0f,
        0.0f,
        -1.0f,
        -1.0f,
        0.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.0f,
        1.0f,
        -1.0f,
        1.0f,
        1.0f,
    };
    glGenVertexArrays(1, &r->vao);
    glGenBuffers(1, &r->vbo);
    glBindVertexArray(r->vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    GLint pos_loc = glGetAttribLocation(r->program, "pos");
    GLint uv_loc = glGetAttribLocation(r->program, "uv");
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    glBindVertexArray(0);

    r->rgb_buffer = malloc(1024u * 512u * 3u);
    if (!r->rgb_buffer)
    {
        fprintf(stderr, "renderer: RGB buffer alloc failed\n");
        exit(1);
    }

    /* 1024x512 texture for full VRAM */
    glGenTextures(1, &r->texture);
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8,
                 1024, 512, 0,
                 GL_RGB, GL_UNSIGNED_BYTE,
                 NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void renderer_display(Renderer *r,
                      const uint16_t *vram,
                      uint16_t display_x, uint16_t display_y,
                      uint16_t display_w, uint16_t display_h,
                      bool display_24bit)
{
    if (!r->window)
        return;

    if (display_w == 0 || display_h == 0)
    {
        display_w = 1024;
        display_h = 512;
        display_x = 0;
        display_y = 0;
        display_24bit = false;
    }

    for (uint32_t y = 0; y < display_h; y++)
    {
        for (uint32_t x = 0; x < display_w; x++)
        {
            uint8_t r8, g8, b8;
            if (display_24bit)
            {
                uint32_t byte_base = (((display_y + y) & 511u) * 1024u +
                                      (display_x & 1023u)) * 2u + x * 3u;
                uint16_t w0 = vram[(byte_base / 2u) & (1024u * 512u - 1u)];
                uint16_t w1 = vram[((byte_base / 2u) + 1u) & (1024u * 512u - 1u)];
                uint32_t bytes = ((uint32_t)w1 << 16) | w0;
                if (byte_base & 1u)
                    bytes >>= 8;
                r8 = bytes & 0xFFu;
                g8 = (bytes >> 8) & 0xFFu;
                b8 = (bytes >> 16) & 0xFFu;
            }
            else
            {
                uint16_t c = vram[((display_y + y) & 511u) * 1024u +
                                  ((display_x + x) & 1023u)];
                uint8_t r5 = c & 0x1Fu;
                uint8_t g5 = (c >> 5) & 0x1Fu;
                uint8_t b5 = (c >> 10) & 0x1Fu;
                r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
                g8 = (uint8_t)((g5 << 3) | (g5 >> 2));
                b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
            }
            uint32_t out = (y * (uint32_t)display_w + x) * 3u;
            r->rgb_buffer[out + 0u] = r8;
            r->rgb_buffer[out + 1u] = g8;
            r->rgb_buffer[out + 2u] = b8;
        }
    }

    /* Upload the decoded display window at the texture origin. */
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, display_w, display_h,
                    GL_RGB, GL_UNSIGNED_BYTE, r->rgb_buffer);

    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = (float)display_w / 1024.0f;
    float v1 = (float)display_h / 512.0f;

    float quad[] = {
        -1.0f,
        1.0f,
        u0,
        v0,
        -1.0f,
        -1.0f,
        u0,
        v1,
        1.0f,
        1.0f,
        u1,
        v0,
        1.0f,
        -1.0f,
        u1,
        v1,
    };
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(r->program);
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glBindVertexArray(r->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    SDL_GL_SwapWindow(r->window);
}

void renderer_destroy(Renderer *r)
{
    if (!r->window)
        return;
    glDeleteTextures(1, &r->texture);
    glDeleteBuffers(1, &r->vbo);
    glDeleteVertexArrays(1, &r->vao);
    glDeleteProgram(r->program);
    free(r->rgb_buffer);
    r->rgb_buffer = NULL;
    SDL_GL_DeleteContext(r->gl_context);
}
