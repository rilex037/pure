/* ui_core.c - window, input handling, hotkeys, menu, timer, playlist */
#include "player.h"
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

Player g_player;

static int S_(int v)
{
    return (int)(((int64_t)v * g_player.dpi + (v >= 0 ? 48 : -48)) / 96);
}

/* ---------------- playlist ---------------- */

static void pl_append(Player *p, const char *f)
{
    if (p->pl_n == p->pl_cap) {
        p->pl_cap = p->pl_cap ? p->pl_cap * 2 : 16;
        p->pl = (char (*)[PATHLEN])realloc(p->pl, (size_t)p->pl_cap * PATHLEN);
    }
    if (!p->pl)
        return;
    SDL_strlcpy(p->pl[p->pl_n], f, PATHLEN);
    p->pl_n++;
}

/* insert a file at position `at` (clamped), growing the list as needed */
static void pl_insert(Player *p, int at, const char *f)
{
    if (at < 0)
        at = 0;
    if (at > p->pl_n)
        at = p->pl_n;
    int n0 = p->pl_n;
    pl_append(p, f);                    /* grows the buffer, adds at the end */
    if (p->pl_n == n0)
        return;                         /* out of memory */
    if (at < p->pl_n - 1) {
        char tmp[PATHLEN];
        SDL_strlcpy(tmp, p->pl[p->pl_n - 1], PATHLEN);
        memmove(p->pl[at + 1], p->pl[at], (size_t)(p->pl_n - 1 - at) * PATHLEN);
        SDL_strlcpy(p->pl[at], tmp, PATHLEN);
    }
}

static void set_title(Player *p)
{
    if (p->playing && p->pl_n > 0) {
        const char *path = p->pl[p->pl_cur];
        const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
        const char *f = a > b ? a : b;
        SDL_strlcpy(p->title, f ? f + 1 : path, sizeof p->title);
    } else {
        SDL_strlcpy(p->title, APP_TITLE, sizeof p->title);
    }
    SDL_SetWindowTitle(p->win, p->title);
}

/* music icon for audio-only files, video icon otherwise (and by default) */
static void set_app_icon(Player *p)
{
    int audio = (p->playing && p->vs < 0);
    if (audio == p->ico_cur)
        return;
    p->ico_cur = audio;
    plat_window_icon(p, audio);
}

/* ---------------- small ui helpers ---------------- */

void ui_toast(Player *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(p->toast, sizeof p->toast, fmt, ap);
    va_end(ap);
    p->toast_until = SDL_GetTicks() + 2200;
    ui_invalidate(p, 2, 0);
}

static void toggle_pause(Player *p)
{
    if (p->paused && ff_at_end(p))
        ff_seek(p, 0);              /* play after the end = start over */
    ff_set_paused(p, !p->paused);
    ui_invalidate(p, 0, 2);
}

static void do_stop(Player *p)
{
    ff_seek(p, 0);
    ff_set_paused(p, 1);
    ui_invalidate(p, 1, 1);
}

static void toggle_loop(Player *p)
{
    p->loop_file = !p->loop_file;
    ui_invalidate(p, 0, 2);
}

static void set_volume(Player *p, double v)
{
    if (v < 0)
        v = 0;
    if (v > 1)
        v = 1;
    p->volume = v;
    if (v > 0 && p->muted)
        p->muted = 0;
    ff_set_volume(p);
    ui_invalidate(p, 0, 2);
}

static void toggle_mute(Player *p)
{
    p->muted = !p->muted;
    ff_set_volume(p);
    ui_invalidate(p, 0, 2);
}

static void do_save(Player *p)
{
    char name[96];
    if (ff_save_frame_png(p, name, 96) == 0)
        ui_toast(p, "Saved to Pictures: %s", name);
    else
        ui_toast(p, "Could not save frame");
}

static void do_copy(Player *p)
{
    if (ff_copy_frame(p) == 0)
        ui_toast(p, "Frame copied to clipboard");
    else
        ui_toast(p, "Copy failed");
}

static void step_frame(Player *p, int back)
{
    ff_set_paused(p, 1);
    if (back)
        ff_step_back(p);
    else
        ff_step_fwd(p);
    ui_invalidate(p, 1, 1);
}

/* show the frame under the knob (called on mouse move and from the timer so
 * the last position is rendered even when the mouse has stopped) */
static void scrub_to_knob(Player *p)
{
    Uint64 now = SDL_GetTicks();
    if (p->seek_req || p->seek_pending)
        return;
    if (now - p->last_scrub < 8 || fabs(p->drag_pos - p->scrub_sent) < 0.004)
        return;
    p->scrub_sent = p->drag_pos;
    p->last_scrub = now;
    if (ff_scrub(p, p->drag_pos))
        ui_invalidate(p, 1, 0);
}

