/* ui_draw.c - Pure-style chrome, software rendered.
 *
 * The scaled video frame lives in a cache bitmap that is only re-rendered when
 * the frame or the window size changes. Every paint copies the cache into a
 * back buffer and draws the header, the overlay control bar, popups, the menu
 * and the toast on top with an anti-aliased signed-distance rasterizer, then
 * copies the dirty rectangle to the window surface. The control bar is an
 * overlay: it never changes the size of the video area.
 */
#include "player.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int S_(int v)
{
    return (int)(((int64_t)v * g_player.dpi + (v >= 0 ? 48 : -48)) / 96);
}
static float SF(double v) { return (float)(v * g_player.dpi / 96.0); }

#define PIN(r, x, y) ((x) >= (r).left && (x) < (r).right && \
                      (y) >= (r).top  && (y) < (r).bottom)

static void rc_set(Rc *r, int l, int t, int rr, int b)
{
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
}
static int rc_isect(Rc *o, const Rc *a, const Rc *b)
{
    o->left = a->left > b->left ? a->left : b->left;
    o->top = a->top > b->top ? a->top : b->top;
    o->right = a->right < b->right ? a->right : b->right;
    o->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    return o->left < o->right && o->top < o->bottom;
}
static void rc_union(Rc *o, const Rc *b)
{
    if (b->left < o->left) o->left = b->left;
    if (b->top < o->top) o->top = b->top;
    if (b->right > o->right) o->right = b->right;
    if (b->bottom > o->bottom) o->bottom = b->bottom;
}

typedef struct { float x, y; } PTF;

#define ARGB(a, r, g, b) ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
                                     ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define A_TEXT    ARGB(255, 238, 238, 236)
#define A_DIM     ARGB(255, 170, 170, 168)
#define A_ACCENT  ARGB(255, 53, 132, 228)
#define A_HOVER   ARGB(40, 255, 255, 255)
#define A_OFF     ARGB(80, 238, 238, 236)       /* disabled button */

/* ---------------- software rasterizer ---------------- */

static uint32_t *FB;                    /* target pixels (BGRA / XRGB) */
static int FW, FH;                      /* target size */
static Rc CLIP;                         /* drawing is limited to this */

static float C01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void put(int x, int y, uint32_t c, float cov)
{
    int a = (int)(((c >> 24) & 255) * cov + 0.5f);
    if (a <= 0 || x < CLIP.left || y < CLIP.top || x >= CLIP.right || y >= CLIP.bottom)
        return;
    uint32_t *d = FB + (size_t)y * FW + x, s = *d;
    uint32_t ia = 255 - (uint32_t)a, da = s >> 24;
    if (da == 255) {
        uint32_t r = (((c >> 16) & 255) * a + ((s >> 16) & 255) * ia) / 255;
        uint32_t g = (((c >> 8) & 255) * a + ((s >> 8) & 255) * ia) / 255;
        uint32_t b = ((c & 255) * a + (s & 255) * ia) / 255;
        *d = 0xFF000000u | r << 16 | g << 8 | b;
    } else {                            /* general "over" (icons on transparent) */
        uint32_t oa = a + da * ia / 255;
        if (!oa)
            return;
        uint32_t r = (((c >> 16) & 255) * a * 255 + ((s >> 16) & 255) * da * ia) / (oa * 255);
        uint32_t g = (((c >> 8) & 255) * a * 255 + ((s >> 8) & 255) * da * ia) / (oa * 255);
        uint32_t b = ((c & 255) * a * 255 + (s & 255) * da * ia) / (oa * 255);
        *d = oa << 24 | r << 16 | g << 8 | b;
    }
}

typedef float (*SDF)(float, float, const void *);
static void shape(float x0, float y0, float x1, float y1, SDF f, const void *u, uint32_t c)
{
    int xa = (int)floorf(x0) - 1, xb = (int)ceilf(x1) + 1;
    int ya = (int)floorf(y0) - 1, yb = (int)ceilf(y1) + 1, x, y;
    if (xa < CLIP.left) xa = CLIP.left;
    if (ya < CLIP.top) ya = CLIP.top;
    if (xb >= CLIP.right) xb = CLIP.right - 1;
    if (yb >= CLIP.bottom) yb = CLIP.bottom - 1;
    for (y = ya; y <= yb; y++)
        for (x = xa; x <= xb; x++)
            put(x, y, c, C01(0.5f - f(x + 0.5f, y + 0.5f, u)));
}

