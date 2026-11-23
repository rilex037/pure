/* plat.c - the few things SDL does not cover: single-instance hand-off,
 * app icons, system font lookup, image clipboard.
 * Windows: named pipe + mutex, resource icons, CF_DIB.
 * Linux:   unix socket + flock, procedural icons, image/png clipboard.
 */
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

/* ---------------- shared helpers ---------------- */

/* "a\0b\0\0" */
static char *pack_files(char (*files)[PATHLEN], int n, size_t *len)
{
    size_t tot = 1, pos = 0;
    int i;
    char *buf;
    for (i = 0; i < n; i++)
        tot += strlen(files[i]) + 1;
    buf = (char *)calloc(tot, 1);
    if (!buf)
        return NULL;
    for (i = 0; i < n; i++) {
        size_t l = strlen(files[i]);
        memcpy(buf + pos, files[i], l);
        pos += l + 1;
    }
    *len = tot;
    return buf;
}

/* hand a received list to the ui thread */
void plat_post_open(const char *data, size_t len)
{
    SDL_Event e;
    char *copy = (char *)calloc(len + 2, 1);
    if (!copy)
        return;
    memcpy(copy, data, len);
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    e.user.code = UEV_OPEN;
    e.user.data1 = copy;
    if (!SDL_PushEvent(&e))
        free(copy);
}

#ifndef _WIN32
static int file_exists(const char *path)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi) && pi.type == SDL_PATHTYPE_FILE;
}
#endif

#define IPC_MAX (4u << 20)

static volatile int g_stop;
static SDL_Thread *g_ipc_th;

/* ============================== Windows ============================== */
#ifdef _WIN32

#define PIPE_NAME L"\\\\.\\pipe\\PurePlayer.SingleInstance"

static HANDLE g_mutex, g_pipe;
static HICON g_hbig[2], g_hsmall[2];

static int SDLCALL ipc_thread(void *arg)
{
    (void)arg;
    for (;;) {
        BOOL ok = ConnectNamedPipe(g_pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (g_stop)
            break;
        if (ok) {
            size_t cap = 4096, len = 0;
            char *buf = (char *)malloc(cap);
            while (buf) {
                DWORD got = 0;
                if (len + 4096 > cap) {
                    char *nb;
                    if (cap > IPC_MAX)
                        break;
                    nb = (char *)realloc(buf, cap * 2);
                    if (!nb)
                        break;
                    buf = nb;
                    cap *= 2;
                }
                if (!ReadFile(g_pipe, buf + len, 4096, &got, NULL) || got == 0)
                    break;
                len += got;
            }
            if (buf && len > 0)
                plat_post_open(buf, len);
            free(buf);
        }
        DisconnectNamedPipe(g_pipe);
    }
    return 0;
}

int plat_single_instance(char (*files)[PATHLEN], int n)
{
    g_mutex = CreateMutexW(NULL, TRUE, L"Local\\PurePlayer.SingleInstance");
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        int tries;
        size_t len = 0;
        char *buf = pack_files(files, n, &len);
        AllowSetForegroundWindow(ASFW_ANY);
        for (tries = 0; tries < 60 && buf; tries++) {   /* it may still start up */
            HANDLE h = CreateFileW(PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                if (n > 0)
                    WriteFile(h, buf, (DWORD)len, &wr, NULL);
                CloseHandle(h);
                break;
            }
            if (GetLastError() == ERROR_PIPE_BUSY)
                WaitNamedPipeW(PIPE_NAME, 500);
            else
                Sleep(100);
        }
        free(buf);
        return 1;
    }
    g_pipe = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_INBOUND,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                              1, 0, 65536, 0, NULL);
    if (g_pipe != INVALID_HANDLE_VALUE)
        g_ipc_th = SDL_CreateThread(ipc_thread, "ipc", NULL);
    return 0;
}