static void seek_rel(Player *p, double d)
{
    if (p->playing)
        ff_seek(p, ff_pos(p) + d);
}

/* window coordinates (float, window units) -> client pixels */
static void to_px(const Player *p, float fx, float fy, int *x, int *y)
{
    *x = (int)floorf(fx * p->density);
    *y = (int)floorf(fy * p->density);
}

/* ---------------- fullscreen / window fitting ---------------- */

static void toggle_fullscreen(Player *p)
{
    p->fullscreen = !p->fullscreen;
    p->bars_visible = 1;
    p->last_input = SDL_GetTicks();
    SDL_SetWindowFullscreen(p->win, p->fullscreen ? true : false);
    ui_invalidate_all(p);
}

static void toggle_maximize(Player *p)
{
    if (p->fullscreen)
        return;
    if (SDL_GetWindowFlags(p->win) & SDL_WINDOW_MAXIMIZED)
        SDL_RestoreWindow(p->win);
    else
        SDL_MaximizeWindow(p->win);
}

static void set_min_size(Player *p)
{
    SDL_SetWindowMinimumSize(p->win,
        (int)(S_(L_MIN_W) / p->density + 0.5f),
        (int)((ui_cap_h(p) + S_(120)) / p->density + 0.5f));
}

/* resize the window so the video fills it exactly (no black bars) */
static void fit_window(Player *p)
{
    SDL_Window *h = p->win;
    SDL_Rect wa;
    float den = p->density;
    if (p->fullscreen || (SDL_GetWindowFlags(h) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) ||
        p->vw <= 0 || p->vdar <= 0)
        return;
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(h), &wa))
        return;
    int ww = (int)(wa.w * den);
    int wh = (int)(wa.h * den);
    int cap = ui_cap_h(p);
    double dar = p->vdar;
    double vh = p->vh, vw = vh * dar;
    double maxw = ww * 0.85, maxh = wh * 0.85 - cap;
    double sc = 1.0;
    if (vw > maxw)
        sc = maxw / vw;
    if (vh * sc > maxh)
        sc = maxh / vh;
    vw *= sc;
    vh *= sc;
    if (vw < S_(640)) {                 /* small videos: show them larger */
        vw = S_(640);
        vh = vw / dar;
        if (vh > maxh) {
            vh = maxh;
            vw = vh * dar;
        }
    }
    if (vw < S_(L_MIN_W)) {
        vw = S_(L_MIN_W);
        vh = vw / dar;
    }
    int W = (int)(vw + 0.5), H = (int)(vh + 0.5) + cap;
    int pw = (int)(W / den + 0.5), ph = (int)(H / den + 0.5);
    int x, y, cw, ch;
    SDL_GetWindowPosition(h, &x, &y);
    SDL_GetWindowSize(h, &cw, &ch);
    x += cw / 2 - pw / 2;
    y += ch / 2 - ph / 2;
    if (x + pw > wa.x + wa.w) x = wa.x + wa.w - pw;
    if (y + ph > wa.y + wa.h) y = wa.y + wa.h - ph;
    if (x < wa.x) x = wa.x;
    if (y < wa.y) y = wa.y;
    SDL_SetWindowSize(h, pw, ph);
    SDL_SetWindowPosition(h, x, y);
}

/* ---------------- file opening ---------------- */

static void play_open(Player *p, const char *path)
{
    if (ff_open(p, path) < 0)
        ui_toast(p, "Could not open this file");
    set_title(p);
    set_app_icon(p);
    ui_invalidate(p, 1, 1);
}

/* Play the given files right now but keep what was played before: they are
 * inserted after the current entry, so Previous goes back to it and Next
 * continues with whatever was queued behind it. */
static void open_list(Player *p, char (*list)[PATHLEN], int n)
{
    int i, at;
    if (n <= 0)
        return;
    if (n == 1 && p->pl_n > 0 && p->playing &&
        SDL_strcasecmp(p->pl[p->pl_cur], list[0]) == 0) {
        ff_seek(p, 0);                  /* same file again: just restart it */
        ff_set_paused(p, 0);
        ui_invalidate(p, 1, 1);
        return;
    }
    at = (p->pl_n > 0) ? p->pl_cur + 1 : 0;
    for (i = 0; i < n; i++)
        pl_insert(p, at + i, list[i]);
    p->pl_cur = at;
    play_open(p, p->pl[at]);
}

/* "a\0b\0\0" -> list, then play */
static void open_packed(Player *p, const char *w)
{
    int n = 0, k = 0;
    const char *q;
    char (*list)[PATHLEN];
    for (q = w; *q; q += strlen(q) + 1)
        n++;
    if (n <= 0)
        return;
    list = (char (*)[PATHLEN])calloc((size_t)n, PATHLEN);
    if (!list)
        return;
    for (q = w; *q && k < n; q += strlen(q) + 1)
        SDL_strlcpy(list[k++], q, PATHLEN);
    open_list(p, list, k);
    free(list);
}