typedef struct { float cx, cy, hw, hh, r; } RR;
static float sd_rr(float x, float y, const void *u)
{
    const RR *s = (const RR *)u;
    float qx = fabsf(x - s->cx) - (s->hw - s->r), qy = fabsf(y - s->cy) - (s->hh - s->r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - s->r;
}

typedef struct { float x1, y1, x2, y2, hw; } LN;
static float sd_ln(float x, float y, const void *u)
{
    const LN *s = (const LN *)u;
    float px = x - s->x1, py = y - s->y1, dx = s->x2 - s->x1, dy = s->y2 - s->y1;
    float t = C01((px * dx + py * dy) / (dx * dx + dy * dy + 1e-9f));
    float ex = px - dx * t, ey = py - dy * t;
    return sqrtf(ex * ex + ey * ey) - s->hw;
}

typedef struct { const PTF *p; int n; float grow; } PG;     /* concave-safe */
static float sd_pg(float x, float y, const void *u)
{
    const PG *g = (const PG *)u;
    float d = 1e9f;
    int in = 0, i, j;
    for (i = 0, j = g->n - 1; i < g->n; j = i++) {
        float ax = g->p[j].x, ay = g->p[j].y, bx = g->p[i].x, by = g->p[i].y;
        float ex = bx - ax, ey = by - ay, wx = x - ax, wy = y - ay;
        float t = C01((wx * ex + wy * ey) / (ex * ex + ey * ey + 1e-9f));
        float qx = wx - ex * t, qy = wy - ey * t, q = qx * qx + qy * qy;
        if (q < d)
            d = q;
        if ((ay > y) != (by > y) && x < ax + (y - ay) * ex / ey)
            in = !in;
    }
    d = sqrtf(d);
    return (in ? -d : d) - g->grow;
}

typedef struct { float cx, cy, r, a0, sw, hw; } AR;         /* sw > 0, rx == ry */
static float sd_ar(float x, float y, const void *u)
{
    const AR *s = (const AR *)u;
    float dx = x - s->cx, dy = y - s->cy;
    float a = atan2f(dy, dx) * 57.29578f - s->a0;           /* y-down = GDI+ angles */
    a -= 360.0f * floorf(a / 360.0f);
    if (a <= s->sw)
        return fabsf(sqrtf(dx * dx + dy * dy) - s->r) - s->hw;
    {
        float r0 = s->a0 * 0.0174533f, r1 = (s->a0 + s->sw) * 0.0174533f;
        float ax = s->cx + s->r * cosf(r0) - x, ay = s->cy + s->r * sinf(r0) - y;
        float bx = s->cx + s->r * cosf(r1) - x, by = s->cy + s->r * sinf(r1) - y;
        return sqrtf(fminf(ax * ax + ay * ay, bx * bx + by * by)) - s->hw;
    }
}

static void grrect(float x, float y, float w, float h, float r, uint32_t c)
{
    RR s;
    s.cx = x + w / 2; s.cy = y + h / 2; s.hw = w / 2; s.hh = h / 2;
    s.r = fminf(r, fminf(w, h) / 2);
    shape(x, y, x + w, y + h, sd_rr, &s, c);
}
static void grrect_r(const Rc *r, float rad, uint32_t c)
{
    grrect((float)r->left, (float)r->top, (float)(r->right - r->left),
           (float)(r->bottom - r->top), rad, c);
}
static void gcircle(float cx, float cy, float r, uint32_t c)
{
    grrect(cx - r, cy - r, 2 * r, 2 * r, r, c);
}
static void gline(float x1, float y1, float x2, float y2, float w, uint32_t c)
{
    LN s;
    s.x1 = x1; s.y1 = y1; s.x2 = x2; s.y2 = y2; s.hw = w * 0.5f;
    shape(fminf(x1, x2) - w, fminf(y1, y2) - w, fmaxf(x1, x2) + w, fmaxf(y1, y2) + w,
          sd_ln, &s, c);
}
static void garc(float cx, float cy, float rx, float ry, float a0, float sw,
                 float w, uint32_t c)
{
    AR s;
    (void)ry;
    s.cx = cx; s.cy = cy; s.r = rx; s.a0 = a0; s.sw = sw; s.hw = w * 0.5f;
    shape(cx - rx - w, cy - rx - w, cx + rx + w, cy + rx + w, sd_ar, &s, c);
}
static void gpoly(const PTF *p, int n, float grow, uint32_t c)
{
    PG g;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int i;
    g.p = p; g.n = n; g.grow = grow;
    for (i = 0; i < n; i++) {
        x0 = fminf(x0, p[i].x); y0 = fminf(y0, p[i].y);
        x1 = fmaxf(x1, p[i].x); y1 = fmaxf(y1, p[i].y);
    }
    shape(x0 - grow, y0 - grow, x1 + grow, y1 + grow, sd_pg, &g, c);
}
static void gfill_poly(const PTF *p, int n, uint32_t c) { gpoly(p, n, 0, c); }

static void gtri(float x1, float y1, float x2, float y2, float x3, float y3,
                 float soften, uint32_t c)
{
    PTF p[3];
    p[0].x = x1; p[0].y = y1;
    p[1].x = x2; p[1].y = y2;
    p[2].x = x3; p[2].y = y3;
    gpoly(p, 3, soften * 0.5f, c);          /* grown polygon = rounded corners */
}

/* ---------------- text (stb_truetype, font loaded at run time) ---------------- */

enum { TF_LEFT = 0, TF_CENTER = 1, TF_RIGHT = 2, TF_VCENTER = 4, TF_ELLIPSIS = 8 };

typedef struct { int cp, w, h, xo, yo; float adv; unsigned char *bm; } Gly;
typedef struct { float scale; int asc, desc; Gly *g; int n, cap; } Font;

static stbtt_fontinfo g_fi;
static unsigned char *g_fdata;
static int g_fok;

static void font_load(void)
{
    char path[PATHLEN];
    size_t sz = 0;
    if (g_fok)
        return;
    if (!plat_find_font(path, sizeof path))
        return;
    g_fdata = (unsigned char *)SDL_LoadFile(path, &sz);
    if (!g_fdata)
        return;
    if (!stbtt_InitFont(&g_fi, g_fdata, stbtt_GetFontOffsetForIndex(g_fdata, 0))) {
        SDL_free(g_fdata);
        g_fdata = NULL;
        return;
    }
    g_fok = 1;
}

static Font *font_make(int px)
{
    Font *f;
    int a, d, lg;
    if (!g_fok)
        return NULL;
    f = (Font *)calloc(1, sizeof(Font));
    if (!f)
        return NULL;
    f->scale = stbtt_ScaleForMappingEmToPixels(&g_fi, (float)px);
    stbtt_GetFontVMetrics(&g_fi, &a, &d, &lg);
    f->asc = (int)floorf(a * f->scale + 0.5f);
    f->desc = (int)floorf(d * f->scale - 0.5f);
    return f;
}

static void font_free(Font *f)
{
    int i;
    if (!f)
        return;
    for (i = 0; i < f->n; i++)
        if (f->g[i].bm)
            stbtt_FreeBitmap(f->g[i].bm, NULL);
    free(f->g);
    free(f);
}

static Gly *glyph(Font *f, int cp)
{
    int i, gi, adv, lsb;
    Gly *g;
    for (i = 0; i < f->n; i++)
        if (f->g[i].cp == cp)
            return &f->g[i];
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 128;
        Gly *ng = (Gly *)realloc(f->g, (size_t)nc * sizeof(Gly));
        if (!ng)
            return NULL;
        f->g = ng;
        f->cap = nc;
    }
    g = &f->g[f->n++];
    memset(g, 0, sizeof(*g));
    g->cp = cp;
    gi = stbtt_FindGlyphIndex(&g_fi, cp);
    stbtt_GetGlyphHMetrics(&g_fi, gi, &adv, &lsb);
    g->adv = adv * f->scale;
    g->bm = stbtt_GetGlyphBitmap(&g_fi, f->scale, f->scale, gi, &g->w, &g->h, &g->xo, &g->yo);
    return g;
}

static int u8next(const char **sp)
{
    const unsigned char *s = (const unsigned char *)*sp;
    int c = *s++, n = 0;
    if (c < 0x80) {
        n = 0;
    } else if ((c & 0xE0) == 0xC0) {
        c &= 0x1F; n = 1;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F; n = 2;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07; n = 3;
    } else {
        *sp += 1;
        return 0xFFFD;
    }
    while (n-- > 0) {
        if ((*s & 0xC0) != 0x80) {
            *sp = (const char *)s;
            return 0xFFFD;
        }
        c = (c << 6) | (*s++ & 0x3F);
    }
    *sp = (const char *)s;
    return c;
}

static int text_w(Font *f, const char *s)
{
    float w = 0;
    if (!f)
        return 0;
    while (*s) {
        int cp = u8next(&s);
        Gly *g = glyph(f, cp);
        if (g)
            w += g->adv;
    }
    return (int)ceilf(w);
}

