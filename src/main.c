/* citadel-dock — standalone dock binary that talks to Lumen via the
 * external window protocol (LUMEN_OP_CREATE_PANEL + LUMEN_OP_INVOKE).
 *
 * Splits the dock out of libcitadel (which used to be linked into Lumen
 * itself and rendered as an overlay callback). The protocol can't read
 * underlying compositor pixels, so the frosted-glass blur is dropped:
 * we render an opaque dark background instead. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#include <glyph.h>
#include <font.h>
#include <lumen_client.h>
#include "icons.h"

/* Dock geometry — must match the old dock.h numbers so the existing
 * dock_click_test geometry remains valid. */
#define DOCK_ICON_SIZE   48
#define DOCK_ICON_GAP    12
#define DOCK_PADDING_X   24
#define DOCK_PADDING_Y   16
#define DOCK_HEIGHT      (DOCK_ICON_SIZE + DOCK_PADDING_Y * 2)
#define DOCK_BG          THEME_SURFACE_2
#define DOCK_HOVER_BG    0x00FFFFFF
#define DOCK_HOVER_ALPHA 40

#define DOCK_ITEM_APPS      0   /* Applications launcher — always leftmost */
#define DOCK_ITEM_SETTINGS  1
#define DOCK_ITEM_FILES     2
#define DOCK_ITEM_TERMINAL  3
#define DOCK_ITEM_CALC      4
#define DOCK_ITEM_EDITOR    5
#define DOCK_ITEM_INSTALLER 6   /* live-boot only — appended at runtime */
#define DOCK_MAX_ITEMS      7

static const char *s_item_keys[DOCK_MAX_ITEMS] = {
    [DOCK_ITEM_APPS]      = "applications",
    [DOCK_ITEM_SETTINGS]  = "settings",
    [DOCK_ITEM_FILES]     = "files",
    [DOCK_ITEM_TERMINAL]  = "terminal",
    [DOCK_ITEM_CALC]      = "calculator",
    [DOCK_ITEM_EDITOR]    = "editor",
    [DOCK_ITEM_INSTALLER] = "installer",
};

/* invoke name sent to Lumen per item (differs from the [DOCK] debug key).
 * These match glyph_icon_draw's known ids exactly, so they double as the
 * icon ids in render_dock. */
static const char *s_item_invoke[DOCK_MAX_ITEMS] = {
    [DOCK_ITEM_APPS]      = "applications",
    [DOCK_ITEM_SETTINGS]  = "settings",
    [DOCK_ITEM_FILES]     = "filemanager",
    [DOCK_ITEM_TERMINAL]  = "terminal",
    [DOCK_ITEM_CALC]      = "calculator",
    [DOCK_ITEM_EDITOR]    = "editor",
    [DOCK_ITEM_INSTALLER] = "gui-installer",
};

static int s_nitems = 6;        /* + installer when on live media */
static int s_dock_w;
static int s_hover = -1;        /* launcher hover, -1 = none */

/* Task area: one entry per open window, to the right of the launcher icons.
 * Populated from LUMEN_EV_WINDOW_LIST; clicking an entry activates that window. */
#define TASK_SEP_W    18   /* gap + divider between launchers and the task area */
#define TASK_ENTRY_W  DOCK_ICON_SIZE   /* one app icon per open window */
#define TASK_ENTRY_GAP DOCK_ICON_GAP
#define TASK_MAX      16
static lumen_window_info_t s_wins[TASK_MAX];
static int s_nwins = 0;
static int s_win_hover = -1;    /* hovered task entry index, -1 = none */
static lumen_window_t *s_panel = NULL;   /* our panel window (for self-resize) */

/* The installer icon only appears on live boots: gen-limine-conf.sh marks
 * every live-media cmdline with aegis_live=1, and vigil removes the
 * installer binaries from installed disks on first boot. Check both the
 * marker and the binary so a half-cleaned disk never shows a dead icon. */
static int
show_installer_item(void)
{
    char cmdline[256] = "";
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    if (n <= 0) return 0;
    cmdline[n] = '\0';
    if (!strstr(cmdline, "aegis_live")) return 0;
    return access("/apps/gui-installer/gui-installer", F_OK) == 0;
}

static void
log_console(const char *msg)
{
    write(2, msg, strlen(msg));
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) { write(cfd, msg, strlen(msg)); close(cfd); }
}

/* Map a window title to a known glyph icon id. Unknown titles pass through:
 * the title becomes the id, so glyph_icon_draw draws its letter-tile fallback
 * (games keep their distinct letters that way). */