static void raise_window(Player *p)
{
    if (SDL_GetWindowFlags(p->win) & SDL_WINDOW_MINIMIZED)
        SDL_RestoreWindow(p->win);
    SDL_RaiseWindow(p->win);
}

static void SDLCALL dialog_cb(void *ud, const char *const *list, int filter)
{
    size_t tot = 1, pos = 0;
    int i;
    char *buf;
    (void)ud;
    (void)filter;
    if (!list || !list[0])
        return;
    for (i = 0; list[i]; i++)
        tot += strlen(list[i]) + 1;
    buf = (char *)calloc(tot, 1);
    if (!buf)
        return;
    for (i = 0; list[i]; i++) {
        size_t l = strlen(list[i]);
        memcpy(buf + pos, list[i], l);
        pos += l + 1;
    }
    plat_post_open(buf, tot);
    free(buf);
}

#define EXT_VIDEO "mp4;m4v;mkv;webm;avi;mov;flv;wmv;ts;mts;mpg;mpeg;3gp;ogv;m2ts;vob"
#define EXT_AUDIO "mp3;flac;wav;m4a;aac;opus;ogg;wma"

static void open_dialog(Player *p)
{
    static const SDL_DialogFileFilter flt[] = {
        { "Media files", EXT_VIDEO ";" EXT_AUDIO },
        { "Video", EXT_VIDEO },
        { "Audio", EXT_AUDIO },
        { "All files", "*" },
    };
    SDL_ShowOpenFileDialog(dialog_cb, NULL, p->win, flt, 4, NULL, true);
}

/* previous / next entry of the played-files list; no wrap-around */
static int can_go(const Player *p, int dir)
{
    int t = p->pl_cur + dir;
    return p->pl_n > 0 && t >= 0 && t < p->pl_n;
}

static void go_next(Player *p, int dir)
{
    if (!can_go(p, dir))
        return;
    p->pl_cur += dir;
    play_open(p, p->pl[p->pl_cur]);
}

/* ---------------- menu ---------------- */

enum { M_COPY = 1, M_SAVE, M_PLAY, M_FFW, M_FBW, M_LOOP, M_MUTE, M_FULL,
       M_OPEN, M_PREV, M_NEXT, M_EXIT, M_CTRL };

static void do_menu(Player *p, int id)
{
    switch (id) {
    case M_COPY:  do_copy(p); break;
    case M_SAVE:  do_save(p); break;
    case M_PLAY:  toggle_pause(p); break;
    case M_FFW:   step_frame(p, 0); break;
    case M_FBW:   step_frame(p, 1); break;
    case M_LOOP:  toggle_loop(p); break;
    case M_MUTE:  toggle_mute(p); break;
    case M_FULL:  toggle_fullscreen(p); break;
    case M_OPEN:  open_dialog(p); break;
    case M_PREV:  go_next(p, -1); break;
    case M_NEXT:  go_next(p, 1); break;
    case M_CTRL:  p->ctl_hidden = !p->ctl_hidden; ui_invalidate_all(p); break;
    case M_EXIT:  p->want_quit = 1; break;
    default: break;
    }
}

/* "label\taccel" */
static void mi_add(Player *p, int id, const char *text, int flags)
{
    MenuItem *m;
    const char *tab;
    if (p->menu_n >= MENU_MAX)
        return;
    m = &p->menu[p->menu_n++];
    memset(m, 0, sizeof *m);
    m->id = id;
    m->flags = flags;
    tab = strchr(text, '\t');
    if (tab) {
        size_t l = (size_t)(tab - text);
        if (l >= sizeof m->label)
            l = sizeof m->label - 1;
        memcpy(m->label, text, l);
        SDL_strlcpy(m->accel, tab + 1, sizeof m->accel);
    } else {
        SDL_strlcpy(m->label, text, sizeof m->label);
    }
}

static void mi_sep(Player *p) { mi_add(p, 0, "", MI_SEP); }

static void menu_close(Player *p)
{
    if (!p->menu_open)
        return;
    p->menu_open = 0;
    ui_invalidate_rect(p, &p->menu_rc);
    p->last_input = SDL_GetTicks();
}

