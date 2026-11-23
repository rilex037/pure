/* player.h - PurePlayer: native FFmpeg + SDL3 video player (Windows, Linux)
 * Plain C. No frameworks, no libmpv.
 *
 * Engine (ff.c):      demux/video/audio threads, packet queues, frame slots,
 *                     audio-master clock (SDL audio stream), seeking, PNG export,
 *                     clipboard copy, frame step back/forward.
 * UI core (ui_core.c): window, input, hotkeys, menu, timer, playlist.
 * UI draw (ui_draw.c): software rasterizer, dark chrome, vector icons, text.
 * Platform (plat.c):   single instance, icons, font lookup, clipboard image.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stddef.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_TITLE    "Pure Player"
#define APP_VERSION  "1.0.2"
#define PATHLEN      2048           /* bytes, UTF-8 */

/* layout constants, 96-dpi pixels (scaled at runtime) */
enum { L_BAR_H = 44, L_CAP_H = 34, L_EDGE = 6, L_ICON = 14, L_MIN_W = 420 };

/* resource ids (assets/app.rc, Windows) */
#define IDI_VIDEO  1
#define IDI_MUSIC  2

/* SDL_EVENT_USER codes */
#define UEV_OPEN   1                /* data1 = malloc'ed "a\0b\0\0" list of paths */

/* colours: 0xAARRGGBB */
#define PX(r, g, b)  ((uint32_t)(0xFF000000u | ((uint32_t)(r) << 16) | \
                                 ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define COL_VIDEO    PX(0, 0, 0)
#define COL_CAPTION  PX(0, 0, 0)
#define COL_TEXT     PX(238, 238, 236)
#define COL_DIM      PX(160, 160, 158)
#define COL_ACCENT   PX(53, 132, 228)

/* mouse hit ids */
enum {
    HT_NONE = 0,
    HT_CAP_FULL, HT_CAP_MENU, HT_CAP_MIN, HT_CAP_MAX, HT_CAP_CLOSE,
    HT_BAR_PREV, HT_BAR_PLAY, HT_BAR_STOP, HT_BAR_NEXT,
    HT_SEEK, HT_TIME, HT_LOOP, HT_VOL, HT_VOL_TRACK, HT_VIDEO
};

typedef struct { int left, top, right, bottom; } Rc;

/* ---------------- packet queue ---------------- */
typedef struct PktNode {
    AVPacket        pkt;        /* owns one reference */
    struct PktNode *next;
    int             serial;
} PktNode;

typedef struct {
    PktNode *first, *last;
    int      nb, serial;
    size_t   bytes;
} PktQ;

/* ---------------- decoded frame slots ---------------- */
typedef struct {
    AVFrame *bgra;      /* pre-allocated AV_PIX_FMT_BGRA frame */
    AVFrame *yuv;       /* the decoded frame (reference), moved to the history
                         * when the slot is displayed */
    double   pts;       /* seconds */
    int      serial;
    int      state;     /* 0 = free, 1 = owned (fifo / shown),
                         * 2 = being filled by the video thread,
                         * 3 = the reserved "view" slot (history display) */
} Slot;

#define SLOT_MAX 16

/* frame history for stepping back: decoded YUV frames, newest at yh_head */
#define YH_MAX 48
typedef struct { AVFrame *f; double pts; } YEnt;

/* popup menu (drawn by ui_draw.c, driven by ui_core.c) */
#define MENU_MAX 24
enum { MI_CHECKED = 1, MI_DISABLED = 2, MI_SEP = 4 };
typedef struct { int id, flags; char label[40], accel[16]; } MenuItem;