static void text_draw(Font *f, const char *s, Rc r, int fl, uint32_t col)
{
    char buf[560];
    int rw = r.right - r.left, rh = r.bottom - r.top, w, x, base;
    float fx;
    if (!f || !s || rw <= 0)
        return;
    w = text_w(f, s);
    if ((fl & TF_ELLIPSIS) && w > rw) {         /* shorten, append "..." */
        const char *q = s;
        int dots = text_w(f, "..."), acc = 0, len = 0;
        while (*q) {
            const char *b = q;
            int cp = u8next(&q);
            Gly *g = glyph(f, cp);
            int cw = g ? (int)ceilf(g->adv) : 0;
            if (acc + cw + dots > rw || len + (int)(q - b) > 540)
                break;
            acc += cw;
            len += (int)(q - b);
        }
        memcpy(buf, s, (size_t)len);
        strcpy(buf + len, "...");
        s = buf;
        w = acc + dots;
    }
    x = r.left;
    if (fl & TF_RIGHT)
        x = r.right - w;
    else if (fl & TF_CENTER)
        x = r.left + (rw - w) / 2;
    base = r.top + f->asc;
    if (fl & TF_VCENTER)
        base = r.top + (rh - (f->asc - f->desc)) / 2 + f->asc;
    fx = (float)x;
    while (*s) {
        int cp = u8next(&s), i, j;
        Gly *g = glyph(f, cp);
        if (!g)
            break;
        if (g->bm) {
            int gx = (int)floorf(fx + 0.5f) + g->xo, gy = base + g->yo;
            for (j = 0; j < g->h; j++) {
                int yy = gy + j;
                if (yy < r.top || yy >= r.bottom)
                    continue;
                for (i = 0; i < g->w; i++) {
                    int xx = gx + i;
                    if (xx < r.left || xx >= r.right)
                        continue;
                    put(xx, yy, col, g->bm[j * g->w + i] / 255.0f);
                }
            }
        }
        fx += g->adv;
    }
}

/* ---------------- buffers ---------------- */

static uint32_t *g_cbits, *g_bbits;     /* video cache, back buffer */
static SDL_Surface *g_bsurf;            /* g_bbits wrapped for blitting */
static int g_bw, g_bh;
static int g_cdirty = 1;
static struct SwsContext *g_usws;       /* frame -> display size scaler */
static Rc g_dirty;
static int g_have_dirty;

void ui_gfx_init(void)
{
}

/* returns 1 when the buffers were (re)allocated */
static int ensure_buffers(int w, int h)
{
    if (g_cbits && g_bbits && w == g_bw && h == g_bh)
        return 0;
    SDL_DestroySurface(g_bsurf);
    g_bsurf = NULL;
    free(g_cbits);
    free(g_bbits);
    g_cbits = (uint32_t *)calloc((size_t)w * h, 4);
    g_bbits = (uint32_t *)calloc((size_t)w * h, 4);
    g_bw = w;
    g_bh = h;
    g_cdirty = 1;
    if (g_bbits) {
        g_bsurf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, g_bbits, w * 4);
        if (g_bsurf)
            SDL_SetSurfaceBlendMode(g_bsurf, SDL_BLENDMODE_NONE);
    }
    return 1;
}

void ui_gfx_free(void)
{
    SDL_DestroySurface(g_bsurf);
    g_bsurf = NULL;
    free(g_cbits);
    free(g_bbits);
    g_cbits = g_bbits = NULL;
    sws_freeContext(g_usws);
    g_usws = NULL;
    if (g_fdata) {
        SDL_free(g_fdata);
        g_fdata = NULL;
        g_fok = 0;
    }
}

/* ---------------- layout ---------------- */

typedef struct {
    Rc cap, vid, bar, ov;
    int  has_cap, has_bar;
    Rc c_full, c_menu, c_min, c_max, c_close;
    Rc b_prev, b_play, b_stop, b_next;
    Rc seek, seek_hit, time, b_loop, b_vol;
    Rc pop, pop_hit, pop_track;
} BL;

static int g_tw_short, g_tw_long;       /* time label widths, measured once */

int ui_cap_h(const Player *p)
{
    (void)p;
    return S_(L_CAP_H);
}

static void win_size(const Player *p, int *w, int *h)
{
    *w = *h = 0;
    if (p->win)
        SDL_GetWindowSizeInPixels(p->win, w, h);
}

static void bar_calc(const Player *p, BL *L)
{
    int W, H;
    win_size(p, &W, &H);
    int fs = p->fullscreen;
    int cap_h = fs ? 0 : ui_cap_h(p);
    int bar_h = S_(L_BAR_H);
    int show = fs ? p->bars_visible : !p->ctl_hidden;

    memset(L, 0, sizeof(*L));
    L->has_cap = !fs;
    L->has_bar = show;
    rc_set(&L->cap, 0, 0, W, cap_h);
    rc_set(&L->vid, 0, cap_h, W, H);
    if (show) {
        int ot = H - bar_h - S_(36);
        if (ot < cap_h)
            ot = cap_h;
        rc_set(&L->bar, 0, H - bar_h, W, H);
        rc_set(&L->ov, 0, ot, W, H);
    } else {
        rc_set(&L->bar, 0, H, W, H);
        rc_set(&L->ov, 0, H, W, H);
    }

    if (!fs) {                                  /* header buttons, right to left */
        int bw = S_(46), x = W;
        rc_set(&L->c_close, x - bw, 0, x, cap_h);   x -= bw;
        rc_set(&L->c_max, x - bw, 0, x, cap_h);     x -= bw;
        rc_set(&L->c_min, x - bw, 0, x, cap_h);     x -= bw;
        rc_set(&L->c_menu, x - bw, 0, x, cap_h);    x -= bw;
        rc_set(&L->c_full, x - bw, 0, x, cap_h);
    }

    if (show) {                                 /* control bar */
        int bs = S_(30), bhh = S_(28);
        int by = L->bar.top + (bar_h - bhh) / 2;
        int cy = L->bar.top + bar_h / 2;
        int gap = S_(2), x = S_(8), last;

#define BTN(r) do { rc_set(&L->r, x, by, x + bs, by + bhh); x += bs + gap; } while (0)
        BTN(b_prev);
        BTN(b_play);
        BTN(b_stop);
        BTN(b_next);
#undef BTN
        last = x - gap;

        int xr = W - S_(8);
        rc_set(&L->b_vol, xr - bs, by, xr, by + bhh);
        xr -= bs + gap;
        rc_set(&L->b_loop, xr - bs, by, xr, by + bhh);
        xr -= bs + S_(8);
        int tw = (p->dur >= 3600) ? g_tw_long : g_tw_short;
        if (tw <= 0)
            tw = S_(84);
        rc_set(&L->time, xr - tw, L->bar.top, xr, L->bar.bottom);
        xr -= tw + S_(12);

        int sx1 = last + S_(12), sx2 = xr;
        if (sx2 < sx1 + S_(24))
            sx2 = sx1 + S_(24);
        rc_set(&L->seek, sx1, cy - S_(2), sx2, cy - S_(2) + S_(3));
        rc_set(&L->seek_hit, sx1 - S_(6), cy - S_(14), sx2 + S_(6), cy + S_(14));

        /* vertical volume popup above the speaker */
        int pcx = (L->b_vol.left + L->b_vol.right) / 2;
        int pw = S_(30), ph = S_(112);
        int pb = L->b_vol.top - S_(6);
        if (pcx - pw / 2 < S_(4))
            pcx = S_(4) + pw / 2;
        rc_set(&L->pop, pcx - pw / 2, pb - ph, pcx + pw / 2, pb);
        rc_set(&L->pop_hit, L->pop.left, L->pop.top, L->pop.right, L->b_vol.top);
        rc_set(&L->pop_track, pcx - S_(2), L->pop.top + S_(12),
               pcx + S_(2), L->pop.bottom - S_(12));
    }
}