void plat_ipc_stop(void)
{
    if (g_ipc_th) {
        HANDLE h;
        g_stop = 1;
        h = CreateFileW(PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);                     /* wakes ConnectNamedPipe */
        SDL_WaitThread(g_ipc_th, NULL);
        g_ipc_th = NULL;
    }
    if (g_pipe && g_pipe != INVALID_HANDLE_VALUE)
        CloseHandle(g_pipe);
    if (g_mutex)
        CloseHandle(g_mutex);
}

static int w2u8(const wchar_t *w, char *out, int cap)
{
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) > 0;
}

int plat_find_font(char *out, int cap)
{
    static const wchar_t *names[] = { L"\\Fonts\\segoeui.ttf", L"\\Fonts\\tahoma.ttf",
                                      L"\\Fonts\\arial.ttf" };
    wchar_t dir[MAX_PATH], full[MAX_PATH + 40];
    int i;
    if (!GetWindowsDirectoryW(dir, MAX_PATH))
        return 0;
    for (i = 0; i < 3; i++) {
        _snwprintf(full, MAX_PATH + 39, L"%s%s", dir, names[i]);
        full[MAX_PATH + 39] = 0;
        if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES && w2u8(full, out, cap))
            return 1;
    }
    return 0;
}

/* HICON -> straight-alpha BGRA, size in *sz */
static uint32_t *hicon_px(HICON ic, int *sz)
{
    ICONINFO ii;
    BITMAP bm;
    BITMAPINFO bi;
    uint32_t *px = NULL, *mk;
    HDC dc;
    int w, h, i, has = 0;

    if (!ic || !GetIconInfo(ic, &ii))
        return NULL;
    if (!ii.hbmColor || !GetObjectW(ii.hbmColor, sizeof bm, &bm)) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return NULL;
    }
    w = bm.bmWidth;
    h = bm.bmHeight;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    px = (uint32_t *)calloc((size_t)w * h, 4);
    mk = (uint32_t *)calloc((size_t)w * h, 4);
    dc = GetDC(NULL);
    if (px && mk && dc) {
        GetDIBits(dc, ii.hbmColor, 0, (UINT)h, px, &bi, DIB_RGB_COLORS);
        for (i = 0; i < w * h; i++)
            if (px[i] >> 24) { has = 1; break; }
        if (!has && ii.hbmMask) {               /* old style icon: AND mask */
            GetDIBits(dc, ii.hbmMask, 0, (UINT)h, mk, &bi, DIB_RGB_COLORS);
            for (i = 0; i < w * h; i++)
                px[i] = (mk[i] & 0xFFFFFF) ? 0 : (px[i] | 0xFF000000u);
        }
    }
    if (dc)
        ReleaseDC(NULL, dc);
    free(mk);
    DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
    *sz = (w == h) ? w : 0;
    if (*sz == 0) {
        free(px);
        return NULL;
    }
    return px;
}

void plat_icons(Player *p)
{
    HINSTANCE hi = GetModuleHandleW(NULL);
    int i, sz = (16 * p->dpi + 48) / 96;
    int bw = GetSystemMetrics(SM_CXICON), bh = GetSystemMetrics(SM_CYICON);
    for (i = 0; i < 2; i++) {
        int got = 0;
        int id = i ? IDI_MUSIC : IDI_VIDEO;
        g_hbig[i] = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(id), IMAGE_ICON, bw, bh,
                                      LR_DEFAULTCOLOR);
        g_hsmall[i] = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(id), IMAGE_ICON, sz, sz,
                                        LR_DEFAULTCOLOR);
        p->ico_px[i] = hicon_px(g_hsmall[i], &got);
        if (p->ico_px[i])
            p->ico_sz = got;
    }
    if (!p->ico_px[0] || !p->ico_px[1])         /* keep both the same size */
        p->ico_sz = p->ico_px[0] || p->ico_px[1] ? p->ico_sz : 0;
}

void plat_window_icon(Player *p, int music)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(p->win),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    int i = music ? 1 : 0;
    if (!hwnd)
        return;
    if (g_hbig[i])
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hbig[i]);
    if (g_hsmall[i])
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hsmall[i]);
}