/* ---------------- global player state ---------------- */
typedef struct Player {
    /* playlist */
    char   (*pl)[PATHLEN];
    int     pl_n, pl_cur, pl_cap;

    /* ffmpeg */
    AVFormatContext *fmt;
    int      vs, as;            /* stream indexes, -1 = none */
    AVCodecContext *vctx, *actx;
    AVStream *vst, *ast;
    struct SwsContext *sws;
    struct SwrContext *swr;
    int      vw, vh;            /* video size, 0 = audio only */
    AVFrame *cover;             /* BGRA cover art of an audio file, or NULL */

    /* threads */
    SDL_Thread *th[3];          /* demux, video, audio */
    volatile int quit;

    /* sync / serial */
    int      serial_gen, cur_serial;
    volatile int seek_req;
    double   seek_target;
    volatile int seek_pending;  /* first frame of current seek not yet shown */
    volatile int eof;
    volatile int video_done, audio_done;
    double   dur;
    double   last_vpts;         /* video-thread only */

    /* audio master clock: SDL audio stream, position = written - queued */
    int      has_audio;
    SDL_AudioStream *astream;
    int      sample_rate;
    int      dev_frames;        /* device buffer, in frames (latency estimate) */
    int      aq_max;            /* max bytes queued in the stream (~400 ms) */
    double   audio_base;
    volatile unsigned long long samples_written;
    volatile int audio_reset_pending;
    double   audio_target;
    int      audio_started;
    uint8_t *aubuf;             /* swresample output, audio thread only */
    int      aubuf_cap;
    double   aclk_est;          /* smoothed clock (ui thread) */
    Uint64   aclk_t;
    int      aclk_valid;

    /* no-audio fallback clock (perf counter) */
    double   sys_base;
    Uint64   sys_t0_ticks;
    Uint64   pc_freq;
    volatile int paused;
    volatile int clock_sys;     /* audio finished: perf counter takes over */
    double   pend_target;       /* seek target the ui is catching up to */

    /* queues and slots (guarded by lock) */
    SDL_Mutex *lock;
    PktQ     pqv, pqa;

    Slot     slot[SLOT_MAX];
    int      slot_n;
    int      fifo[SLOT_MAX];            /* undisplayed slots, oldest at fifo_r */
    int      fifo_n, fifo_r;
    double  *kf;                        /* keyframe times from the index (sorted) */
    int      kf_n;
    YEnt     yh[YH_MAX];                /* history ring (decoded YUV frames) */
    int      yh_cap, yh_n, yh_head;
    int      yh_pos;                    /* frames stepped back from newest */
    int      view_slot;                 /* BGRA slot used to show history frames */
    struct SwsContext *sws_view;        /* ui-thread converter for the view slot */
    int      shown_slot;                /* -1 = nothing on screen */

    /* ui state (ui thread only) */
    SDL_Window *win;
    float    density;                   /* pixels per window coordinate unit */
    int      playing;
    int      loop_file;
    double   volume;                    /* 0..1 */
    int      muted;
    int      fullscreen;
    int      want_quit;
    Uint64   last_input;
    Uint64   vol_show_until;            /* volume popup stays until this tick */
    int      bars_visible, mouse_tracked;
    int      hover, hover_down;
    int      dragging;                  /* 0 none, 1 seek, 2 volume */
    double   drag_pos;
    int      pre_click_paused;
    int      dpi;
    int      ui_last_cur, ui_last_playing, ui_last_q;
    char     time_txt[64];
    int      time_w;
    char     toast[192];
    Uint64   toast_until;
    char     title[512];
    int      cursor_hidden;

    /* popup menu */
    int      menu_open, menu_sel;
    MenuItem menu[MENU_MAX];
    int      menu_n;
    Rc       menu_rc;

    void    *fnt_cap, *fnt_bar, *fnt_hint;     /* Font* (ui_draw.c) */

    /* video geometry / seek state added for the overlay ui */
    double   vdar, vfd;                 /* display aspect ratio, frame duration */
    volatile int media_gen;             /* ++ on every successful ff_open */
    volatile int seek_mode, pend_mode;  /* 0 = land on/after target, 1 = step back */
    int      pend_slot;                 /* newest catch-up frame while seeking */
    volatile int stepped;               /* user stepped frames while paused */

    int      ctl_hidden;                /* "Toggle Controls" */
    int      scrub_was_paused;
    Uint64   last_scrub;
    double   scrub_sent;
    int      last_mx, last_my;
    int      ui_last_gen;
    int      ui_last_n;

    /* app icons, BGRA ico_sz x ico_sz: [0] video, [1] music */
    int      ico_sz;
    uint32_t *ico_px[2];
    int      ico_cur;
} Player;

