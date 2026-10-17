// A Pane client for the phase 12 acceptance test (started by `pane --test`)
// and a small demo: a window with a blue field, a red 10x10 square after the
// first frame, then a few more windows when run by hand (`panetest demo`).
#include <cerberus.h>
#include "../libpane/pane.h"

static void fill(pane::Window* w, int x, int y, int cw, int ch, uint32_t c) {
    for (int j = y; j < y + ch && j < w->height; j++)
        for (int i = x; i < x + cw && i < w->width; i++) w->pixels[j * w->stride + i] = c;
}

int main(int argc, char** argv) {
    bool demo = argc > 1 && strcmp(argv[1], "demo") == 0;
    pane::Connection c;
    if (!c.connect("panetest")) {
        printf("panetest: no window server (%s)\n", strerror(errno));
        return 1;
    }
    pane::Window* w = c.create_window(320, 200, "Hello from a program");
    if (!w) {
        printf("panetest: could not create a window\n");
        return 1;
    }
    fill(w, 0, 0, w->width, w->height, 0xFF2040C0);         // blue
    c.damage(w, 0, 0, w->width, w->height);
    c.commit(w);

    pane::Event e;
    int frames = 0;
    bool red_done = false;
    pane::Window* extra[3] = {};
    // The test gives up after 5 s without events; the demo stays until
    // its first window is closed or Pane goes away.
    while (c.next_event(&e, demo ? ~0ull : 5000)) {
        if (e.type == pane::EVT_DISCONNECTED) break;
        if (e.type == pane::EVT_FRAME && e.window == w) {
            frames++;
            if (!red_done) {
                fill(w, 20, 20, 10, 10, 0xFFE03030);            // red
                c.damage(w, 20, 20, 10, 10);
                c.commit(w);
                red_done = true;
            } else if (!demo) {
                break;                                          // the test needs two frames
            } else if (!extra[0]) {
                static const uint32_t colours[3] = {0xFF208060, 0xFF806020, 0xFF602080};
                static const char* titles[3] = {"Second window", "Third window", "Fourth window"};
                for (int i = 0; i < 3; i++) {
                    extra[i] = c.create_window(240 + i * 40, 160, titles[i]);
                    if (!extra[i]) continue;
                    fill(extra[i], 0, 0, extra[i]->width, extra[i]->height, colours[i]);
                    c.damage(extra[i], 0, 0, extra[i]->width, extra[i]->height);
                    c.commit(extra[i]);
                }
            }
        }
        if (e.type == pane::EVT_CLOSE) {
            if (e.window == w) break;
            c.destroy_window(e.window);
        }
        if (e.type == pane::EVT_CONFIGURE && e.window && (e.w != e.window->width || e.h != e.window->height)) {
            // resized by the user: a new buffer, drawn again
            pane::Window* win = e.window;
            uint32_t colour = win == w ? 0xFF2040C0 : win->pixels ? win->pixels[0] : 0xFF208060;
            if (c.resize(win, e.w, e.h)) {
                fill(win, 0, 0, win->width, win->height, colour);
                if (win == w) fill(win, 20, 20, 10, 10, 0xFFE03030);
                c.damage(win, 0, 0, win->width, win->height);
                c.commit(win);
            }
        }
        if (e.type == pane::EVT_KEY && e.pressed && e.unicode == 'q') break;
    }
    printf("panetest: %d frame(s) shown, done\n", frames);
    c.close();
    return frames >= 2 ? 0 : 1;
}
