#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *VERTEX_SHADER_SRC =
    "#version 330 core\n"
    "in ivec2 vertex_position;\n"
    "in uvec3 vertex_color;\n"
    "out vec3 color;\n"
    "uniform ivec2 offset;\n"
    "void main() {\n"
    "  ivec2 position = vertex_position + offset;\n"
    "  float xpos = (float(position.x) / 512) - 1.0;\n"
    "  float ypos = 1.0 - (float(position.y) / 256);\n"
    "  gl_Position.xyzw = vec4(xpos, ypos, 0.0, 1.0);\n"
    "  color = vec3(\n"
    "    float(vertex_color.r) / 255,\n"
    "    float(vertex_color.g) / 255,\n"
    "    float(vertex_color.b) / 255\n"
    "  );\n"
    "}\n";

static const char *FRAGMENT_SHADER_SRC =
    "#version 330 core\n"
    "in vec3 color;\n"
    "out vec4 flag_color;\n"
    "void main() {\n"
    "  flag_color = vec4(color, 1.0);\n"
    "}\n";

static GLuint compile_shader(const char *src, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "Shader compilation failed!\n");
        exit(1);
    }
    return shader;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint status;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "OpenGL program linking failed!\n");
        exit(1);
    }
    return program;
}

static GLuint find_attrib(GLuint program, const char *name) {
    GLint index = glGetAttribLocation(program, name);
    if (index < 0) {
        fprintf(stderr, "Attribute \"%s\" not found in program\n", name);
        exit(1);
    }
    return (GLuint)index;
}

static GLint find_uniform(GLuint program, const char *name) {
    GLint index = glGetUniformLocation(program, name);
    if (index < 0) {
        fprintf(stderr, "Uniform \"%s\" not found in program\n", name);
        exit(1);
    }
    return index;
}

Position position_from_gp0(uint32_t val) {
    Position p;
    p.x = (GLshort)(int16_t)(val & 0xFFFF);
    p.y = (GLshort)(int16_t)(val >> 16);
    return p;
}

Color color_from_gp0(uint32_t val) {
    Color c;
    c.r = (GLubyte)(val & 0xFF);
    c.g = (GLubyte)((val >> 8) & 0xFF);
    c.b = (GLubyte)((val >> 16) & 0xFF);
    return c;
}

void renderer_init(Renderer *r, SDL_Window *window) {
    r->window = window;
    r->gl_context = SDL_GL_CreateContext(window);
    if (!r->gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        exit(1);
    }

    GLenum err = glewInit();
    if (err != GLEW_OK) {
        fprintf(stderr, "glewInit failed: %s\n", glewGetErrorString(err));
        exit(1);
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);

    r->vertex_shader   = compile_shader(VERTEX_SHADER_SRC,   GL_VERTEX_SHADER);
    r->fragment_shader = compile_shader(FRAGMENT_SHADER_SRC, GL_FRAGMENT_SHADER);
    r->program         = link_program(r->vertex_shader, r->fragment_shader);

    r->nvertices     = 0;
    r->uniform_offset = find_uniform(r->program, "offset");
    glUseProgram(r->program);
    glUniform2i(r->uniform_offset, 0, 0);

    memset(r->positions, 0, sizeof(r->positions));
    memset(r->colors,    0, sizeof(r->colors));
}

void renderer_push_triangle(Renderer *r, Position positions[3], Color colors[3]) {
    if (r->nvertices + 3 > VERTEX_BUFFER_LEN) {
        printf("Vertex buffer full, forcing draw\n");
        renderer_draw(r);
    }
    for (int i = 0; i < 3; i++) {
        r->positions[r->nvertices] = positions[i];
        r->colors[r->nvertices]    = colors[i];
        r->nvertices++;
    }
}

void renderer_push_quad(Renderer *r, Position positions[4], Color colors[4]) {
    if (r->nvertices + 6 > VERTEX_BUFFER_LEN)
        renderer_draw(r);

    for (int i = 0; i < 3; i++) {
        r->positions[r->nvertices] = positions[i];
        r->colors[r->nvertices]    = colors[i];
        r->nvertices++;
    }
    for (int i = 1; i < 4; i++) {
        r->positions[r->nvertices] = positions[i];
        r->colors[r->nvertices]    = colors[i];
        r->nvertices++;
    }
}

void renderer_draw(Renderer *r) {
    if (!r->window || r->nvertices == 0) { r->nvertices = 0; return; }

    GLuint position_vbo;
    glGenBuffers(1, &position_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, position_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 r->nvertices * sizeof(Position),
                 r->positions, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLuint color_vbo;
    glGenBuffers(1, &color_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, color_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 r->nvertices * sizeof(Color),
                 r->colors, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, position_vbo);
    GLuint pos_idx = find_attrib(r->program, "vertex_position");
    glEnableVertexAttribArray(pos_idx);
    glVertexAttribIPointer(pos_idx, 2, GL_SHORT, 0, NULL);

    glBindBuffer(GL_ARRAY_BUFFER, color_vbo);
    GLuint col_idx = find_attrib(r->program, "vertex_color");
    glEnableVertexAttribArray(col_idx);
    glVertexAttribIPointer(col_idx, 3, GL_UNSIGNED_BYTE, 0, NULL);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glUseProgram(r->program);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)r->nvertices);

    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    for (;;) {
        GLenum res = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 10000000);
        if (res == GL_ALREADY_SIGNALED || res == GL_CONDITION_SATISFIED) break;
    }
    glDeleteSync(sync);

    glDeleteBuffers(1, &position_vbo);
    glDeleteBuffers(1, &color_vbo);
    glDeleteVertexArrays(1, &vao);

    r->nvertices = 0;
}

void renderer_set_draw_offset(Renderer *r, int16_t x, int16_t y) {
    if (!r->window) return;
    renderer_draw(r);
    glUseProgram(r->program);
    glUniform2i(r->uniform_offset, (GLint)x, (GLint)y);
}

void renderer_display(Renderer *r) {
    if (!r->window) return;
    renderer_draw(r);
    SDL_GL_SwapWindow(r->window);
}

void renderer_destroy(Renderer *r) {
    if (!r->window) return;
    glDeleteShader(r->vertex_shader);
    glDeleteShader(r->fragment_shader);
    glDeleteProgram(r->program);
    SDL_GL_DeleteContext(r->gl_context);
}