static void show_menu(Player *p, int x, int y, int context)
{
    int frame_ok = (p->playing && p->shown_slot >= 0);
    int fg = frame_ok ? 0 : MI_DISABLED;
    int pg = p->playing ? 0 : MI_DISABLED;
    p->menu_n = 0;
    if (context) {
        mi_add(p, M_COPY, "Copy video frame\tCtrl+C", fg);
        mi_add(p, M_SAVE, "Save frame to Pictures\tS", fg);
        mi_sep(p);
        mi_add(p, M_PLAY, p->paused ? "Play\tSpace" : "Pause\tSpace", pg);
        mi_add(p, M_FFW, "Frame forward\t.", pg);
        mi_add(p, M_FBW, "Frame back\t,", pg);
        mi_sep(p);
        mi_add(p, M_LOOP, "Loop this file", p->loop_file ? MI_CHECKED : 0);
        mi_add(p, M_MUTE, "Mute\tM", p->muted ? MI_CHECKED : 0);
        mi_add(p, M_FULL, "Fullscreen\tF", p->fullscreen ? MI_CHECKED : 0);
        mi_add(p, M_CTRL, "Show controls", p->ctl_hidden ? 0 : MI_CHECKED);
        mi_sep(p);
        mi_add(p, M_OPEN, "Open file...\tO", 0);
        mi_add(p, M_PREV, "Previous file", can_go(p, -1) ? 0 : MI_DISABLED);
        mi_add(p, M_NEXT, "Next file", can_go(p, 1) ? 0 : MI_DISABLED);
        mi_sep(p);
        mi_add(p, M_EXIT, "Exit", 0);
    } else {
        mi_add(p, M_OPEN, "Open file...\tO", 0);
        mi_sep(p);
        mi_add(p, M_SAVE, "Save frame to Pictures", fg);
        mi_add(p, M_COPY, "Copy video frame", fg);
        mi_sep(p);
        mi_add(p, M_CTRL, "Show controls", p->ctl_hidden ? 0 : MI_CHECKED);
        mi_add(p, M_LOOP, "Loop this file", p->loop_file ? MI_CHECKED : 0);
        mi_add(p, M_FULL, "Fullscreen\tF", p->fullscreen ? MI_CHECKED : 0);
        mi_sep(p);
        mi_add(p, M_EXIT, "Exit", 0);
    }
    ui_menu_place(p, x, y, !context);
    p->menu_open = 1;
    ui_invalidate_rect(p, &p->menu_rc);
}

static void menu_activate(Player *p, int idx)
{
    int id;
    if (idx < 0 || idx >= p->menu_n)
        return;
    id = p->menu[idx].id;
    menu_close(p);
    do_menu(p, id);
}

static void menu_move(Player *p, int dir)
{
    int i = p->menu_sel, k;
    if (i < 0)
        i = dir > 0 ? -1 : p->menu_n;
    for (k = 0; k < p->menu_n; k++) {
        i += dir;
        if (i >= p->menu_n)
            i = 0;
        if (i < 0)
            i = p->menu_n - 1;
        if (!(p->menu[i].flags & (MI_SEP | MI_DISABLED))) {
            p->menu_sel = i;
            ui_invalidate_rect(p, &p->menu_rc);
            return;
        }
    }
}

/* ---------------- timer ---------------- */

static int g_ss_inhibit;

static void on_timer(Player *p)
{
    int rep = ff_tick(p);
    Uint64 now = SDL_GetTicks();

    if (p->dragging == 1)
        scrub_to_knob(p);

    if (p->playing && !p->paused && ff_eop(p)) {
        if (p->loop_file)
            ff_seek(p, 0);
        else if (can_go(p, 1))
            go_next(p, 1);
        else
            ff_set_paused(p, 1);
        ui_invalidate(p, 1, 1);
    }

    if (p->media_gen != p->ui_last_gen) {       /* new media: fit the window */
        p->ui_last_gen = p->media_gen;
        set_title(p);
        set_app_icon(p);
        fit_window(p);
        ui_invalidate(p, 1, 1);
    }

    if (p->pl_cur != p->ui_last_cur || p->playing != p->ui_last_playing ||
        p->pl_n != p->ui_last_n) {
        p->ui_last_cur = p->pl_cur;
        p->ui_last_playing = p->playing;
        p->ui_last_n = p->pl_n;
        set_title(p);
        set_app_icon(p);
        ui_invalidate(p, 0, 2);         /* prev / next enabled state */
    }

    if (p->playing) {
        double cur = (p->dragging == 1) ? p->drag_pos : ff_pos(p);
        char a[32], b[32], tt[64];
        ui_fmt_time(a, 32, cur);
        if (p->dur > 0) {
            ui_fmt_time(b, 32, p->dur);
            snprintf(tt, sizeof tt, "%s/%s", a, b);
        } else {
            snprintf(tt, sizeof tt, "%s", a);
        }
        int q = (int)(cur * 15.0);              /* keeps the seekbar smooth */
        if (strcmp(tt, p->time_txt) != 0 || q != p->ui_last_q) {
            SDL_strlcpy(p->time_txt, tt, sizeof p->time_txt);
            p->ui_last_q = q;
            ui_invalidate(p, 0, 2);
        }
    }

    if (p->vol_show_until && p->vol_show_until <= now) {
        p->vol_show_until = 0;
        ui_invalidate(p, 0, 2);
    }

    if (p->toast[0] && now > p->toast_until) {
        p->toast[0] = 0;
        ui_invalidate(p, 2, 0);
    }

    if (p->fullscreen && p->bars_visible && !p->menu_open && !p->dragging &&
        (p->hover == HT_NONE || p->hover == HT_VIDEO) &&
        now - p->last_input > 2500) {
        p->bars_visible = 0;
        ui_invalidate(p, 1, 1);
    }

    {   /* no cursor over a fullscreen picture; no screensaver while playing */
        int hide = p->fullscreen && !p->bars_visible && !p->menu_open;
        int ss = p->playing && !p->paused && p->vs >= 0;
        if (hide != p->cursor_hidden) {
            if (hide)
                SDL_HideCursor();
            else
                SDL_ShowCursor();
            p->cursor_hidden = hide;
        }
        if (ss != g_ss_inhibit) {
            if (ss)
                SDL_DisableScreenSaver();
            else
                SDL_EnableScreenSaver();
            g_ss_inhibit = ss;
        }
    }

    if (rep)
        ui_invalidate(p, 1, 0);
}

