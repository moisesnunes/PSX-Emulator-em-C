#include "debug_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    TRACE_EVENT_FRAME = 1u << 0,
    TRACE_EVENT_GP0 = 1u << 1,
    TRACE_EVENT_GTE = 1u << 2,
    TRACE_EVENT_ALL = TRACE_EVENT_FRAME | TRACE_EVENT_GP0 | TRACE_EVENT_GTE,
};

typedef struct
{
    bool initialized;
    bool enabled;
    FILE *file;
    uint64_t limit;
    uint64_t count;
    uint32_t start_frame;
    uint32_t current_frame;
    uint32_t event_mask;
} DebugTraceState;

static DebugTraceState g_trace;

static bool trace_token_matches(const char *events, const char *name)
{
    size_t name_len = strlen(name);
    const char *p = events;
    while (*p)
    {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t')
            p++;
        if ((size_t)(p - start) == name_len && strncmp(start, name, name_len) == 0)
            return true;
    }
    return false;
}

static void debug_trace_init(void)
{
    if (g_trace.initialized)
        return;
    g_trace.initialized = true;
    g_trace.event_mask = TRACE_EVENT_ALL;

    const char *path = getenv("PS1_TRACE_VISUAL");
    if (!path || !path[0])
        path = getenv("PS1_TRACE_GPU_GTE");
    if (!path || !path[0])
        return;

    if (strcmp(path, "1") == 0 || strcmp(path, "-") == 0)
    {
        g_trace.file = stderr;
    }
    else
    {
        g_trace.file = fopen(path, "w");
        if (!g_trace.file)
        {
            fprintf(stderr, "debug_trace: failed to open %s\n", path);
            return;
        }
    }

    const char *limit = getenv("PS1_TRACE_VISUAL_LIMIT");
    if (!limit || !limit[0])
        limit = getenv("PS1_TRACE_GPU_GTE_LIMIT");
    if (limit && limit[0])
        g_trace.limit = strtoull(limit, NULL, 10);
    const char *start_frame = getenv("PS1_TRACE_VISUAL_START_FRAME");
    if (!start_frame || !start_frame[0])
        start_frame = getenv("PS1_TRACE_GPU_GTE_START_FRAME");
    if (start_frame && start_frame[0])
        g_trace.start_frame = (uint32_t)strtoul(start_frame, NULL, 10);
    const char *events = getenv("PS1_TRACE_VISUAL_EVENTS");
    if (!events || !events[0])
        events = getenv("PS1_TRACE_GPU_GTE_EVENTS");
    if (events && events[0])
    {
        uint32_t mask = 0;
        if (trace_token_matches(events, "frame"))
            mask |= TRACE_EVENT_FRAME;
        if (trace_token_matches(events, "gp0"))
            mask |= TRACE_EVENT_GP0;
        if (trace_token_matches(events, "gte"))
            mask |= TRACE_EVENT_GTE;
        if (mask)
            g_trace.event_mask = mask;
    }
    g_trace.enabled = true;
}

static bool debug_trace_begin(uint32_t event)
{
    debug_trace_init();
    if (!g_trace.enabled || !g_trace.file)
        return false;
    if ((g_trace.event_mask & event) == 0)
        return false;
    if (g_trace.current_frame < g_trace.start_frame)
        return false;
    if (g_trace.limit && g_trace.count >= g_trace.limit)
        return false;
    g_trace.count++;
    return true;
}

static void debug_trace_end(void)
{
    fputc('\n', g_trace.file);
    fflush(g_trace.file);
}

static int32_t gp0_coord11(uint32_t v)
{
    v &= 0x7FFu;
    return (v & 0x400u) ? (int32_t)(v | 0xFFFFF800u) : (int32_t)v;
}

static void bbox_add(int32_t x, int32_t y, bool *valid,
                     int32_t *min_x, int32_t *min_y, int32_t *max_x, int32_t *max_y)
{
    if (!*valid)
    {
        *valid = true;
        *min_x = *max_x = x;
        *min_y = *max_y = y;
        return;
    }
    if (x < *min_x)
        *min_x = x;
    if (x > *max_x)
        *max_x = x;
    if (y < *min_y)
        *min_y = y;
    if (y > *max_y)
        *max_y = y;
}

static void bbox_add_pair(int32_t x, int32_t y, int16_t dx, int16_t dy,
                          bool *valid, int32_t *min_x, int32_t *min_y, int32_t *max_x, int32_t *max_y,
                          bool *screen_valid, int32_t *screen_min_x, int32_t *screen_min_y,
                          int32_t *screen_max_x, int32_t *screen_max_y)
{
    bbox_add(x, y, valid, min_x, min_y, max_x, max_y);
    bbox_add(x + dx, y + dy, screen_valid, screen_min_x, screen_min_y, screen_max_x, screen_max_y);
}

