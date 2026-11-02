/* notasplash: boot animation from early boot until tvshell takes the screen.
 *
 * Started by /etc/init.d/S01notasplash right after the display driver is loaded. Draws the NotaTV name and
 * three pulsing dots straight into a DRM dumb buffer (no GPU, no SDL), so the TV shows that the box is alive
 * while the init scripts run. tvshell stops it (SIGTERM via the pid file) right before it opens the display;
 * the pid file is removed only after the DRM device is closed, so tvshell can wait for it.
 *
 * Test without a display: notasplash --ppm out.ppm 1920x1080 [seconds]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/kd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "vendor/stb_truetype.h"

#define PIDFILE "/run/notasplash.pid"
#define FONT "/usr/share/tvshell/Inter-Bold.ttf"
#define MAX_SECONDS (15 * 60) /* never keep animating forever if tvshell does not come */

typedef struct { uint32_t *px; int w, h, stride; /* stride in pixels */ } Canvas;

static volatile sig_atomic_t quit;
static void on_term(int s) { (void)s; quit = 1; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------- drawing (same colours as the home screen: background 16,19,27 -> 7,8,12, accent 79,140,255) ---- */
static const uint8_t bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint32_t bg_at(const Canvas *c, int x, int y) {
    float t = c->h > 1 ? (float)y / (c->h - 1) : 0;
    float d = (bayer4[y & 3][x & 3] + 0.5f) / 16.0f; /* ordered dither: no banding in the dark gradient */
    int r = (int)(16 + (7 - 16) * t + d), g = (int)(19 + (8 - 19) * t + d), b = (int)(27 + (12 - 27) * t + d);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static void fill_bg(Canvas *c, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (size_t)y * c->stride;
        for (int x = x0; x < x1; x++) row[x] = bg_at(c, x, y);
    }
}

static uint32_t blend(uint32_t dst, int r, int g, int b, float a) {
    int dr = dst >> 16 & 255, dg = dst >> 8 & 255, db = dst & 255;
    dr += (int)((r - dr) * a + 0.5f);
    dg += (int)((g - dg) * a + 0.5f);
    db += (int)((b - db) * a + 0.5f);
    return (uint32_t)dr << 16 | (uint32_t)dg << 8 | (uint32_t)db;
}

/* text centred on (cx, baseline); returns 0 if the font is missing */
static int draw_text(Canvas *c, const unsigned char *font, const char *s, float px, int cx, int base, int r, int g,
                     int b) {
    stbtt_fontinfo fi;
    if (!font || !stbtt_InitFont(&fi, font, stbtt_GetFontOffsetForIndex(font, 0))) return 0;
    float sc = stbtt_ScaleForPixelHeight(&fi, px);
    float w = 0;
    for (const char *p = s; *p; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&fi, *p, &adv, &lsb);
        w += adv * sc;
        if (p[1]) w += stbtt_GetCodepointKernAdvance(&fi, *p, p[1]) * sc;
    }
    float x = cx - w / 2;
    for (const char *p = s; *p; p++) {
        int adv, lsb, gw, gh, ox, oy;
        stbtt_GetCodepointHMetrics(&fi, *p, &adv, &lsb);
        float fx = x - floorf(x);
        unsigned char *bm = stbtt_GetCodepointBitmapSubpixel(&fi, sc, sc, fx, 0, *p, &gw, &gh, &ox, &oy);
        int gx = (int)floorf(x) + ox, gy = base + oy;
        for (int j = 0; bm && j < gh; j++) {
            int y = gy + j;
            if (y < 0 || y >= c->h) continue;
            for (int i = 0; i < gw; i++) {
                int xx = gx + i;
                unsigned a = bm[j * gw + i];
                if (xx < 0 || xx >= c->w || !a) continue;
                uint32_t *d = c->px + (size_t)y * c->stride + xx;
                *d = blend(*d, r, g, b, a / 255.0f);
            }
        }
        stbtt_FreeBitmap(bm, NULL);
        x += adv * sc;
        if (p[1]) x += stbtt_GetCodepointKernAdvance(&fi, *p, p[1]) * sc;
    }
    return 1;
}

/* anti-aliased filled circle */
static void draw_dot(Canvas *c, float cx, float cy, float rad, int r, int g, int b, float alpha) {
    int x0 = (int)(cx - rad - 1), x1 = (int)(cx + rad + 2), y0 = (int)(cy - rad - 1), y1 = (int)(cy + rad + 2);
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < c->w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = rad + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            uint32_t *d = c->px + (size_t)y * c->stride + x;
            *d = blend(*d, r, g, b, cov * alpha);
        }
}