/* ---------------- window hit test (drag / resize of the borderless window) ---------------- */

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *w, const SDL_Point *a, void *d)
{
    Player *p = &g_player;
    int x = (int)(a->x * p->density), y = (int)(a->y * p->density);
    int W, H, e, c, hit;
    (void)d;
    if (p->fullscreen)
        return SDL_HITTEST_NORMAL;
    SDL_GetWindowSizeInPixels(w, &W, &H);
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED)) {
        e = S_(L_EDGE);
        c = e * 3;
        if (y < e)
            return x < c ? SDL_HITTEST_RESIZE_TOPLEFT :
                   x >= W - c ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
        if (y >= H - e)
            return x < c ? SDL_HITTEST_RESIZE_BOTTOMLEFT :
                   x >= W - c ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_BOTTOM;
        if (x < e)
            return y < c ? SDL_HITTEST_RESIZE_TOPLEFT :
                   y >= H - c ? SDL_HITTEST_RESIZE_BOTTOMLEFT : SDL_HITTEST_RESIZE_LEFT;
        if (x >= W - e)
            return y < c ? SDL_HITTEST_RESIZE_TOPRIGHT :
                   y >= H - c ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_RIGHT;
    }
    if (y < ui_cap_h(p) && !p->menu_open) {
        hit = ui_hit(p, x, y);
        return hit == HT_NONE ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
    }
    return SDL_HITTEST_NORMAL;
}

/* ---------------- events ---------------- */

static void update_scale(Player *p)
{
    int i;
    float sc = SDL_GetWindowDisplayScale(p->win);
    p->density = SDL_GetWindowPixelDensity(p->win);
    if (p->density < 0.5f)
        p->density = 1.0f;
    p->dpi = (int)(96.0f * (sc > 0.5f ? sc : 1.0f) + 0.5f);
    if (p->dpi < 96)
        p->dpi = 96;
    ui_init_fonts(p);
    for (i = 0; i < 2; i++) {
        free(p->ico_px[i]);
        p->ico_px[i] = NULL;
    }
    plat_icons(p);
    set_min_size(p);
    ui_invalidate_all(p);
}

static void end_drag(Player *p)
{
    if (p->dragging == 1 && !p->scrub_was_paused)
        ff_set_paused(p, 0);
    p->dragging = 0;
}

static void on_motion(Player *p, int x, int y)
{
    if (p->menu_open) {
        int s = ui_menu_hit(p, x, y);
        if (s != p->menu_sel) {
            p->menu_sel = s;
            ui_invalidate_rect(p, &p->menu_rc);
        }
        return;
    }
    if (!p->dragging && x == p->last_mx && y == p->last_my)
        return;                         /* not a real move (e.g. relayout) */
    p->last_mx = x;
    p->last_my = y;
    p->last_input = SDL_GetTicks();
    if (p->fullscreen && !p->bars_visible) {
        p->bars_visible = 1;
        ui_invalidate(p, 1, 1);
    }
    if (p->dragging == 1 && p->dur > 0) {
        p->drag_pos = ui_seek_frac(p, x) * p->dur;
        /* live scrubbing: follow the knob, one seek at a time */
        scrub_to_knob(p);
        ui_invalidate(p, 0, 2);
    } else if (p->dragging == 2) {
        set_volume(p, ui_vol_frac(p, y));
    } else {
        int h = ui_hit(p, x, y);
        if (h != p->hover) {
            p->hover = h;
            ui_invalidate(p, 0, 1);
        }
    }
}

