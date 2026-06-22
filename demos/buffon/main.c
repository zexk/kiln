#include "render.h"
#include "platform.h"
#include "linalg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

/* Buffon's Needle: drop needles of length L onto a ruled plane with line
   spacing t.  When L == t the crossing probability is exactly 2/pi, so
   pi ~ 2 * throws / crossings.  The demo runs the simulation live and
   accumulates statistics across visual resets. */

#define WIN_W              960
#define WIN_H              720
#define LINE_COUNT          10   /* parallel horizontal lines */
#define MAX_NEEDLES       8000   /* ring buffer; visual clears while stats grow */
#define NEEDLES_PER_FRAME   25

typedef struct { float x, y, angle; bool crossing; } needle_t;

static needle_t g_needles[MAX_NEEDLES];
static int      g_n      = 0;
static long     g_total  = 0;
static long     g_cross  = 0;

static float randf(float lo, float hi) {
    return lo + (float)rand() / ((float)RAND_MAX + 1.0f) * (hi - lo);
}

static void throw_needle(float w, float h, float spacing) {
    if (g_n >= MAX_NEEDLES) g_n = 0;   /* ring reset; g_total/g_cross keep growing */
    needle_t *n = &g_needles[g_n++];
    n->x     = randf(0.0f, w);
    n->y     = randf(0.0f, h);
    n->angle = randf(0.0f, KLN_PI);

    /* L = spacing → P(crossing) = 2/pi */
    float half_L = spacing * 0.5f;
    float y_lo   = n->y - half_L * fabsf(sinf(n->angle));
    float y_hi   = n->y + half_L * fabsf(sinf(n->angle));
    n->crossing  = (int)floorf(y_lo / spacing) != (int)floorf(y_hi / spacing);

    g_total++;
    if (n->crossing) g_cross++;
}

int main(void) {
    srand((unsigned)time(NULL));

    window_t *win = window_create("Buffon's Needle", WIN_W, WIN_H);
    if (!win) return 1;
    if (!render_init(win)) { window_destroy(win); return 1; }

    render_set_clear_color(0.07f, 0.07f, 0.09f);

    bool running = true;
    bool paused  = false;

    while (running) {
        uint32_t w = WIN_W, h = WIN_H;
        window_size(win, &w, &h);
        float fw = (float)w, fh = (float)h;
        float spacing = fh / (float)(LINE_COUNT + 1);
        float half_L  = spacing * 0.5f;

        event_t ev;
        while (window_poll_event(win, &ev)) {
            if (ev.type == EVENT_QUIT) running = false;
            if (ev.type == EVENT_KEY_DOWN) {
                if (ev.key.code == KEY_ESCAPE)                   running = false;
                if (ev.key.keysym == ' ')                        paused  = !paused;
                if (ev.key.keysym == 'r' || ev.key.keysym == 'R') {
                    g_n = 0; g_total = 0; g_cross = 0;
                }
            }
        }

        if (!paused)
            for (int i = 0; i < NEEDLES_PER_FRAME; i++)
                throw_needle(fw, fh, spacing);

        /* grid lines */
        for (int i = 1; i <= LINE_COUNT; i++)
            render_line(0.0f, spacing * (float)i,
                        fw,   spacing * (float)i,
                        1.5f, 0.22f, 0.52f, 0.65f);

        /* non-crossing needles drawn first so crossing ones render on top */
        for (int i = 0; i < g_n; i++) {
            if (g_needles[i].crossing) continue;
            float dx = half_L * cosf(g_needles[i].angle);
            float dy = half_L * sinf(g_needles[i].angle);
            render_line(g_needles[i].x - dx, g_needles[i].y - dy,
                        g_needles[i].x + dx, g_needles[i].y + dy,
                        1.0f, 0.42f, 0.46f, 0.50f);
        }
        for (int i = 0; i < g_n; i++) {
            if (!g_needles[i].crossing) continue;
            float dx = half_L * cosf(g_needles[i].angle);
            float dy = half_L * sinf(g_needles[i].angle);
            render_line(g_needles[i].x - dx, g_needles[i].y - dy,
                        g_needles[i].x + dx, g_needles[i].y + dy,
                        1.8f, 0.92f, 0.30f, 0.20f);
        }

        /* stats header */
        double pi_est = (g_cross > 0) ? 2.0 * g_total / (double)g_cross : 0.0;
        double err    = (pi_est > 0.0)
            ? 100.0 * fabs(pi_est - 3.14159265358979) / 3.14159265358979 : 0.0;
        char buf[128];
        render_rect(0.0f, 0.0f, fw, 56.0f, 0.05f, 0.05f, 0.07f);
        snprintf(buf, sizeof(buf),
                 "Buffon's Needle   pi ~ %.8f   error %.5f%%   throws %ld",
                 pi_est, err, g_total);
        render_text(12.0f, 10.0f, 2.0f, 1.0f, 1.0f, 1.0f, buf);
        render_text(12.0f, 36.0f, 1.5f, 0.40f, 0.72f, 0.52f,
                    paused ? "PAUSED    space resume   r reset   esc quit"
                           : "space pause   r reset   esc quit");

        render_draw();
    }

    render_shutdown();
    window_destroy(win);
    return 0;
}