static void trace_gp0_bbox(const uint32_t *words, uint8_t len, int16_t draw_x_offset, int16_t draw_y_offset)
{
    if (len == 0)
        return;

    uint8_t op = (uint8_t)(words[0] >> 24);
    bool valid = false;
    int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool screen_valid = false;
    int32_t screen_min_x = 0, screen_min_y = 0, screen_max_x = 0, screen_max_y = 0;

    if (op >= 0x20 && op <= 0x3F)
    {
        uint32_t vertices = (op & 0x08u) ? 4u : 3u;
        bool textured = (op & 0x04u) != 0;
        bool shaded = (op & 0x10u) != 0;
        uint32_t idx = 1;
        for (uint32_t v = 0; v < vertices && idx < len; v++)
        {
            uint32_t xy = words[idx++];
            bbox_add_pair(gp0_coord11(xy), gp0_coord11(xy >> 16), draw_x_offset, draw_y_offset,
                          &valid, &min_x, &min_y, &max_x, &max_y,
                          &screen_valid, &screen_min_x, &screen_min_y, &screen_max_x, &screen_max_y);
            if (textured && idx < len)
                idx++;
            if (shaded && v + 1 < vertices && idx < len)
                idx++;
        }
    }
    else if ((op >= 0x40 && op <= 0x47) && len >= 3)
    {
        bbox_add_pair(gp0_coord11(words[1]), gp0_coord11(words[1] >> 16), draw_x_offset, draw_y_offset,
                      &valid, &min_x, &min_y, &max_x, &max_y,
                      &screen_valid, &screen_min_x, &screen_min_y, &screen_max_x, &screen_max_y);
        bbox_add_pair(gp0_coord11(words[2]), gp0_coord11(words[2] >> 16), draw_x_offset, draw_y_offset,
                      &valid, &min_x, &min_y, &max_x, &max_y,
                      &screen_valid, &screen_min_x, &screen_min_y, &screen_max_x, &screen_max_y);
    }
    else if ((op >= 0x50 && op <= 0x57) && len >= 4)
    {
        bbox_add_pair(gp0_coord11(words[1]), gp0_coord11(words[1] >> 16), draw_x_offset, draw_y_offset,
                      &valid, &min_x, &min_y, &max_x, &max_y,
                      &screen_valid, &screen_min_x, &screen_min_y, &screen_max_x, &screen_max_y);
        bbox_add_pair(gp0_coord11(words[3]), gp0_coord11(words[3] >> 16), draw_x_offset, draw_y_offset,
                      &valid, &min_x, &min_y, &max_x, &max_y,
                      &screen_valid, &screen_min_x, &screen_min_y, &screen_max_x, &screen_max_y);
    }
    else if (op >= 0x60 && op <= 0x7F && len >= 2)
    {
        bool textured = (op & 0x04u) != 0;
        uint32_t xy = words[1];
        int32_t x = gp0_coord11(xy);
        int32_t y = gp0_coord11(xy >> 16);
        uint32_t w = 1, h = 1;
        if ((op & 0x18u) == 0x10u)
            w = h = 8;
        else if ((op & 0x18u) == 0x18u)
            w = h = 16;
        else if (len >= 3)
        {
            uint32_t wh = words[textured ? 3 : 2];
            w = wh & 0xFFFFu;
            h = wh >> 16;
        }
        bbox_add_pair(x, y, draw_x_offset, draw_y_offset,
                      &valid, &min_x, &min_y, &max_x, &max_y,
                      &screen_valid, &screen_min_x, &screen_min_y, &screen_max_x, &screen_max_y);
        bbox_add_pair(x + (int32_t)w - 1, y + (int32_t)h - 1, draw_x_offset, draw_y_offset,
                      &valid, &min_x, &min_y, &max_x, &max_y,
                      &screen_valid, &screen_min_x, &screen_min_y, &screen_max_x, &screen_max_y);
    }
    else if ((op == 0x80 || op == 0xA0 || op == 0xC0) && len >= 3)
    {
        uint32_t xy = words[op == 0x80 ? 2 : 1];
        uint32_t wh = words[op == 0x80 ? 3 : 2];
        int32_t x = (int32_t)(xy & 0x3FFu);
        int32_t y = (int32_t)((xy >> 16) & 0x1FFu);
        uint32_t w = wh & 0xFFFFu;
        uint32_t h = wh >> 16;
        bbox_add(x, y, &valid, &min_x, &min_y, &max_x, &max_y);
        bbox_add(x + (int32_t)w - 1, y + (int32_t)h - 1, &valid, &min_x, &min_y, &max_x, &max_y);
    }

    if (valid)
        fprintf(g_trace.file, ",\"bbox\":[%d,%d,%d,%d]", min_x, min_y, max_x, max_y);
    if (screen_valid)
        fprintf(g_trace.file, ",\"bbox_screen\":[%d,%d,%d,%d]", screen_min_x, screen_min_y, screen_max_x, screen_max_y);
}