static const char *
title_icon_id(const char *title)
{
    static const struct { const char *title, *id; } map[] = {
        { "Applications",    "applications" },
        { "Settings",        "settings" },
        { "Files",           "filemanager" },
        { "Terminal",        "terminal" },
        { "Calculator",      "calculator" },
        { "Text Editor",     "editor" },
        { "System Monitor",  "sysmon" },
        { "Network Manager", "netman" },
        { "Calendar",        "calendar" },
        { "Images",          "imageviewer" },
        { "Tunes",           "tunes" },
        { "Run",             "run" },
        { "Snake",           "snake" },
        { "Minesweeper",     "minesweeper" },
        /* "2048" title == its icon id, so it passes through. */
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(title, map[i].title) == 0)
            return map[i].id;
    return title;
}

/* Bridge surface_t expected by glyph draw_* over the lumen window backbuf. */
static surface_t
backbuf_surface(lumen_window_t *win)
{
    surface_t s;
    s.buf   = (uint32_t *)win->backbuf;
    s.w     = win->w;
    s.h     = win->h;
    s.pitch = win->stride;
    return s;
}

/* Local-coord rect of icon i within the dock surface. */
static void
item_rect(int i, int *ix, int *iy)
{
    *ix = DOCK_PADDING_X + i * (DOCK_ICON_SIZE + DOCK_ICON_GAP);
    *iy = DOCK_PADDING_Y;
}

/* Right edge of the launcher icon row (local x). */
static int
launchers_right(void)
{
    return DOCK_PADDING_X + s_nitems * DOCK_ICON_SIZE +
           (s_nitems - 1) * DOCK_ICON_GAP;
}

/* Local x of task entry i (the window at s_wins[i]). */
static int
task_entry_x(int i)
{
    return launchers_right() + TASK_SEP_W + i * (TASK_ENTRY_W + TASK_ENTRY_GAP);
}

/* Total dock width for the current launcher + task-entry counts. */
static int
dock_width(void)
{
    int w = launchers_right();
    if (s_nwins > 0)
        w += TASK_SEP_W + s_nwins * TASK_ENTRY_W +
             (s_nwins - 1) * TASK_ENTRY_GAP;
    return w + DOCK_PADDING_X;
}

/* Vertical color lerp a→b over [0,n). */
static uint32_t
dock_lerp(uint32_t a, uint32_t b, int t, int n)
{
    if (n <= 1) return a;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * t / (n - 1);
    int g = ag + (bg - ag) * t / (n - 1);
    int bl = ab + (bb - ab) * t / (n - 1);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static void
dput(surface_t *s, int x, int y, uint32_t c)
{
    if (x >= 0 && x < s->w && y >= 0 && y < s->h)
        s->buf[y * s->pitch + x] = c;
}

/* A 1px directional rim that follows the compositor's rounded corners EXACTLY
 * (same corner centers + dist<=r arc), so it can never be clipped into a dotted
 * edge. Color is a vertical gradient: a light-grey highlight at the top flowing
 * to a near-black shadow at the bottom, so the sides sit at a medium tone and
 * the whole rim flows smoothly around the perimeter (top bright > sides > bottom
 * darkest). */
static void
dock_rim(surface_t *s, int w, int h, int r)
{
    const uint32_t top = 0x00828C9E, bot = 0x00070A10;
    for (int x = r; x < w - r; x++) {
        dput(s, x, 0,     dock_lerp(top, bot, 0,     h));
        dput(s, x, h - 1, dock_lerp(top, bot, h - 1, h));
    }
    for (int y = r; y < h - r; y++) {
        uint32_t c = dock_lerp(top, bot, y, h);
        dput(s, 0, y, c);
        dput(s, w - 1, y, c);
    }
    /* Corner arcs: draw the OUTER boundary of the kept region (a pixel that is
     * inside the arc but has a neighbour one step further out that is cut). This
     * is gap-free and pixel-aligned with round_window_corner. */
    const int cx[4] = { r, w - r - 1, r, w - r - 1 };
    const int cy[4] = { r, r, h - r - 1, h - r - 1 };
    const int ox[4] = { 0, w - r, 0, w - r };
    const int oy[4] = { 0, 0, h - r, h - r };
    for (int cc = 0; cc < 4; cc++)
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                int x = ox[cc] + i, y = oy[cc] + j;
                int dx = x - cx[cc], dy = y - cy[cc];
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if (dx * dx + dy * dy <= r * r &&
                    ((adx + 1) * (adx + 1) + ady * ady > r * r ||
                     adx * adx + (ady + 1) * (ady + 1) > r * r))
                    dput(s, x, y, dock_lerp(top, bot, y, h));
            }
}