/* layout, relative to the screen height so it looks the same at 1080p and 4K */
typedef struct { int cx, base, dots_y; float title_px, rad, gap; } Layout;

static Layout layout(const Canvas *c) {
    Layout L;
    L.cx = c->w / 2;
    L.title_px = c->h * 0.085f;
    L.base = (int)(c->h * 0.5f);
    L.rad = c->h * 0.0065f;
    L.gap = c->h * 0.03f;
    L.dots_y = (int)(c->h * 0.58f);
    return L;
}

static void draw_static(Canvas *c, const unsigned char *font) {
    Layout L = layout(c);
    fill_bg(c, 0, 0, c->w, c->h);
    draw_text(c, font, "NotaTV", L.title_px, L.cx, L.base, 242, 244, 248);
}

/* three dots in a wave (one cycle per 1.2 s); only their small area is redrawn */
static void draw_dots(Canvas *c, double t) {
    Layout L = layout(c);
    int pad = (int)(L.rad * 2 + 2);
    fill_bg(c, (int)(L.cx - L.gap - pad), L.dots_y - pad, (int)(L.cx + L.gap + pad + 1), L.dots_y + pad + 1);
    for (int i = 0; i < 3; i++) {
        float ph = (float)(t / 1.2 * 2 * M_PI) - i * 0.9f;
        float s = 0.5f + 0.5f * sinf(ph);
        float a = 0.25f + 0.75f * s * s;
        draw_dot(c, L.cx + (i - 1) * L.gap, (float)L.dots_y, L.rad * (0.85f + 0.15f * s), 79, 140, 255, a);
    }
}

static unsigned char *load_font(void) {
    const char *fp = getenv("NOTASPLASH_FONT"); /* for --ppm tests on a PC */
    FILE *f = fopen(fp ? fp : FONT, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = n > 0 ? malloc(n) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

/* ---------- test: one frame to a PPM file ---------- */
static int write_ppm(const char *path, int w, int h, double t) {
    Canvas c = {calloc((size_t)w * h, 4), w, h, w};
    if (!c.px) return 1;
    unsigned char *font = load_font();
    if (!font) fprintf(stderr, "notasplash: no font at %s (dots only)\n", FONT);
    draw_static(&c, font);
    draw_dots(&c, t);
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        unsigned char p[3] = {c.px[i] >> 16 & 255, c.px[i] >> 8 & 255, c.px[i] & 255};
        fwrite(p, 1, 3, f);
    }
    fclose(f);
    return 0;
}

/* ---------- display ---------- */
typedef struct {
    int fd;
    uint32_t conn, crtc, fb, handle;
    drmModeModeInfo mode;
    void *map;
    size_t size;
    Canvas c;
} Disp;

/* a connected display on a card that can show something; uses the mode already on the screen (set by the
   kernel's console when the driver loaded) so the TV does not have to resync, and tvshell finds the same */
static int find_output(Disp *D, int fd) {
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) return 0;
    int ok = 0;
    for (int i = 0; i < res->count_connectors && !ok; i++) {
        drmModeConnector *cn = drmModeGetConnector(fd, res->connectors[i]);
        if (!cn) continue;
        if (cn->connection == DRM_MODE_CONNECTED && cn->count_modes > 0) {
            drmModeEncoder *e = cn->encoder_id ? drmModeGetEncoder(fd, cn->encoder_id) : NULL;
            drmModeCrtc *cr = e && e->crtc_id ? drmModeGetCrtc(fd, e->crtc_id) : NULL;
            if (cr && cr->mode_valid) {
                D->crtc = cr->crtc_id;
                D->mode = cr->mode;
            } else {
                /* nothing on the screen yet: preferred mode and the first CRTC this connector can use */
                D->mode = cn->modes[0];
                for (int m = 0; m < cn->count_modes; m++)
                    if (cn->modes[m].type & DRM_MODE_TYPE_PREFERRED) { D->mode = cn->modes[m]; break; }
                D->crtc = 0;
                for (int k = 0; k < cn->count_encoders && !D->crtc; k++) {
                    drmModeEncoder *pe = drmModeGetEncoder(fd, cn->encoders[k]);
                    for (int j = 0; pe && j < res->count_crtcs; j++)
                        if (pe->possible_crtcs & (1u << j)) { D->crtc = res->crtcs[j]; break; }
                    drmModeFreeEncoder(pe);
                }
            }
            if (D->crtc) { D->conn = cn->connector_id; ok = 1; }
            drmModeFreeCrtc(cr);
            drmModeFreeEncoder(e);
        }
        drmModeFreeConnector(cn);
    }
    drmModeFreeResources(res);
    return ok;
}