static void trace_gte_xy(const char *name, uint32_t v)
{
    int32_t x = (int32_t)(int16_t)(v & 0xFFFFu);
    int32_t y = (int32_t)(int16_t)(v >> 16);
    fprintf(g_trace.file, ",\"%s\":[%d,%d]", name, x, y);
}

static void trace_gte_selected(const char *prefix, const uint32_t dr[32], const uint32_t cr[32])
{
    fprintf(g_trace.file,
            ",\"%s_ir\":[%d,%d,%d,%d],\"%s_sz\":[%u,%u,%u,%u],\"%s_mac\":[%d,%d,%d,%d]",
            prefix,
            (int32_t)(int16_t)dr[8], (int32_t)(int16_t)dr[9],
            (int32_t)(int16_t)dr[10], (int32_t)(int16_t)dr[11],
            prefix,
            dr[16] & 0xFFFFu, dr[17] & 0xFFFFu, dr[18] & 0xFFFFu, dr[19] & 0xFFFFu,
            prefix,
            (int32_t)dr[24], (int32_t)dr[25], (int32_t)dr[26], (int32_t)dr[27]);
    trace_gte_xy(prefix[0] == 'b' ? "before_sxy0" : "after_sxy0", dr[12]);
    trace_gte_xy(prefix[0] == 'b' ? "before_sxy1" : "after_sxy1", dr[13]);
    trace_gte_xy(prefix[0] == 'b' ? "before_sxy2" : "after_sxy2", dr[14]);
    fprintf(g_trace.file, ",\"%s_flag\":\"%08X\"", prefix, cr[31]);
}

void debug_trace_frame(uint32_t frame, uint16_t display_x, uint16_t display_y,
                       uint16_t display_w, uint16_t display_h, bool display_24bit,
                       bool display_disabled, uint64_t pixels_written)
{
    debug_trace_init();
    g_trace.current_frame = frame;
    if (!debug_trace_begin(TRACE_EVENT_FRAME))
        return;
    fprintf(g_trace.file,
            "{\"event\":\"frame\",\"frame\":%u,\"display\":[%u,%u,%u,%u],\"depth\":%u,\"disabled\":%u,\"pixels_written\":%llu}",
            frame, display_x, display_y, display_w, display_h,
            display_24bit ? 24u : 15u, display_disabled ? 1u : 0u,
            (unsigned long long)pixels_written);
    debug_trace_end();
}

void debug_trace_gp0(uint32_t frame, const uint32_t *words, uint8_t len,
                     int16_t draw_x_offset, int16_t draw_y_offset,
                     uint16_t draw_left, uint16_t draw_top,
                     uint16_t draw_right, uint16_t draw_bottom)
{
    debug_trace_init();
    if (frame > g_trace.current_frame)
        g_trace.current_frame = frame;
    if (!debug_trace_begin(TRACE_EVENT_GP0))
        return;
    uint8_t op = len ? (uint8_t)(words[0] >> 24) : 0;
    fprintf(g_trace.file,
            "{\"event\":\"gp0\",\"frame\":%u,\"op\":\"%02X\",\"len\":%u,\"draw_offset\":[%d,%d],\"draw_area\":[%u,%u,%u,%u],\"words\":[",
            frame, op, len, draw_x_offset, draw_y_offset, draw_left, draw_top, draw_right, draw_bottom);
    for (uint8_t i = 0; i < len; i++)
        fprintf(g_trace.file, "%s\"%08X\"", i ? "," : "", words[i]);
    fputc(']', g_trace.file);
    trace_gp0_bbox(words, len, draw_x_offset, draw_y_offset);
    fputc('}', g_trace.file);
    debug_trace_end();
}

void debug_trace_gte(uint32_t cmd, const uint32_t before_dr[32], const uint32_t before_cr[32],
                     const uint32_t after_dr[32], const uint32_t after_cr[32])
{
    if (!debug_trace_begin(TRACE_EVENT_GTE))
        return;
    fprintf(g_trace.file, "{\"event\":\"gte\",\"frame\":%u,\"cmd\":\"%08X\",\"op\":\"%02X\"",
            g_trace.current_frame, cmd, cmd & 0x3Fu);
    trace_gte_selected("before", before_dr, before_cr);
    trace_gte_selected("after", after_dr, after_cr);
    fputc('}', g_trace.file);
    debug_trace_end();
}