static void
render_dock(lumen_window_t *win)
{
    surface_t s = backbuf_surface(win);
    int w = s.w, h = s.h;

    /* Frosted glass dock: fill with the compositor's frost key (C_TERM_BG) so
     * Lumen blurs + tints the desktop behind it — the compositor also rounds the
     * panel corners (all four). Over the frost: a directional rim that follows
     * those exact corners (no dotted clipping), plus a faint inner top sheen for
     * glass. */
    draw_fill_rect(&s, 0, 0, w, h, C_TERM_BG);
    dock_rim(&s, w, h, R_MD);
    draw_blend_rect(&s, R_MD, 1, w - 2 * R_MD, 1, 0x00FFFFFF, 28);

    for (int i = 0; i < s_nitems; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        if (i == s_hover) {
            draw_blend_rounded_rect(&s, ix - 4, iy - 4,
                                    DOCK_ICON_SIZE + 8, DOCK_ICON_SIZE + 8,
                                    R_SM + 2, DOCK_HOVER_BG, DOCK_HOVER_ALPHA);
        }
        /* Shared libglyph icons; the invoke name is the icon id
         * (s_item_keys[] holds the [DOCK] debug keys, not all of which
         * glyph_icon_draw knows — e.g. "installer"). */
        glyph_icon_draw(&s, s_item_invoke[i], NULL, ix, iy, DOCK_ICON_SIZE);
    }

    /* Task area: a divider, then one app icon per open window with a
     * macOS-style running dot beneath it (accent when focused). Minimized
     * windows are still running, so the icon is dimmed but keeps its dot. */
    if (s_nwins > 0) {
        int divx = launchers_right() + TASK_SEP_W / 2;
        draw_blend_rect(&s, divx, DOCK_PADDING_Y, 1, DOCK_ICON_SIZE, 0x00FFFFFF, 30);
        for (int i = 0; i < s_nwins; i++) {
            const lumen_window_info_t *w = &s_wins[i];
            int ex = task_entry_x(i), ey = DOCK_PADDING_Y;
            if (i == s_win_hover)
                draw_blend_rounded_rect(&s, ex - 4, ey - 4,
                                        DOCK_ICON_SIZE + 8, DOCK_ICON_SIZE + 8,
                                        R_SM + 2, DOCK_HOVER_BG, DOCK_HOVER_ALPHA);
            glyph_icon_draw(&s, title_icon_id(w->title), w->title,
                            ex, ey, DOCK_ICON_SIZE);
            if (w->minimized)
                draw_blend_rounded_rect(&s, ex, ey, DOCK_ICON_SIZE,
                                        DOCK_ICON_SIZE, R_SM, 0x00000000, 120);
            draw_circle_filled(&s, ex + DOCK_ICON_SIZE / 2,
                               ey + DOCK_ICON_SIZE + 4, 3,
                               w->focused ? THEME_ACCENT : THEME_TEXT_DIM);
        }
    }
}

static int
hit_test(int lx, int ly)
{
    if (lx < 0 || lx >= s_dock_w || ly < 0 || ly >= DOCK_HEIGHT)
        return -1;
    for (int i = 0; i < s_nitems; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        if (lx >= ix && lx < ix + DOCK_ICON_SIZE &&
            ly >= iy && ly < iy + DOCK_ICON_SIZE)
            return i;
    }
    return -1;
}

/* Task-entry index at (lx,ly), or -1. */
static int
hit_window(int lx, int ly)
{
    if (ly < DOCK_PADDING_Y || ly >= DOCK_PADDING_Y + DOCK_ICON_SIZE)
        return -1;
    for (int i = 0; i < s_nwins; i++) {
        int ex = task_entry_x(i);
        if (lx >= ex && lx < ex + TASK_ENTRY_W)
            return i;
    }
    return -1;
}

/* Adopt a new window list from the compositor: copy it, resize the panel to fit
 * the task area, and repaint. */