static void on_button_down(Player *p, const SDL_MouseButtonEvent *b)
{
    int x, y, hit;
    to_px(p, b->x, b->y, &x, &y);
    p->last_input = SDL_GetTicks();
    if (p->menu_open) {
        if (ui_menu_hit(p, x, y) < 0)
            menu_close(p);              /* click outside: just close */
        return;
    }
    if (b->button != SDL_BUTTON_LEFT)
        return;
    hit = ui_hit(p, x, y);
    switch (hit) {
    case HT_SEEK:
        if (p->playing && p->dur > 0) {
            p->dragging = 1;
            p->scrub_was_paused = p->paused;
            ff_set_paused(p, 1);            /* silent while scrubbing */
            p->drag_pos = ui_seek_frac(p, x) * p->dur;
            p->scrub_sent = -1;
            scrub_to_knob(p);
            ui_invalidate(p, 0, 2);
        }
        break;
    case HT_VOL_TRACK:
        p->dragging = 2;
        set_volume(p, ui_vol_frac(p, y));
        break;
    case HT_VOL:
        toggle_mute(p);
        break;
    case HT_VIDEO:
        if (b->clicks >= 2)
            toggle_fullscreen(p);
        break;
    default:
        if (hit != HT_NONE && hit != HT_TIME) {
            p->hover_down = hit;
            ui_invalidate(p, 0, 1);
        }
        break;
    }
}

static void on_button_up(Player *p, const SDL_MouseButtonEvent *b)
{
    int x, y, hit;
    to_px(p, b->x, b->y, &x, &y);
    p->last_input = SDL_GetTicks();
    if (p->menu_open) {
        if (b->button == SDL_BUTTON_LEFT)
            menu_activate(p, ui_menu_hit(p, x, y));
        return;
    }
    if (b->button == SDL_BUTTON_RIGHT) {
        if (ui_hit(p, x, y) == HT_VIDEO)
            show_menu(p, x, y, 1);
        return;
    }
    if (b->button != SDL_BUTTON_LEFT)
        return;
    hit = ui_hit(p, x, y);
    if (p->dragging) {
        int was = p->dragging;
        p->dragging = 0;
        if (was == 1) {
            ff_seek(p, p->drag_pos);        /* exact final position */
            if (!p->scrub_was_paused)
                ff_set_paused(p, 0);
        }
        ui_invalidate(p, 0, 1);
    } else if (hit != HT_NONE && hit != HT_TIME && p->hover_down == hit) {
        switch (hit) {
        case HT_BAR_PREV: go_next(p, -1); break;
        case HT_BAR_PLAY: toggle_pause(p); break;
        case HT_BAR_STOP: do_stop(p); break;
        case HT_BAR_NEXT: go_next(p, 1); break;
        case HT_LOOP:     toggle_loop(p); break;
        case HT_CAP_FULL: toggle_fullscreen(p); break;
        case HT_CAP_MIN:  SDL_MinimizeWindow(p->win); break;
        case HT_CAP_MAX:  toggle_maximize(p); break;
        case HT_CAP_CLOSE: p->want_quit = 1; break;
        case HT_CAP_MENU: {
            int mx, my;
            ui_menu_anchor(p, &mx, &my);
            show_menu(p, mx, my, 0);
            break;
        }
        default: break;
        }
    }
    p->hover_down = HT_NONE;
    ui_invalidate(p, 0, 1);
}

static void on_wheel(Player *p, float dy)
{
    int bar_shown = p->fullscreen ? p->bars_visible : !p->ctl_hidden;
    if (dy == 0.0f || p->menu_open)
        return;
    set_volume(p, p->volume + (dy > 0 ? 0.05 : -0.05));
    p->last_input = SDL_GetTicks();
    if (p->fullscreen && !p->bars_visible) {
        p->bars_visible = 1;                /* reveal the bar with the popup */
        bar_shown = 1;
        ui_invalidate(p, 0, 1);
    }
    if (bar_shown) {                        /* volume popup, 0.5 s after the last tick */
        p->vol_show_until = SDL_GetTicks() + 500;
        ui_invalidate(p, 0, 2);
    } else {                                /* controls hidden: small toast instead */
        ui_toast(p, "Volume %d%%", (int)(p->volume * 100.0 + 0.5));
        p->toast_until = SDL_GetTicks() + 700;
    }
}