static int open_display(Disp *D) {
    char path[32];
    for (int n = 0; n < 8; n++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", n);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        uint64_t dumb = 0;
        drmVersion *v = drmGetVersion(fd);
        /* skip the firmware's simple framebuffer (it goes away when vc4 takes over) */
        int simple = v && v->name && strstr(v->name, "simple");
        drmFreeVersion(v);
        if (!simple && !drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &dumb) && dumb && find_output(D, fd)) {
            D->fd = fd;
            return 1;
        }
        close(fd);
    }
    return 0;
}

static int setup_fb(Disp *D) {
    struct drm_mode_create_dumb cr = {.width = D->mode.hdisplay, .height = D->mode.vdisplay, .bpp = 32};
    if (drmIoctl(D->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr)) return 0;
    D->handle = cr.handle;
    D->size = cr.size;
    uint32_t h[4] = {cr.handle}, p[4] = {cr.pitch}, o[4] = {0};
    if (drmModeAddFB2(D->fd, cr.width, cr.height, DRM_FORMAT_XRGB8888, h, p, o, &D->fb, 0)) return 0;
    struct drm_mode_map_dumb mp = {.handle = cr.handle};
    if (drmIoctl(D->fd, DRM_IOCTL_MODE_MAP_DUMB, &mp)) return 0;
    D->map = mmap(NULL, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, D->fd, mp.offset);
    if (D->map == MAP_FAILED) { D->map = NULL; return 0; }
    D->c = (Canvas){D->map, cr.width, cr.height, cr.pitch / 4};
    return 1;
}

static void set_kd_graphics(void) {
    int fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    ioctl(fd, KDSETMODE, KD_GRAPHICS); /* the kernel console must not draw on top (tvshell keeps it like this) */
    close(fd);
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--ppm")) {
        int w = 1920, h = 1080;
        sscanf(argv[3], "%dx%d", &w, &h);
        return write_ppm(argv[2], w, h, argc > 4 ? atof(argv[4]) : 0.3);
    }
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, SIG_IGN);

    /* the pid file first: tvshell may already want the screen while we wait for the display */
    FILE *pf = fopen(PIDFILE, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }

    Disp D = {.fd = -1};
    double t0 = now_s();
    /* the display driver is loading in parallel, and the TV may answer slowly: try for up to 10 s */
    while (!quit && !open_display(&D) && now_s() - t0 < 10) usleep(50000);
    if (quit || D.fd < 0) goto out;
    if (!setup_fb(&D)) goto out;

    unsigned char *font = load_font();
    draw_static(&D.c, font);
    free(font);
    draw_dots(&D.c, 0);
    if (drmModeSetCrtc(D.fd, D.crtc, D.fb, 0, 0, &D.conn, 1, &D.mode)) {
        fprintf(stderr, "notasplash: setcrtc: %s\n", strerror(errno));
        goto out;
    }
    set_kd_graphics();
    double start = now_s();
    while (!quit && now_s() - start < MAX_SECONDS) {
        draw_dots(&D.c, now_s() - start);
        drmModeDirtyFB(D.fd, D.fb, NULL, 0);
        usleep(33000); /* ~30 fps is plenty for three dots */
    }
out:
    /* close the device BEFORE removing the pid file: the kernel puts its console mode back when the last
       user closes it, and tvshell must find that, not our buffer */
    if (D.map) munmap(D.map, D.size);
#ifdef DRM_IOCTL_MODE_CLOSEFB
    /* let go of the buffer without turning the screen off (kernel 6.8+); otherwise closing removes it */
    if (D.fb) {
        struct drm_mode_closefb cf = {.fb_id = D.fb};
        drmIoctl(D.fd, DRM_IOCTL_MODE_CLOSEFB, &cf);
    }
#endif
    if (D.fd >= 0) close(D.fd);
    unlink(PIDFILE);
    return 0;
}
