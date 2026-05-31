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

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { fprintf(stderr, "Shader compile error\n"); exit(1); }
    return s;
}

static GLuint make_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   VERT_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    GLuint p  = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "Program link error\n"); exit(1); }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

void renderer_init(Renderer *r, SDL_Window *window) {
    r->window     = window;
    r->gl_context = SDL_GL_CreateContext(window);
    if (!r->gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); exit(1);
    }
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        fprintf(stderr, "glewInit: %s\n", glewGetErrorString(err)); exit(1);
    }

    r->program = make_program();

    /* fullscreen quad: two triangles, NDC coords + UV */
    static const float quad[] = {
        /* x      y      u      v  */
        -1.0f,  1.0f,  0.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
    };
    glGenVertexArrays(1, &r->vao);
    glGenBuffers(1, &r->vbo);
    glBindVertexArray(r->vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    GLint pos_loc = glGetAttribLocation(r->program, "pos");
    GLint uv_loc  = glGetAttribLocation(r->program, "uv");
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));

    glBindVertexArray(0);

    /* 1024x512 texture for full VRAM */
    glGenTextures(1, &r->texture);
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1,
                 1024, 512, 0,
                 GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV,
                 NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void renderer_display(Renderer *r,
                      const uint16_t *vram,
                      uint16_t display_x, uint16_t display_y,
                      uint16_t display_w, uint16_t display_h) {
    if (!r->window) return;

    /* Upload full VRAM each frame — simple and correct */
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512,
                    GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, vram);

    /* Compute UV crop so we show only the display area */
    float u0 = (float)display_x / 1024.0f;
    float v0 = (float)display_y / 512.0f;
    float u1 = (float)(display_x + display_w) / 1024.0f;
    float v1 = (float)(display_y + display_h) / 512.0f;

    /* If display is unknown/zero, show full VRAM */
    if (display_w == 0 || display_h == 0) { u0=0; v0=0; u1=1; v1=1; }

    float quad[] = {
        -1.0f,  1.0f,  u0, v0,
        -1.0f, -1.0f,  u0, v1,
         1.0f,  1.0f,  u1, v0,
         1.0f, -1.0f,  u1, v1,
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

void renderer_destroy(Renderer *r) {
    if (!r->window) return;
    glDeleteTextures(1, &r->texture);
    glDeleteBuffers(1, &r->vbo);
    glDeleteVertexArrays(1, &r->vao);
    glDeleteProgram(r->program);
    SDL_GL_DeleteContext(r->gl_context);
}