static void on_key(Player *p, const SDL_KeyboardEvent *k)
{
    int fresh = !k->repeat;
    int ctrl = (k->mod & SDL_KMOD_CTRL) != 0;
    int plain = !(k->mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI | SDL_KMOD_SHIFT));
    SDL_Keycode key = k->key;

    p->last_input = SDL_GetTicks();
    if (p->menu_open) {
        if (key == SDLK_ESCAPE)
            menu_close(p);
        else if (key == SDLK_DOWN)
            menu_move(p, 1);
        else if (key == SDLK_UP)
            menu_move(p, -1);
        else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && fresh)
            menu_activate(p, p->menu_sel);
        return;
    }
    if (p->fullscreen && !p->bars_visible) {
        p->bars_visible = 1;
        ui_invalidate(p, 1, 1);
    }
    switch (key) {
    case SDLK_SPACE:
    case SDLK_K:
        if (fresh)
            toggle_pause(p);
        break;
    case SDLK_LEFT:  seek_rel(p, -5);  break;
    case SDLK_RIGHT: seek_rel(p, 5);   break;
    case SDLK_J:     seek_rel(p, -10); break;
    case SDLK_L:     seek_rel(p, 10);  break;
    case SDLK_UP:    set_volume(p, p->volume + 0.05); break;
    case SDLK_DOWN:  set_volume(p, p->volume - 0.05); break;
    case SDLK_M:     if (fresh) toggle_mute(p); break;
    case SDLK_F:
    case SDLK_F11:   if (fresh) toggle_fullscreen(p); break;
    case SDLK_S:     if (p->playing && !ctrl && fresh) do_save(p); break;
    case SDLK_O:     if (fresh) open_dialog(p); break;
    case SDLK_HOME:  ff_seek(p, 0); break;
    case SDLK_END:   ff_seek(p, p->dur - 0.05); break;
    case SDLK_ESCAPE:
        if (p->fullscreen)
            toggle_fullscreen(p);
        break;
    case SDLK_C:
        if (ctrl && p->playing && fresh)
            do_copy(p);
        break;
    case SDLK_COMMA:
    case SDLK_PERIOD:
        /* frame stepping; auto-repeat steps again, like holding the key */
        if (plain && p->playing)
            step_frame(p, key == SDLK_COMMA);
        break;
    default:
        if (key >= SDLK_0 && key <= SDLK_9 && p->dur > 0)
            ff_seek(p, p->dur * (double)(key - SDLK_0) / 10.0);
        break;
    }
}

static char (*g_drop)[PATHLEN];
static int g_drop_n, g_drop_cap;