int plat_clip_image(Player *p, const uint8_t *bgra, int w, int h, int stride)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(p->win),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    size_t sz = 40 + (size_t)w * h * 4;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sz);
    uint8_t *base, *dst;
    BITMAPINFOHEADER *bh;
    int y, x;
    if (!hg)
        return -2;
    base = (uint8_t *)GlobalLock(hg);
    if (!base) {
        GlobalFree(hg);
        return -2;
    }
    bh = (BITMAPINFOHEADER *)base;
    bh->biSize = 40;
    bh->biWidth = w;
    bh->biHeight = h;            /* positive = bottom-up */
    bh->biPlanes = 1;
    bh->biBitCount = 32;
    bh->biCompression = BI_RGB;
    bh->biSizeImage = (DWORD)((size_t)w * h * 4);
    dst = base + 40;
    for (y = 0; y < h; y++) {
        const uint8_t *s = bgra + (size_t)(h - 1 - y) * stride;
        uint8_t *d = dst + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 0xFF;         /* opaque */
            d += 4;
            s += 4;
        }
    }
    GlobalUnlock(hg);
    if (!OpenClipboard(hwnd)) {
        GlobalFree(hg);
        return -3;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, hg)) {
        CloseClipboard();
        GlobalFree(hg);
        return -4;
    }
    CloseClipboard();
    return 0;
}

/* ============================== Linux / other ============================== */
#else

static int g_lockfd = -1, g_srv = -1;
static char g_sock[256], g_lock[256];

static int sock_paths(void)
{
    const char *d = getenv("XDG_RUNTIME_DIR");
    if (!d || !*d)
        d = "/tmp";
    snprintf(g_sock, sizeof g_sock, "%s/pure-player-%u.sock", d, (unsigned)getuid());
    snprintf(g_lock, sizeof g_lock, "%s/pure-player-%u.lock", d, (unsigned)getuid());
    return strlen(g_sock) < sizeof(((struct sockaddr_un *)0)->sun_path);
}

static int sock_connect(void)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, g_sock, strlen(g_sock) + 1);   /* length checked in sock_paths */
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int SDLCALL ipc_thread(void *arg)
{
    (void)arg;
    while (!g_stop) {
        struct pollfd pf;
        int c;
        pf.fd = g_srv; pf.events = POLLIN; pf.revents = 0;
        if (poll(&pf, 1, 100) <= 0)
            continue;
        c = accept(g_srv, NULL, NULL);
        if (c < 0)
            continue;
        {
            size_t cap = 4096, len = 0;
            char *buf = (char *)malloc(cap);
            while (buf) {
                ssize_t got;
                struct pollfd cp;
                cp.fd = c; cp.events = POLLIN; cp.revents = 0;
                if (poll(&cp, 1, 1000) <= 0)
                    break;
                if (len + 4096 > cap) {
                    char *nb;
                    if (cap > IPC_MAX)
                        break;
                    nb = (char *)realloc(buf, cap * 2);
                    if (!nb)
                        break;
                    buf = nb;
                    cap *= 2;
                }
                got = read(c, buf + len, 4096);
                if (got <= 0)
                    break;
                len += (size_t)got;
            }
            if (buf && len > 0)
                plat_post_open(buf, len);
            free(buf);
        }
        close(c);
    }
    return 0;
}

