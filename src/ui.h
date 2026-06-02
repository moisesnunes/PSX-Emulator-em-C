#pragma once
#include <stdbool.h>
#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UiCallbacks
{
    void (*on_load_exe)(const char *path);
    void (*on_load_disc)(const char *path);
    void (*on_pause_toggle)(void);
    void (*on_reset)(void);
    void (*on_quit)(void);
} UiCallbacks;

typedef struct UiState
{
    bool        running;   /* true once an exe/disc is loaded and emulating */
    bool        paused;
    const char *loaded_path;  /* path of the currently loaded exe or disc */
    uint16_t    display_w;
    uint16_t    display_h;
    float       fps;
    UiCallbacks cb;
} UiState;

void ui_init(SDL_Window *window, SDL_GLContext gl_ctx);
void ui_process_event(SDL_Event *event);

/* Render ImGui overlay/launcher/pause-menu then call SwapWindow.
   renderer_upload_frame() must have been called before this. */
void ui_render(UiState *state);

void ui_destroy(void);

#ifdef __cplusplus
}
#endif