static void handle_event(Player *p, SDL_Event *e)
{
    int x, y;
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        p->want_quit = 1;
        break;

    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        p->density = SDL_GetWindowPixelDensity(p->win);
        ui_invalidate_all(p);
        break;
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        ui_invalidate_all(p);
        break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        p->fullscreen = (e->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
        p->bars_visible = 1;
        p->last_input = SDL_GetTicks();
        ui_invalidate_all(p);
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        update_scale(p);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        end_drag(p);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        p->last_mx = p->last_my = -1;
        if (p->hover) {
            p->hover = HT_NONE;
            p->hover_down = HT_NONE;
            ui_invalidate(p, 0, 1);
        }
        break;

    case SDL_EVENT_MOUSE_MOTION:
        to_px(p, e->motion.x, e->motion.y, &x, &y);
        on_motion(p, x, y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        on_button_down(p, &e->button);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        on_button_up(p, &e->button);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        on_wheel(p, e->wheel.y);
        break;
    case SDL_EVENT_KEY_DOWN:
        on_key(p, &e->key);
        break;

    case SDL_EVENT_DROP_BEGIN:
        g_drop_n = 0;
        break;
    case SDL_EVENT_DROP_FILE:
        if (e->drop.data) {
            if (g_drop_n == g_drop_cap) {
                int nc = g_drop_cap ? g_drop_cap * 2 : 8;
                char (*nl)[PATHLEN] = (char (*)[PATHLEN])realloc(g_drop, (size_t)nc * PATHLEN);
                if (!nl)
                    break;
                g_drop = nl;
                g_drop_cap = nc;
            }
            SDL_strlcpy(g_drop[g_drop_n++], e->drop.data, PATHLEN);
        }
        break;
    case SDL_EVENT_DROP_COMPLETE:
        p->last_input = SDL_GetTicks();
        if (g_drop_n > 0) {                 /* play right away, keep the history */
            raise_window(p);
            open_list(p, g_drop, g_drop_n);
        }
        g_drop_n = 0;
        break;

    case SDL_EVENT_USER:
        if (e->user.code == UEV_OPEN && e->user.data1) {    /* second launch / dialog */
            raise_window(p);
            open_packed(p, (const char *)e->user.data1);
            free(e->user.data1);
        }
        break;
    default:
        break;
    }
}

/* ---------------- entry ---------------- */

/* command-line arguments as absolute paths; returns the count (list is
 * calloc'ed, caller frees) */
static int cmdline_files(int argc, char **argv, char (**out)[PATHLEN])
{
    int i, n = 0;
    char (*list)[PATHLEN] = NULL;
    *out = NULL;
    if (argc > 1)
        list = (char (*)[PATHLEN])calloc((size_t)argc, PATHLEN);
    if (!list)
        return 0;
    for (i = 1; i < argc; i++) {
        char *dst = list[n];
        if (strstr(argv[i], "://")) {               /* a URL: leave it alone */
            SDL_strlcpy(dst, argv[i], PATHLEN);
        } else {
#ifdef _WIN32
            if (!_fullpath(dst, argv[i], PATHLEN))
                SDL_strlcpy(dst, argv[i], PATHLEN);
#else
            char *rp = realpath(argv[i], NULL);
            SDL_strlcpy(dst, rp ? rp : argv[i], PATHLEN);
            free(rp);
#endif
        }
        n++;
    }
    *out = list;
    return n;
}

/* PurePlayer --write-icons DIR : the procedural icons as PNG (Linux desktop files) */
static int write_icons(const char *dir)
{
    static const char *names[2] = { "pure-video.png", "pure-music.png" };
    int i, rc = 0;
    for (i = 0; i < 2; i++) {
        uint32_t *px = (uint32_t *)calloc(256 * 256, 4);
        uint8_t *png = NULL;
        size_t n = 0;
        char path[PATHLEN];
        if (!px)
            return 1;
        ui_render_icon(i, 256, px);
        if (ff_png_from_bgra((const uint8_t *)px, 256, 256, 1024, 1, &png, &n) != 0) {
            rc = 1;
        } else {
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            if (!SDL_SaveFile(path, png, n))
                rc = 1;
        }
        free(png);
        free(px);
    }
    return rc;
}

int main(int argc, char **argv)
{
    Player *p = &g_player;
    char (*files)[PATHLEN] = NULL;
    int nfiles, w0, h0;
    SDL_Window *win;

    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", APP_TITLE, APP_VERSION);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--write-icons") == 0)
        return write_icons(argv[2]);

    nfiles = cmdline_files(argc, argv, &files);
    if (plat_single_instance(files, nfiles)) {      /* handed over: we are done */
        free(files);
        return 0;
    }

    SDL_SetHint(SDL_HINT_APP_ID, "pure-player");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        if (!SDL_Init(SDL_INIT_VIDEO))
            return 1;
    }
    {   /* PURE_DEBUG=1: name the audio driver so silent-driver builds
         * are obvious ("dummy" = no sound, ever). */
        const char *dbg = getenv("PURE_DEBUG");
        if (dbg && *dbg && strcmp(dbg, "0") != 0) {
            const char *drv = SDL_GetCurrentVideoDriver();
            const char *adrv = SDL_GetCurrentAudioDriver();
            fprintf(stderr, "[pure-init] video=%s audio=%s (audio driver "
                            "\"dummy\" means no real audio backend was "
                            "compiled in)\n",
                    drv ? drv : "?", adrv ? adrv : "none");
            fflush(stderr);
        }
    }
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();
    ui_gfx_init();

    ff_init(p);
    p->volume = 1.0;
    p->bars_visible = 1;
    p->hover = HT_NONE;
    p->hover_down = HT_NONE;
    p->shown_slot = -1;
    p->pend_slot = -1;
    p->vs = p->as = -1;
    p->last_mx = p->last_my = -1;
    p->dpi = 96;
    p->density = 1.0f;
    SDL_strlcpy(p->title, APP_TITLE, sizeof p->title);

    win = SDL_CreateWindow(APP_TITLE, 800, 484,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS |
                           SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win)
        return 1;
    p->win = win;
    SDL_SetWindowHitTest(win, hit_test, NULL);
    update_scale(p);                                /* dpi, fonts, icons */
    w0 = S_(800);
    h0 = ui_cap_h(p) + S_(450);
    SDL_SetWindowSize(win, (int)(w0 / p->density + 0.5f), (int)(h0 / p->density + 0.5f));
    plat_window_icon(p, 0);
    SDL_SyncWindow(win);
    SDL_ShowWindow(win);
    ui_invalidate_all(p);

    if (nfiles > 0)
        open_list(p, files, nfiles);
    free(files);

    {
        Uint64 last_tick = 0;
        while (!p->want_quit) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 10)) {
                do {
                    handle_event(p, &e);
                } while (SDL_PollEvent(&e));
            }
            if (SDL_GetTicks() - last_tick >= 10) {
                last_tick = SDL_GetTicks();
                on_timer(p);
            }
            ui_paint(p);
        }
    }

    ff_shutdown(p);
    ui_free_fonts(p);
    ui_gfx_free();
    free(g_drop);
    free(p->ico_px[0]);
    free(p->ico_px[1]);
    SDL_DestroyWindow(win);
    plat_ipc_stop();
    SDL_Quit();
    return 0;
}