extern Player g_player;

/* ---------------- ff.c ---------------- */
void   ff_init(Player *p);
int    ff_open(Player *p, const char *path_utf8);
void   ff_close(Player *p);             /* unload media, keep playlist */
void   ff_shutdown(Player *p);          /* unload + free playlist */
int    ff_seek(Player *p, double sec);
int    ff_scrub(Player *p, double sec);       /* exact frame for the seekbar drag */
double ff_clock(Player *p);
double ff_pos(Player *p);
void   ff_set_paused(Player *p, int pause);
void   ff_set_volume(Player *p);
int    ff_step_fwd(Player *p);          /* 1 = new frame shown */
int    ff_step_back(Player *p);         /* 1 = older frame shown */
int    ff_tick(Player *p);              /* 1 = new frame shown (repaint video) */
int    ff_eop(Player *p);               /* end of playback reached */
int    ff_at_end(Player *p);            /* everything played (even if paused) */
int    ff_save_frame_png(Player *p, char *out_name, int out_cap);
int    ff_copy_frame(Player *p);        /* clipboard image */
/* BGRA -> PNG in memory (malloc'ed *out, free with free()); 0 = ok */
int    ff_png_from_bgra(const uint8_t *bgra, int w, int h, int stride,
                        int keep_alpha, uint8_t **out, size_t *outsz);

/* ---------------- ui_draw.c ---------------- */
void   ui_init_fonts(Player *p);
void   ui_free_fonts(Player *p);
void   ui_layout(const Player *p, Rc *cap, Rc *video, Rc *bar);
int    ui_cap_h(const Player *p);      /* caption strip height, px */
int    ui_hit(Player *p, int x, int y);
void   ui_paint(Player *p);            /* repaint whatever ui_invalidate marked */
void   ui_invalidate(Player *p, int video, int chrome);
void   ui_invalidate_all(Player *p);
void   ui_invalidate_rect(Player *p, const Rc *r);
void   ui_fmt_time(char *o, int cap, double s);
void   ui_gfx_init(void);
void   ui_gfx_free(void);
double ui_seek_frac(const Player *p, int x);   /* mouse x -> 0..1 on the seek track */
double ui_vol_frac(const Player *p, int y);    /* mouse y -> 0..1 on the volume popup */
void   ui_menu_anchor(const Player *p, int *x, int *y);  /* header menu button */
void   ui_menu_place(Player *p, int x, int y, int right_align);
int    ui_menu_hit(const Player *p, int x, int y);       /* item index or -1 */
void   ui_render_icon(int music, int size, uint32_t *px);  /* procedural icon */

/* ---------------- ui_core.c ---------------- */
void   ui_toast(Player *p, const char *fmt, ...);

/* ---------------- plat.c ---------------- */
/* 1 = files were handed to the running instance, exit now.
 * 0 = this is the only instance; a listener posts UEV_OPEN events. */
int    plat_single_instance(char (*files)[PATHLEN], int n);
void   plat_ipc_stop(void);
int    plat_find_font(char *out, int cap);
void   plat_post_open(const char *list, size_t len);   /* queue a UEV_OPEN event */
void   plat_icons(Player *p);           /* fills ico_sz / ico_px */
void   plat_window_icon(Player *p, int music);
int    plat_clip_image(Player *p, const uint8_t *bgra, int w, int h, int stride);

#ifdef __cplusplus
}
#endif
#endif /* PLAYER_H */