static int vol_visible(const Player *p)
{
    return p->hover == HT_VOL || p->hover == HT_VOL_TRACK || p->dragging == 2 ||
           (p->vol_show_until && p->vol_show_until > SDL_GetTicks());
}

void ui_layout(const Player *p, Rc *cap, Rc *video, Rc *bar)
{
    BL L;
    bar_calc(p, &L);
    *cap = L.cap;
    *video = L.vid;
    *bar = L.bar;
}

/* ---------------- invalidation ---------------- */

void ui_invalidate_rect(Player *p, const Rc *r)
{
    Rc c;
    int W, H;
    win_size(p, &W, &H);
    rc_set(&c, 0, 0, W, H);
    Rc x;
    if (!p->win || !rc_isect(&x, r, &c))
        return;
    if (g_have_dirty)
        rc_union(&g_dirty, &x);
    else
        g_dirty = x;
    g_have_dirty = 1;
}

void ui_invalidate_all(Player *p)
{
    int W, H;
    Rc r;
    win_size(p, &W, &H);
    rc_set(&r, 0, 0, W, H);
    g_cdirty = 1;
    g_have_dirty = 0;
    ui_invalidate_rect(p, &r);
}

void ui_invalidate(Player *p, int video, int chrome)
{
    if (!p->win)
        return;
    BL L;
    bar_calc(p, &L);
    if (video) {                    /* 1 = new frame, 2 = repaint only */
        if (video == 1)
            g_cdirty = 1;
        ui_invalidate_rect(p, &L.vid);
    }
    if (chrome) {                   /* 1 = header + bar, 2 = bar only */
        if (L.has_cap && chrome != 2)
            ui_invalidate_rect(p, &L.cap);
        if (L.has_bar) {
            ui_invalidate_rect(p, &L.ov);
            ui_invalidate_rect(p, &L.pop);
        }
    }
}

int ui_hit(Player *p, int x, int y)
{
    BL L;
    bar_calc(p, &L);
    if (L.has_cap && y < L.cap.bottom) {
        if (PIN(L.c_close, x, y)) return HT_CAP_CLOSE;
        if (PIN(L.c_max, x, y))   return HT_CAP_MAX;
        if (PIN(L.c_min, x, y))   return HT_CAP_MIN;
        if (PIN(L.c_menu, x, y))  return HT_CAP_MENU;
        if (PIN(L.c_full, x, y))  return HT_CAP_FULL;
        return HT_NONE;
    }
    if (L.has_bar) {
        if (vol_visible(p) && PIN(L.pop_hit, x, y))
            return HT_VOL_TRACK;
        if (PIN(L.bar, x, y)) {
            if (PIN(L.b_prev, x, y)) return HT_BAR_PREV;
            if (PIN(L.b_play, x, y)) return HT_BAR_PLAY;
            if (PIN(L.b_stop, x, y)) return HT_BAR_STOP;
            if (PIN(L.b_next, x, y)) return HT_BAR_NEXT;
            if (PIN(L.b_vol, x, y))  return HT_VOL;
            if (PIN(L.b_loop, x, y)) return HT_LOOP;
            if (PIN(L.seek_hit, x, y)) return HT_SEEK;
            if (PIN(L.time, x, y))   return HT_TIME;
            return HT_NONE;
        }
    }
    return HT_VIDEO;
}