int plat_single_instance(char (*files)[PATHLEN], int n)
{
    int have_paths = sock_paths();
    if (!have_paths)
        return 0;
    g_lockfd = open(g_lock, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_lockfd >= 0 && flock(g_lockfd, LOCK_EX | LOCK_NB) != 0) {
        /* another instance holds the lock: hand the files over */
        int tries;
        size_t len = 0;
        char *buf = pack_files(files, n, &len);
        for (tries = 0; tries < 60 && buf; tries++) {   /* it may still start up */
            int fd = sock_connect();
            if (fd >= 0) {
                size_t off = 0;
                while (n > 0 && off < len) {
                    ssize_t w = write(fd, buf + off, len - off);
                    if (w <= 0)
                        break;
                    off += (size_t)w;
                }
                close(fd);
                break;
            }
            SDL_Delay(100);
        }
        free(buf);
        return 1;
    }
    if (g_lockfd >= 0) {                                /* we are the instance */
        struct sockaddr_un sa;
        g_srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (g_srv >= 0) {
            memset(&sa, 0, sizeof sa);
            sa.sun_family = AF_UNIX;
            memcpy(sa.sun_path, g_sock, strlen(g_sock) + 1);   /* length checked in sock_paths */
            unlink(g_sock);
            if (bind(g_srv, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(g_srv, 8) == 0) {
                g_ipc_th = SDL_CreateThread(ipc_thread, "ipc", NULL);
            } else {
                close(g_srv);
                g_srv = -1;
            }
        }
    }
    return 0;
}

void plat_ipc_stop(void)
{
    if (g_ipc_th) {
        g_stop = 1;
        SDL_WaitThread(g_ipc_th, NULL);
        g_ipc_th = NULL;
    }
    if (g_srv >= 0) {
        close(g_srv);
        unlink(g_sock);
        g_srv = -1;
    }
    if (g_lockfd >= 0) {
        close(g_lockfd);
        g_lockfd = -1;
    }
}

int plat_find_font(char *out, int cap)
{
    static const char *fallback[] = {
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    char path[PATHLEN];
    FILE *f = popen("fc-match -f '%{file}' sans:regular 2>/dev/null", "r");
    size_t i;
    if (f) {
        size_t got = fread(path, 1, sizeof path - 1, f);
        pclose(f);
        path[got] = 0;
        if (got > 0 && file_exists(path) && (int)got < cap) {
            strcpy(out, path);
            return 1;
        }
    }
    for (i = 0; i < sizeof fallback / sizeof fallback[0]; i++)
        if (file_exists(fallback[i]) && (int)strlen(fallback[i]) < cap) {
            strcpy(out, fallback[i]);
            return 1;
        }
    return 0;
}

void plat_icons(Player *p)
{
    int i, sz = (16 * p->dpi + 48) / 96;
    for (i = 0; i < 2; i++) {
        p->ico_px[i] = (uint32_t *)calloc((size_t)sz * sz, 4);
        if (p->ico_px[i])
            ui_render_icon(i, sz, p->ico_px[i]);
    }
    p->ico_sz = (p->ico_px[0] && p->ico_px[1]) ? sz : 0;
}

void plat_window_icon(Player *p, int music)
{
    enum { N = 64 };
    uint32_t *px = (uint32_t *)calloc(N * N, 4);
    SDL_Surface *s;
    if (!px)
        return;
    ui_render_icon(music ? 1 : 0, N, px);
    s = SDL_CreateSurfaceFrom(N, N, SDL_PIXELFORMAT_ARGB8888, px, N * 4);
    if (s) {
        SDL_SetWindowIcon(p->win, s);           /* copies the pixels */
        SDL_DestroySurface(s);
    }
    free(px);
}

typedef struct { uint8_t *d; size_t n; } ClipPng;

static const void *SDLCALL clip_cb(void *ud, const char *mime, size_t *size)
{
    ClipPng *c = (ClipPng *)ud;
    if (!mime || strcmp(mime, "image/png") != 0) {
        *size = 0;
        return NULL;
    }
    *size = c->n;
    return c->d;
}

static void SDLCALL clip_cleanup(void *ud)
{
    ClipPng *c = (ClipPng *)ud;
    if (c) {
        free(c->d);
        free(c);
    }
}

int plat_clip_image(Player *p, const uint8_t *bgra, int w, int h, int stride)
{
    static const char *mimes[] = { "image/png" };
    ClipPng *c;
    (void)p;
    c = (ClipPng *)calloc(1, sizeof *c);
    if (!c)
        return -2;
    if (ff_png_from_bgra(bgra, w, h, stride, 0, &c->d, &c->n) != 0) {
        free(c);
        return -3;
    }
    if (!SDL_SetClipboardData(clip_cb, clip_cleanup, c, mimes, 1)) {
        clip_cleanup(c);
        return -4;
    }
    return 0;
}

#endif