static void
apply_window_list(const lumen_window_info_t *items, int count)
{
    if (count > TASK_MAX) count = TASK_MAX;
    s_nwins = count;
    for (int i = 0; i < count; i++)
        s_wins[i] = items[i];
    s_win_hover = -1;

    int neww = dock_width();
    if (s_panel && neww != s_panel->w &&
        lumen_window_resize_self(s_panel, neww, DOCK_HEIGHT) == 0)
        s_dock_w = neww;

    if (s_panel) {
        render_dock(s_panel);
        lumen_window_present(s_panel);
    }
}

/* Emit the same [DOCK] debug lines the old in-process dock did so that
 * dock_click_test (which parses serial for icon centers) keeps working. */
static void
emit_debug_lines(int dock_screen_x, int dock_screen_y)
{
    char buf[160];
    for (int i = 0; i < s_nitems; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        int cx = dock_screen_x + ix + DOCK_ICON_SIZE / 2;
        int cy = dock_screen_y + iy + DOCK_ICON_SIZE / 2;
        int hw = DOCK_ICON_SIZE / 2;
        int hh = DOCK_ICON_SIZE / 2;
        int n = snprintf(buf, sizeof(buf),
                         "[DOCK] item=%s idx=%d cx=%d cy=%d hw=%d hh=%d\n",
                         s_item_keys[i], i, cx, cy, hw, hh);
        if (n > 0) log_console(buf);
    }
    log_console("[DOCK] ready\n");
}

int main(void)
{
    /* Wait forever for Lumen. Vigil starts citadel-dock at boot under
     * graphical mode, but Lumen doesn't bind /run/lumen.sock until
     * Bastion authenticates the user — which can be any number of
     * minutes later. Exiting on timeout would burn vigil restart credits
     * for no reason. Loop indefinitely instead. */
    int fd = -1;
    for (;;) {
        fd = lumen_connect();
        if (fd >= 0) break;
        if (fd != -111) {  /* ECONNREFUSED is expected; anything else is fatal */
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                "[DOCK] lumen_connect=%d (giving up)\n", fd);
            if (n > 0) log_console(buf);
            return 1;
        }
        struct timespec ts = { 0, 200 * 1000 * 1000 };  /* 200ms */
        nanosleep(&ts, NULL);
    }

    /* Initialize TTF font renderer so terminal icon ">_" looks right. */
    font_init();

    if (show_installer_item())
        s_nitems = DOCK_ITEM_INSTALLER + 1;

    s_dock_w = DOCK_PADDING_X * 2 +
               s_nitems * DOCK_ICON_SIZE +
               (s_nitems - 1) * DOCK_ICON_GAP;

    lumen_window_t *win = lumen_panel_create(fd, s_dock_w, DOCK_HEIGHT);
    if (!win) {
        log_console("[DOCK] FAIL: panel_create returned NULL\n");
        close(fd);
        return 1;
    }
    s_panel = win;   /* for apply_window_list's self-resize */

    /* dock_click_test parses [DOCK] lines for icon SCREEN-centers.
     * lumen_window_created_t reply now carries the panel's screen
     * position (Lumen places panels at bottom-center). */
    emit_debug_lines(win->x, win->y);

    render_dock(win);
    lumen_window_present(win);

    /* Event loop */
    for (;;) {
        lumen_event_t ev;
        int r = lumen_wait_event(fd, &ev, -1);  /* block forever */
        if (r < 0) {
            log_console("[DOCK] connection lost\n");
            break;
        }
        if (r == 0) continue;

        switch (ev.type) {
        case LUMEN_EV_MOUSE: {
            int item = hit_test(ev.mouse.x, ev.mouse.y);
            int wi   = (item < 0) ? hit_window(ev.mouse.x, ev.mouse.y) : -1;
            if (ev.mouse.evtype == LUMEN_MOUSE_MOVE) {
                if (item != s_hover || wi != s_win_hover) {
                    s_hover = item;
                    s_win_hover = wi;
                    render_dock(win);
                    lumen_window_present(win);
                }
            } else if (ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                       (ev.mouse.buttons & 1)) {
                if (item >= 0 && item < s_nitems)
                    lumen_invoke(fd, s_item_invoke[item]);
                else if (wi >= 0 && wi < s_nwins)
                    lumen_activate_window(fd, s_wins[wi].gid);
            }
            break;
        }
        case LUMEN_EV_WINDOW_LIST:
            apply_window_list((const lumen_window_info_t *)ev.windows.items,
                              ev.windows.count);
            break;
        case LUMEN_EV_CLOSE_REQUEST:
            /* Ignore — dock is unkillable from the UI. */
            break;
        default:
            break;
        }
    }

    lumen_window_destroy(win);
    close(fd);
    return 0;
}