double ui_seek_frac(const Player *p, int x)
{
    BL L;
    bar_calc(p, &L);
    int w = L.seek.right - L.seek.left;
    if (w <= 0)
        return 0;
    double f = (double)(x - L.seek.left) / (double)w;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

double ui_vol_frac(const Player *p, int y)
{
    BL L;
    bar_calc(p, &L);
    int h = L.pop_track.bottom - L.pop_track.top;
    if (h <= 0)
        return 0;
    double f = (double)(L.pop_track.bottom - y) / (double)h;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

void ui_menu_anchor(const Player *p, int *x, int *y)
{
    BL L;
    bar_calc(p, &L);
    *x = L.c_menu.right;
    *y = L.cap.bottom;
}

/* ---------------- fonts / time ---------------- */

void ui_init_fonts(Player *p)
{
    ui_free_fonts(p);
    font_load();
    p->fnt_cap  = font_make(S_(12));
    p->fnt_bar  = font_make(S_(12));
    p->fnt_hint = font_make(S_(15));
    g_tw_short = text_w((Font *)p->fnt_bar, "88:88/88:88") + S_(6);
    g_tw_long = text_w((Font *)p->fnt_bar, "88:88:88/88:88:88") + S_(6);
}

void ui_free_fonts(Player *p)
{
    font_free((Font *)p->fnt_cap);
    font_free((Font *)p->fnt_bar);
    font_free((Font *)p->fnt_hint);
    p->fnt_cap = p->fnt_bar = p->fnt_hint = NULL;
}

void ui_fmt_time(char *o, int cap, double s)
{
    if (s < 0)
        s = 0;
    int t = (int)(s + 0.5);
    int h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    if (h)
        snprintf(o, (size_t)cap, "%02d:%02d:%02d", h, m, sec);
    else
        snprintf(o, (size_t)cap, "%02d:%02d", m, sec);
}

/* ---------------- glyphs (cx, cy = centre, s = icon size in px) ---------------- */

static void glyph_play(float cx, float cy, float s, uint32_t c)
{
    gtri(cx - s * 0.34f, cy - s * 0.46f, cx - s * 0.34f, cy + s * 0.46f,
         cx + s * 0.46f, cy, s * 0.12f, c);
}

static void glyph_pause(float cx, float cy, float s, uint32_t c)
{
    float w = s * 0.26f, h = s * 0.92f;
    grrect(cx - s * 0.40f, cy - h / 2, w, h, s * 0.06f, c);
    grrect(cx + s * 0.14f, cy - h / 2, w, h, s * 0.06f, c);
}

static void glyph_stop(float cx, float cy, float s, uint32_t c)
{
    float w = s * 0.74f;
    grrect(cx - w / 2, cy - w / 2, w, w, s * 0.10f, c);
}

static void glyph_prev(float cx, float cy, float s, uint32_t c)
{
    grrect(cx - s * 0.52f, cy - s * 0.46f, s * 0.15f, s * 0.92f, s * 0.05f, c);
    gtri(cx + s * 0.50f, cy - s * 0.46f, cx + s * 0.50f, cy + s * 0.46f,
         cx - s * 0.28f, cy, s * 0.10f, c);
}

static void glyph_next(float cx, float cy, float s, uint32_t c)
{
    grrect(cx + s * 0.37f, cy - s * 0.46f, s * 0.15f, s * 0.92f, s * 0.05f, c);
    gtri(cx - s * 0.50f, cy - s * 0.46f, cx - s * 0.50f, cy + s * 0.46f,
         cx + s * 0.28f, cy, s * 0.10f, c);
}

static void glyph_loop(float cx, float cy, float s, uint32_t c)
{
    float pw = fmaxf(1.5f, s * 0.11f);
    float y0 = cy - s * 0.30f, y1 = cy + s * 0.30f, r = (y1 - y0) / 2;
    float half = s * 0.44f, xr = cx + half, xl = cx - half;
    gline(xl, y0, xr - s * 0.22f, y0, pw, c);
    gline(xr, y1, xl + s * 0.22f, y1, pw, c);
    garc(xr, cy, r, r, -90, 180, pw, c);
    garc(xl, cy, r, r, 90, 180, pw, c);
    gtri(xr - s * 0.26f, y0 - s * 0.17f, xr - s * 0.26f, y0 + s * 0.17f,
         xr + s * 0.06f, y0, 0, c);
    gtri(xl + s * 0.26f, y1 - s * 0.17f, xl + s * 0.26f, y1 + s * 0.17f,
         xl - s * 0.06f, y1, 0, c);
}

static void glyph_speaker(float cx, float cy, float s, uint32_t c, double vol, int muted)
{
    PTF p[6];
    float pw = fmaxf(1.4f, s * 0.10f);
    p[0].x = cx - s * 0.56f; p[0].y = cy - s * 0.20f;
    p[1].x = cx - s * 0.30f; p[1].y = cy - s * 0.20f;
    p[2].x = cx + s * 0.02f; p[2].y = cy - s * 0.48f;
    p[3].x = cx + s * 0.02f; p[3].y = cy + s * 0.48f;
    p[4].x = cx - s * 0.30f; p[4].y = cy + s * 0.20f;
    p[5].x = cx - s * 0.56f; p[5].y = cy + s * 0.20f;
    gfill_poly(p, 6, c);
    if (muted) {
        gline(cx + s * 0.18f, cy - s * 0.24f, cx + s * 0.52f, cy + s * 0.24f, pw, c);
        gline(cx + s * 0.18f, cy + s * 0.24f, cx + s * 0.52f, cy - s * 0.24f, pw, c);
    } else {
        if (vol > 0.0)
            garc(cx + s * 0.04f, cy, s * 0.28f, s * 0.28f, -48, 96, pw, c);
        if (vol > 0.45)
            garc(cx + s * 0.04f, cy, s * 0.52f, s * 0.52f, -48, 96, pw, c);
    }
}

static void glyph_fs(float cx, float cy, float s, uint32_t c, int exit)
{
    float pw = fmaxf(1.0f, SF(1.0));
    float m = exit ? s * 0.28f : s * 0.46f, ln = s * 0.30f;
    gline(cx - m, cy - m + ln, cx - m, cy - m, pw, c);
    gline(cx - m, cy - m, cx - m + ln, cy - m, pw, c);
    gline(cx + m - ln, cy - m, cx + m, cy - m, pw, c);
    gline(cx + m, cy - m, cx + m, cy - m + ln, pw, c);
    gline(cx + m, cy + m - ln, cx + m, cy + m, pw, c);
    gline(cx + m, cy + m, cx + m - ln, cy + m, pw, c);
    gline(cx - m + ln, cy + m, cx - m, cy + m, pw, c);
    gline(cx - m, cy + m, cx - m, cy + m - ln, pw, c);
}

static void glyph_menu(float cx, float cy, float s, uint32_t c)
{
    float pw = fmaxf(1.0f, SF(1.0)), hw = s * 0.46f;
    gline(cx - hw, cy - s * 0.30f, cx + hw, cy - s * 0.30f, pw, c);
    gline(cx - hw, cy, cx + hw, cy, pw, c);
    gline(cx - hw, cy + s * 0.30f, cx + hw, cy + s * 0.30f, pw, c);
}

/* square outline made of four crisp 1 px bars */
static void sq_outline(float x, float y, float w, float h, float t, uint32_t c)
{
    grrect(x, y, w, t, 0, c);
    grrect(x, y + h - t, w, t, 0, c);
    grrect(x, y, t, h, 0, c);
    grrect(x + w - t, y, t, h, 0, c);
}

static void glyph_min(float cx, float cy, float s, uint32_t c)
{
    float t = fmaxf(1.0f, floorf(SF(1.0)));
    grrect(floorf(cx - s * 0.5f), floorf(cy), floorf(s), t, 0, c);
}

static void glyph_max(float cx, float cy, float s, uint32_t c, int restore)
{
    float t = fmaxf(1.0f, floorf(SF(1.0))), w = floorf(s);
    float x = floorf(cx - w / 2), y = floorf(cy - w / 2);
    if (!restore) {
        sq_outline(x, y, w, w, t, c);
    } else {
        float o = floorf(w * 0.22f), q = w - o;
        grrect(x + o, y, q, t, 0, c);                   /* back square: top, right */
        grrect(x + w - t, y, t, q, 0, c);
        sq_outline(x, y + o, q, q, t, c);               /* front square */
    }
}

static void glyph_close(float cx, float cy, float s, uint32_t c)
{
    float pw = fmaxf(1.0f, SF(1.0)), h = s * 0.5f;
    gline(cx - h, cy - h, cx + h, cy + h, pw, c);
    gline(cx - h, cy + h, cx + h, cy - h, pw, c);
}

/* ---------------- procedural app icon ---------------- */

void ui_render_icon(int music, int size, uint32_t *px)
{
    uint32_t *sfb = FB;
    int sfw = FW, sfh = FH;
    Rc sclip = CLIP;
    float s = (float)size, hr = s * 0.095f, pw = s * 0.055f;
    uint32_t wh = ARGB(255, 255, 255, 255);

    memset(px, 0, (size_t)size * size * 4);
    FB = px; FW = FH = size;
    rc_set(&CLIP, 0, 0, size, size);
    grrect(0, 0, s, s, s * 0.22f, music ? ARGB(255, 38, 162, 105) : A_ACCENT);
    if (!music) {
        glyph_play(s * 0.53f, s * 0.5f, s * 0.52f, wh);
    } else {
        float x1 = s * 0.36f, x2 = s * 0.66f, y1 = s * 0.70f, y2 = s * 0.64f;
        float t1 = s * 0.28f, t2 = s * 0.22f;
        PTF beam[4];
        gcircle(x1, y1, hr, wh);
        gcircle(x2, y2, hr, wh);
        gline(x1 + hr * 0.85f, y1, x1 + hr * 0.85f, t1, pw, wh);
        gline(x2 + hr * 0.85f, y2, x2 + hr * 0.85f, t2, pw, wh);
        beam[0].x = x1 + hr * 0.85f; beam[0].y = t1;
        beam[1].x = x2 + hr * 0.85f; beam[1].y = t2;
        beam[2].x = x2 + hr * 0.85f; beam[2].y = t2 + s * 0.09f;
        beam[3].x = x1 + hr * 0.85f; beam[3].y = t1 + s * 0.09f;
        gfill_poly(beam, 4, wh);
    }
    FB = sfb; FW = sfw; FH = sfh; CLIP = sclip;
}

/* ---------------- video cache ---------------- */

static Rc fit_rect(double dar, const Rc *a)
{
    Rc o;
    rc_set(&o, 0, 0, 0, 0);
    int dw = a->right - a->left, dh = a->bottom - a->top;
    if (dar <= 0 || dw <= 0 || dh <= 0)
        return o;
    int w = dw, h = (int)(dw / dar + 0.5);
    if (h > dh) {
        h = dh;
        w = (int)(dh * dar + 0.5);
    }
    if (abs(dw - w) <= 2) w = dw;       /* no 1-2 px black slivers */
    if (abs(dh - h) <= 2) h = dh;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    o.left = a->left + (dw - w) / 2;
    o.top = a->top + (dh - h) / 2;
    o.right = o.left + w;
    o.bottom = o.top + h;
    return o;
}

static void fill_cache(const Rc *r, uint32_t c)
{
    int x, y;
    Rc a, all;
    rc_set(&all, 0, 0, g_bw, g_bh);
    if (!rc_isect(&a, r, &all))
        return;
    for (y = a.top; y < a.bottom; y++) {
        uint32_t *d = g_cbits + (size_t)y * g_bw;
        for (x = a.left; x < a.right; x++)
            d[x] = c;
    }
}

static void render_cache(Player *p, const BL *L)
{
    Rc all;
    rc_set(&all, 0, 0, g_bw, g_bh);

    AVFrame *fr = NULL;
    double dar = 0;
    if (p->shown_slot >= 0 && p->slot[p->shown_slot].bgra) {
        fr = p->slot[p->shown_slot].bgra;
        dar = (p->vdar > 0) ? p->vdar : (double)fr->width / fr->height;
    } else if (p->playing && p->vs < 0 && p->cover && p->cover->height > 0) {
        fr = p->cover;                      /* audio file: show its cover art */
        dar = (double)fr->width / fr->height;
    }
    if (fr) {
        Rc f = fit_rect(dar, &L->vid);
        int dw = f.right - f.left, dh = f.bottom - f.top;
        if (dw > 0 && dh > 0) {
            /* black bars only where the picture doesn't reach */
            Rc t = all, b = all, l = all, r = all;
            t.bottom = f.top;
            b.top = f.bottom;
            l.top = f.top; l.bottom = f.bottom; l.right = f.left;
            r.top = f.top; r.bottom = f.bottom; r.left = f.right;
            fill_cache(&t, COL_VIDEO); fill_cache(&b, COL_VIDEO);
            fill_cache(&l, COL_VIDEO); fill_cache(&r, COL_VIDEO);

            /* scale straight into the cache bitmap with swscale (SIMD) */
            g_usws = sws_getCachedContext(g_usws, fr->width, fr->height,
                         AV_PIX_FMT_BGRA, dw, dh, AV_PIX_FMT_BGRA,
                         (dw < fr->width) ? SWS_AREA : SWS_BILINEAR,
                         NULL, NULL, NULL);
            if (g_usws && f.left >= 0 && f.top >= 0 &&
                f.right <= g_bw && f.bottom <= g_bh) {
                uint8_t *dst[4] = { (uint8_t *)g_cbits +
                                    (size_t)f.top * g_bw * 4 + (size_t)f.left * 4,
                                    NULL, NULL, NULL };
                int dls[4] = { g_bw * 4, 0, 0, 0 };
                sws_scale(g_usws, (const uint8_t *const *)fr->data, fr->linesize,
                          0, fr->height, dst, dls);
                return;
            }
        }
    }
    fill_cache(&all, COL_VIDEO);
}

/* ---------------- overlay pieces ---------------- */

static void paint_gradient(const Rc *clip, const Rc *ov)
{
    Rc r;
    int oh = ov->bottom - ov->top, x, y;
    if (oh <= 0 || !rc_isect(&r, clip, ov))
        return;
    for (y = r.top; y < r.bottom; y++) {
        double t = (double)(y - ov->top + 1) / (double)oh;
        int a = (int)(256.0 * 0.84 * pow(t, 1.3) + 0.5);
        int inv = 256 - a;
        uint32_t *px = g_bbits + (size_t)y * g_bw + r.left;
        for (x = r.left; x < r.right; x++, px++) {
            uint32_t c = *px;
            uint32_t b = ((c & 255) * inv + 12 * a) >> 8;
            uint32_t g = (((c >> 8) & 255) * inv + 12 * a) >> 8;
            uint32_t rr = (((c >> 16) & 255) * inv + 14 * a) >> 8;
            *px = 0xFF000000u | (rr << 16) | (g << 8) | b;
        }
    }
}

static void text_in(const char *s, Rc r, int fl, void *f, uint32_t c)
{
    text_draw((Font *)f, s, r, fl, c);
}

static void rect_center(const Rc *r, float *cx, float *cy)
{
    *cx = (r->left + r->right) / 2.0f;
    *cy = (r->top + r->bottom) / 2.0f;
}

static void fill_rgb(const Rc *r, uint32_t c)
{
    Rc a;
    int x, y;
    rc_set(&a, 0, 0, 0, 0);
    if (!rc_isect(&a, r, &CLIP))
        return;
    for (y = a.top; y < a.bottom; y++) {
        uint32_t *d = FB + (size_t)y * FW;
        for (x = a.left; x < a.right; x++)
            d[x] = c;
    }
}

static void blit_icon(const uint32_t *px, int sz, int x0, int y0)
{
    int x, y;
    if (!px)
        return;
    for (y = 0; y < sz; y++)
        for (x = 0; x < sz; x++) {
            uint32_t c = px[y * sz + x];
            put(x0 + x, y0 + y, c | 0, 1.0f);
        }
}

/* caption strip: black, icon + title on the left, flat buttons on the right */
static void draw_header(Player *p, const BL *L)
{
    float cx, cy, s = SF(10);
    int cap_h = L->cap.bottom - L->cap.top;
    int maxed = (SDL_GetWindowFlags(p->win) & SDL_WINDOW_MAXIMIZED) != 0;
    uint32_t hov = PX(28, 28, 28), dn = PX(46, 46, 46);

    fill_rgb(&L->cap, COL_CAPTION);

#define HOV(r, id, col_h, col_d) \
    if (p->hover == (id)) fill_rgb(&L->r, p->hover_down == (id) ? (col_d) : (col_h))
    HOV(c_full, HT_CAP_FULL, hov, dn);
    HOV(c_menu, HT_CAP_MENU, hov, dn);
    HOV(c_min, HT_CAP_MIN, hov, dn);
    HOV(c_max, HT_CAP_MAX, hov, dn);
    HOV(c_close, HT_CAP_CLOSE, PX(196, 43, 28), PX(160, 36, 24));
#undef HOV

    rect_center(&L->c_full, &cx, &cy);
    glyph_fs(cx, cy, s, A_TEXT, p->fullscreen);
    rect_center(&L->c_menu, &cx, &cy);
    glyph_menu(cx, cy, s, A_TEXT);
    rect_center(&L->c_min, &cx, &cy);
    glyph_min(cx, cy, s, A_TEXT);
    rect_center(&L->c_max, &cx, &cy);
    glyph_max(cx, cy, s * 0.95f, A_TEXT, maxed);
    rect_center(&L->c_close, &cx, &cy);
    glyph_close(cx, cy, s, A_TEXT);

    /* app icon + title, left aligned like a native title bar */
    {
        int isz = p->ico_sz;
        int ix = S_(10), iy = (cap_h - isz) / 2;
        Rc tr;
        if (isz > 0 && p->ico_px[p->ico_cur & 1])
            blit_icon(p->ico_px[p->ico_cur & 1], isz, ix, iy);
        rc_set(&tr, ix + isz + S_(8), L->cap.top, L->c_full.left - S_(8), L->cap.bottom);
        if (tr.right > tr.left + 8)
            text_in(p->title, tr, TF_LEFT | TF_VCENTER | TF_ELLIPSIS,
                    p->fnt_cap, COL_TEXT);
    }
}

static void hbg(const Rc *r, int on)
{
    if (on)
        grrect_r(r, SF(5), A_HOVER);
}

static void draw_bar(Player *p, const BL *L)
{
    float cx, cy, s = SF(L_ICON);

    int can_prev = p->pl_cur > 0 && p->pl_n > 0;
    int can_next = p->pl_cur + 1 < p->pl_n;

    rect_center(&L->b_prev, &cx, &cy);
    hbg(&L->b_prev, can_prev && p->hover == HT_BAR_PREV);
    glyph_prev(cx, cy, s, can_prev ? A_TEXT : A_OFF);

    rect_center(&L->b_play, &cx, &cy);
    hbg(&L->b_play, p->hover == HT_BAR_PLAY);
    if (p->paused || !p->playing)
        glyph_play(cx, cy, s, A_TEXT);
    else
        glyph_pause(cx, cy, s, A_TEXT);

    rect_center(&L->b_stop, &cx, &cy);
    hbg(&L->b_stop, p->hover == HT_BAR_STOP);
    glyph_stop(cx, cy, s, A_TEXT);

    rect_center(&L->b_next, &cx, &cy);
    hbg(&L->b_next, can_next && p->hover == HT_BAR_NEXT);
    glyph_next(cx, cy, s, can_next ? A_TEXT : A_OFF);

    rect_center(&L->b_loop, &cx, &cy);
    hbg(&L->b_loop, p->hover == HT_LOOP);
    glyph_loop(cx, cy, s, p->loop_file ? A_ACCENT : A_TEXT);

    rect_center(&L->b_vol, &cx, &cy);
    hbg(&L->b_vol, p->hover == HT_VOL);
    glyph_speaker(cx, cy, s, A_TEXT, p->volume, p->muted);

    /* seek track: thin line, white fill, white knob */
    {
        float x1 = (float)L->seek.left, x2 = (float)L->seek.right;
        float ty = (float)L->seek.top, th = (float)(L->seek.bottom - L->seek.top);
        float ky = ty + th / 2.0f;
        grrect(x1, ty, x2 - x1, th, th / 2.0f, ARGB(90, 255, 255, 255));
        if (p->playing && p->dur > 0) {
            double pos = (p->dragging == 1) ? p->drag_pos : ff_pos(p);
            double fr = pos / p->dur;
            if (fr < 0) fr = 0;
            if (fr > 1) fr = 1;
            float fx = x1 + (float)((x2 - x1) * fr);
            if (fx > x1 + 1)
                grrect(x1, ty, fx - x1, th, th / 2.0f, A_TEXT);
            gcircle(fx, ky, (p->hover == HT_SEEK || p->dragging == 1) ?
                    SF(6.5) : SF(5.5), A_TEXT);
        }
    }

    if (p->playing && p->time_txt[0])
        text_in(p->time_txt, L->time, TF_RIGHT | TF_VCENTER, p->fnt_bar, COL_TEXT);
}

static void draw_popup(Player *p, const BL *L)
{
    if (!vol_visible(p))
        return;
    grrect_r(&L->pop, SF(8), ARGB(255, 40, 40, 42));
    float cx = (L->pop_track.left + L->pop_track.right) / 2.0f;
    float y0 = (float)L->pop_track.top, y1 = (float)L->pop_track.bottom;
    float v = p->muted ? 0.0f : (float)p->volume;
    float ky = y1 - (y1 - y0) * v;
    grrect(cx - SF(1.5), y0, SF(3), y1 - y0, SF(1.5), ARGB(90, 255, 255, 255));
    if (y1 - ky > 1)
        grrect(cx - SF(1.5), ky, SF(3), y1 - ky, SF(1.5), A_TEXT);
    gcircle(cx, ky, SF(5.5), A_TEXT);
}

static void draw_toast(Player *p, const BL *L)
{
    if (!p->toast[0] || SDL_GetTicks() >= p->toast_until)
        return;
    int tw = text_w((Font *)p->fnt_bar, p->toast);
    int th = ((Font *)p->fnt_bar) ? ((Font *)p->fnt_bar)->asc - ((Font *)p->fnt_bar)->desc : S_(16);
    Rc tr;
    tr.left = L->vid.left + S_(14);
    tr.right = tr.left + tw + S_(24);
    tr.bottom = (L->has_bar ? L->bar.top : L->vid.bottom) - S_(10);
    tr.top = tr.bottom - th - S_(14);
    grrect_r(&tr, SF(8), ARGB(215, 20, 20, 22));
    tr.left += S_(10); tr.right -= S_(10); tr.top += S_(6); tr.bottom -= S_(6);
    text_in(p->toast, tr, TF_CENTER | TF_VCENTER, p->fnt_bar, COL_TEXT);
}

/* ---------------- popup menu ---------------- */

static int mi_h(const MenuItem *m) { return (m->flags & MI_SEP) ? S_(9) : S_(28); }

void ui_menu_place(Player *p, int x, int y, int right_align)
{
    int i, W, H, lw = 0, aw = 0, w, h = S_(8);
    Font *f = (Font *)p->fnt_bar;
    win_size(p, &W, &H);
    for (i = 0; i < p->menu_n; i++) {
        const MenuItem *m = &p->menu[i];
        int a = text_w(f, m->label), b = text_w(f, m->accel);
        if (a > lw) lw = a;
        if (b > aw) aw = b;
        h += mi_h(m);
    }
    w = S_(34) + lw + (aw ? S_(28) + aw : 0) + S_(14);
    if (w < S_(170))
        w = S_(170);
    if (right_align)
        x -= w;
    if (x + w > W) x = W - w;
    if (x < 0) x = 0;
    if (y + h > H) y = H - h;
    if (y < 0) y = 0;
    rc_set(&p->menu_rc, x, y, x + w, y + h);
    p->menu_sel = -1;
}

int ui_menu_hit(const Player *p, int x, int y)
{
    int i, ty = p->menu_rc.top + S_(4);
    if (!PIN(p->menu_rc, x, y))
        return -1;
    for (i = 0; i < p->menu_n; i++) {
        int h = mi_h(&p->menu[i]);
        if (y >= ty && y < ty + h)
            return (p->menu[i].flags & (MI_SEP | MI_DISABLED)) ? -1 : i;
        ty += h;
    }
    return -1;
}

static void draw_menu(Player *p)
{
    int i, ty;
    Rc r = p->menu_rc;
    Font *f = (Font *)p->fnt_bar;
    if (!p->menu_open || p->menu_n <= 0)
        return;
    grrect_r(&r, SF(7), ARGB(255, 70, 70, 74));
    {
        Rc in = r;
        in.left++; in.top++; in.right--; in.bottom--;
        grrect_r(&in, SF(6), ARGB(255, 40, 40, 42));
    }
    ty = r.top + S_(4);
    for (i = 0; i < p->menu_n; i++) {
        const MenuItem *m = &p->menu[i];
        int h = mi_h(m);
        Rc ir;
        rc_set(&ir, r.left + S_(4), ty, r.right - S_(4), ty + h);
        if (m->flags & MI_SEP) {
            grrect(ir.left + S_(4), (float)(ty + h / 2), (float)(ir.right - ir.left - S_(8)),
                   fmaxf(1.0f, SF(1.0)), 0, ARGB(70, 255, 255, 255));
        } else {
            int dis = (m->flags & MI_DISABLED) != 0;
            uint32_t col = dis ? COL_DIM : COL_TEXT;
            Rc tr;
            if (!dis && i == p->menu_sel)
                grrect_r(&ir, SF(4), ARGB(60, 255, 255, 255));
            if (m->flags & MI_CHECKED) {
                float cx = ir.left + S_(14), cy = ty + h / 2.0f, s = SF(9);
                float pw = fmaxf(1.3f, SF(1.5));
                gline(cx - s * 0.45f, cy, cx - s * 0.1f, cy + s * 0.4f, pw, col);
                gline(cx - s * 0.1f, cy + s * 0.4f, cx + s * 0.55f, cy - s * 0.4f, pw, col);
            }
            rc_set(&tr, ir.left + S_(28), ty, ir.right - S_(10), ty + h);
            text_in(m->label, tr, TF_LEFT | TF_VCENTER, f, col);
            if (m->accel[0])
                text_in(m->accel, tr, TF_RIGHT | TF_VCENTER, f, COL_DIM);
        }
        ty += h;
    }
}

/* ---------------- paint ---------------- */

void ui_paint(Player *p)
{
    int W, H, y, w, h;
    Rc rc, pr;
    if (!g_have_dirty || !p->win)
        return;
    win_size(p, &W, &H);
    rc_set(&rc, 0, 0, W, H);
    if (W <= 0 || H <= 0 || !rc_isect(&pr, &g_dirty, &rc)) {
        g_have_dirty = 0;
        return;
    }
    g_have_dirty = 0;
    if (ensure_buffers(W, H))
        pr = rc;
    if (!g_cbits || !g_bbits || !g_bsurf)
        return;

    BL L;
    bar_calc(p, &L);
    if (g_cdirty) {
        render_cache(p, &L);
        g_cdirty = 0;
    }
    w = pr.right - pr.left;
    h = pr.bottom - pr.top;
    for (y = pr.top; y < pr.bottom; y++)
        memcpy(g_bbits + (size_t)y * g_bw + pr.left,
               g_cbits + (size_t)y * g_bw + pr.left, (size_t)w * 4);

    FB = g_bbits;
    FW = g_bw;
    FH = g_bh;
    CLIP = pr;

    if (!p->playing) {
        Rc vr = L.vid;
        text_in("Drop a file here, or press O to open", vr,
                TF_CENTER | TF_VCENTER, p->fnt_hint, COL_DIM);
    }
    if (L.has_bar)
        paint_gradient(&pr, &L.ov);
    if (L.has_cap)
        draw_header(p, &L);
    if (L.has_bar) {
        draw_bar(p, &L);
        draw_popup(p, &L);
    }
    draw_toast(p, &L);
    draw_menu(p);

    if (!p->fullscreen && !(SDL_GetWindowFlags(p->win) & SDL_WINDOW_MAXIMIZED)) {
        uint32_t bc = PX(58, 58, 60);           /* 1 px window outline */
        Rc t, b, l, r;
        rc_set(&t, 0, 0, W, 1);
        rc_set(&b, 0, H - 1, W, H);
        rc_set(&l, 0, 0, 1, H);
        rc_set(&r, W - 1, 0, W, H);
        fill_rgb(&t, bc); fill_rgb(&b, bc); fill_rgb(&l, bc); fill_rgb(&r, bc);
    }

    {
        SDL_Surface *ws = SDL_GetWindowSurface(p->win);
        if (ws && (ws->w != W || ws->h != H)) {     /* stale after a programmatic resize */
            SDL_DestroyWindowSurface(p->win);
            ws = SDL_GetWindowSurface(p->win);
        }
        if (ws && ws->w == W && ws->h == H) {
            SDL_Rect sr;
            sr.x = pr.left; sr.y = pr.top; sr.w = w; sr.h = h;
            SDL_BlitSurface(g_bsurf, &sr, ws, &sr);
            SDL_UpdateWindowSurfaceRects(p->win, &sr, 1);
        }
    }
}
