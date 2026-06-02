#include "ui.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static SDL_Window    *s_window     = nullptr;
static SDL_GLContext  s_gl_ctx     = nullptr;
static bool           s_show_overlay = true;

/* Recent files list (max 8 entries) */
static constexpr int  MAX_RECENT = 8;
static std::vector<std::string> s_recent;

static void recent_push(const char *path)
{
    std::string p = path;
    for (auto it = s_recent.begin(); it != s_recent.end(); ++it)
        if (*it == p) { s_recent.erase(it); break; }
    s_recent.insert(s_recent.begin(), p);
    if ((int)s_recent.size() > MAX_RECENT)
        s_recent.resize(MAX_RECENT);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void ui_init(SDL_Window *window, SDL_GLContext gl_ctx)
{
    s_window = window;
    s_gl_ctx = gl_ctx;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = "psx_ui.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    /* TV-friendly dark style with slightly larger default font scale */
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowPadding     = ImVec2(16, 12);
    style.FramePadding      = ImVec2(10, 6);
    style.ItemSpacing       = ImVec2(10, 8);
    io.FontGlobalScale      = 1.2f;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");
}

void ui_process_event(SDL_Event *event)
{
    ImGui_ImplSDL2_ProcessEvent(event);
}

/* ------------------------------------------------------------------ */
/* Internal draw helpers                                                */
/* ------------------------------------------------------------------ */

static void try_load(const char *path, UiState *state)
{
    if (!path || path[0] == '\0') return;
    size_t len = strlen(path);
    if (len > 4)
    {
        const char *ext = path + len - 4;
        if (SDL_strcasecmp(ext, ".exe") == 0 && state->cb.on_load_exe)
        {
            recent_push(path);
            state->cb.on_load_exe(path);
            return;
        }
        if (SDL_strcasecmp(ext, ".bin") == 0 && state->cb.on_load_disc)
        {
            recent_push(path);
            state->cb.on_load_disc(path);
            return;
        }
    }
}

static void draw_launcher(UiState *state)
{
    ImGuiIO &io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("PSX Emulator").x) * 0.5f);
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "PSX Emulator");
    ImGui::Separator();
    ImGui::Spacing();

    /* Input field for manual path entry */
    static char path_buf[512] = {};
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##path", "Caminho do .exe ou .bin ...", path_buf, sizeof(path_buf));

    float btn_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Abrir EXE...", ImVec2(btn_w, 36)))
    {
        /* SDL2 has no built-in file dialog; use the path buffer */
        if (path_buf[0])
        {
            try_load(path_buf, state);
            path_buf[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Abrir Disc (.bin)...", ImVec2(btn_w, 36)))
    {
        if (path_buf[0])
        {
            try_load(path_buf, state);
            path_buf[0] = '\0';
        }
    }

    /* Enter key on the text field also loads */
    if (ImGui::IsItemDeactivatedAfterEdit() ||
        (ImGui::IsKeyPressed(ImGuiKey_Enter) && path_buf[0]))
    {
        try_load(path_buf, state);
        path_buf[0] = '\0';
    }

    /* Recent files */
    if (!s_recent.empty())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Recentes");
        ImGui::Spacing();
        for (auto &p : s_recent)
        {
            /* Show only the filename part */
            const char *name = p.c_str();
            const char *slash = strrchr(name, '/');
            if (!slash) slash = strrchr(name, '\\');
            const char *display = slash ? slash + 1 : name;

            if (ImGui::Selectable(display))
                try_load(p.c_str(), state);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", name);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() -
                          ImGui::CalcTextSize("Drag & drop .exe / .bin na janela").x) * 0.5f);
    ImGui::TextDisabled("Drag & drop .exe / .bin na janela");

    ImGui::End();
}

static void draw_overlay(UiState *state)
{
    if (!s_show_overlay) return;

    ImGuiIO &io = ImGui::GetIO();
    float pad = 10.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, pad), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::Begin("##overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::Text("%.0f FPS", (double)state->fps);
    if (state->display_w && state->display_h)
        ImGui::Text("%ux%u", state->display_w, state->display_h);

    ImGui::End();
}

static void draw_pause_menu(UiState *state)
{
    ImGuiIO &io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;

    /* Semi-transparent dimming backdrop */
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##dim", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();

    /* Pause panel */
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::Begin("##pause", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("PAUSA").x) * 0.5f);
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "PAUSA");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float bw = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Retomar", ImVec2(bw, 40)))
        if (state->cb.on_pause_toggle) state->cb.on_pause_toggle();

    ImGui::Spacing();
    if (ImGui::Button("Reset", ImVec2(bw, 40)))
        if (state->cb.on_reset) state->cb.on_reset();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Sair", ImVec2(bw, 40)))
        if (state->cb.on_quit) state->cb.on_quit();

    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Main render entry point                                              */
/* ------------------------------------------------------------------ */

void ui_render(UiState *state)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (!state->running)
        draw_launcher(state);
    else if (state->paused)
        draw_pause_menu(state);
    else
        draw_overlay(state);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(s_window);
}

void ui_destroy(void)
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}
