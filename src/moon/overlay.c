/* tvmoon - stats overlay: text on an overlay plane on top of the video (Green toggles).
 * Drawn by the CPU in a small ARGB buffer (two of them, swapped on each update) and sent along in
 * the video's atomic commits, so it never delays a frame. */
#define _GNU_SOURCE
#include "tvmoon.h"
#include <drm_fourcc.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "vendor/stb_truetype.h"
#pragma GCC diagnostic pop

#define FONT "/usr/share/tvshell/Inter-SemiBold.ttf"

typedef struct { uint32_t handle, fb, pitch; uint8_t *map; size_t size; } DumbBuf;

static struct {
    int fd;
    uint32_t plane, crtc;
    uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch;
    int w, h, x, y;
    DumbBuf buf[2];
    int front;             /* the buffer being shown / to be shown */
    bool want, shown;      /* wanted visible / last sent visible */
    bool dirty;            /* a new buffer or changed visibility must go into the next commit */
    pthread_mutex_t mu;
    unsigned char *ttf;
    stbtt_fontinfo font;
    bool font_ok;
} O = {.fd = -1, .mu = PTHREAD_MUTEX_INITIALIZER};

volatile bool stats_toggle;

static uint32_t pid(uint32_t obj, const char *name, uint64_t *range_max) {
    drmModeObjectProperties *pr = drmModeObjectGetProperties(O.fd, obj, DRM_MODE_OBJECT_PLANE);
    uint32_t id = 0;
    for (uint32_t i = 0; pr && i < pr->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(O.fd, pr->props[i]);
        if (p && !strcmp(p->name, name)) {
            id = p->prop_id;
            if (range_max && p->count_values >= 2) *range_max = p->values[1];
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(pr);
    return id;
}

static bool make_dumb(DumbBuf *b) {
    struct drm_mode_create_dumb c = {.width = (uint32_t)O.w, .height = (uint32_t)O.h, .bpp = 32};
    if (drmIoctl(O.fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return false;
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;
    struct drm_mode_map_dumb m = {.handle = c.handle};
    if (drmIoctl(O.fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return false;
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, O.fd, (off_t)m.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; return false; }
    memset(b->map, 0, b->size);
    uint32_t h[4] = {b->handle}, p[4] = {b->pitch}, o[4] = {0};
    return drmModeAddFB2(O.fd, (uint32_t)O.w, (uint32_t)O.h, DRM_FORMAT_ARGB8888, h, p, o, &b->fb, 0) == 0;
}

void overlay_init(int fd, uint32_t crtc, int crtc_idx, int mode_w, int mode_h) {
    O.fd = fd;
    O.crtc = crtc;
    /* size follows the screen: 1080p -> 1000x420, 4K -> double */
    O.w = mode_w * 1000 / 1920;
    O.h = mode_h * 470 / 1080;
    O.x = mode_w * 40 / 1920;
    O.y = mode_h * 40 / 1080;
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    for (uint32_t i = 0; pr && i < pr->count_planes && !O.plane; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
        if (p && (p->possible_crtcs & (1u << crtc_idx)) && !p->fb_id) {
            O.plane = p->plane_id;
            uint64_t type = 0;
            uint32_t tp = pid(p->plane_id, "type", NULL);
            drmModeObjectProperties *op = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            for (uint32_t k = 0; op && k < op->count_props; k++) if (op->props[k] == tp) type = op->prop_values[k];
            drmModeFreeObjectProperties(op);
            if (type != DRM_PLANE_TYPE_OVERLAY) O.plane = 0;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    if (!O.plane) { tlog("overlay: intet ledigt overlay-plan"); return; }
    uint32_t pl = O.plane;
    O.p_fb = pid(pl, "FB_ID", NULL); O.p_crtc = pid(pl, "CRTC_ID", NULL);
    O.p_sx = pid(pl, "SRC_X", NULL); O.p_sy = pid(pl, "SRC_Y", NULL); O.p_sw = pid(pl, "SRC_W", NULL); O.p_sh = pid(pl, "SRC_H", NULL);
    O.p_cx = pid(pl, "CRTC_X", NULL); O.p_cy = pid(pl, "CRTC_Y", NULL); O.p_cw = pid(pl, "CRTC_W", NULL); O.p_ch = pid(pl, "CRTC_H", NULL);
    uint64_t zmax = 0;
    uint32_t pz = pid(pl, "zpos", &zmax);
    if (pz && zmax) drmModeObjectSetProperty(fd, pl, DRM_MODE_OBJECT_PLANE, pz, zmax); /* on top (vc4: overlay >= 1) */
    if (!make_dumb(&O.buf[0]) || !make_dumb(&O.buf[1])) { tlog("overlay: buffer fejlede"); O.plane = 0; return; }
    FILE *f = fopen(FONT, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        O.ttf = malloc((size_t)n);
        if (O.ttf && fread(O.ttf, 1, (size_t)n, f) == (size_t)n)
            O.font_ok = stbtt_InitFont(&O.font, O.ttf, stbtt_GetFontOffsetForIndex(O.ttf, 0)) != 0;
        fclose(f);
    }
    tlog("overlay: plan %u, %dx%d%s", O.plane, O.w, O.h, O.font_ok ? "" : " (ingen skrifttype!)");
}

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xf0) { c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); p += 3; }
    else if (c >= 0xe0) { c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u); p += 2; }
    else if (c >= 0xc0) { c = (c & 31) << 6 | (p[0] & 63u); p += 1; }
    *s = (const char *)p;
    return c;
}

/* draw text (premultiplied alpha) into buffer b from (x, y=baseline) */
static void draw_text(DumbBuf *b, int x, int y, float px, uint8_t lum, const char *s) {
    float sc = stbtt_ScaleForPixelHeight(&O.font, px);
    float pen = (float)x;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&O.font, (int)cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&O.font, (int)cp, sc, sc, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw < 512 && gh < 512) {
            static unsigned char g[512 * 512];
            stbtt_MakeCodepointBitmap(&O.font, g, gw, gh, gw, sc, sc, (int)cp);
            for (int j = 0; j < gh; j++) {
                int yy = y + y0 + j;
                if (yy < 0 || yy >= O.h) continue;
                uint32_t *row = (uint32_t *)(b->map + (size_t)yy * b->pitch);
                for (int i = 0; i < gw; i++) {
                    int xx = (int)pen + x0 + i;
                    if (xx < 0 || xx >= O.w || !g[j * gw + i]) continue;
                    uint32_t a = g[j * gw + i], d = row[xx];
                    uint32_t da = d >> 24, dc = d & 0xff;
                    uint32_t oa = a + da * (255 - a) / 255, oc = lum * a / 255 + dc * (255 - a) / 255;
                    row[xx] = oa << 24 | oc << 16 | oc << 8 | oc;
                }
            }
        }
        pen += adv * sc;
    }
}

void overlay_set_text(const char *const *lines, int n, int highlight, bool hdr) {
    if (!O.plane || !O.font_ok) return;
    pthread_mutex_lock(&O.mu);
    DumbBuf *b = &O.buf[!O.front];
    pthread_mutex_unlock(&O.mu);
    /* semi-transparent black background (premultiplied: color 0) */
    for (int y = 0; y < O.h; y++) {
        uint32_t *row = (uint32_t *)(b->map + (size_t)y * b->pitch);
        for (int x = 0; x < O.w; x++) row[x] = 0xC0u << 24;
    }
    float px = O.h / 9.4f;
    /* in HDR (PQ) full white is far too bright: ~200 nits */
    uint8_t lum = hdr ? 150 : 255;
    if (highlight >= 0 && highlight < n) { /* highlighted line: a lighter bar behind the text */
        int y0 = (int)(px * (1.25f + highlight * 1.22f - 0.95f)), y1 = y0 + (int)(px * 1.22f);
        if (y0 < 0) y0 = 0;
        if (y1 > O.h) y1 = O.h;
        uint32_t g = hdr ? 0x38 : 0x50; /* premultiplied gray */
        for (int y = y0; y < y1; y++) {
            uint32_t *row = (uint32_t *)(b->map + (size_t)y * b->pitch);
            for (int x = (int)(px * 0.3f); x < O.w - (int)(px * 0.3f); x++) row[x] = 0xF0u << 24 | g << 16 | g << 8 | g;
        }
    }
    for (int i = 0; i < n; i++) draw_text(b, (int)(px * 0.6f), (int)(px * (1.25f + i * 1.22f)), px, i == 0 ? lum : (uint8_t)(lum * 0.92f), lines[i]);
    pthread_mutex_lock(&O.mu);
    O.front = !O.front;
    O.dirty = true;
    pthread_mutex_unlock(&O.mu);
}

bool overlay_visible(void) { return O.want; }

void overlay_show(bool on) {
    pthread_mutex_lock(&O.mu);
    if (O.want != on) O.dirty = true;
    O.want = on;
    pthread_mutex_unlock(&O.mu);
}

bool overlay_dirty(void) { return O.plane && O.dirty; }

/* add the overlay plane to an atomic commit (called from the display thread) */
void overlay_commit(void *req) {
    drmModeAtomicReq *rq = req;
    if (!O.plane) return;
    pthread_mutex_lock(&O.mu);
    if (O.dirty || O.want != O.shown) {
        if (O.want) {
            drmModeAtomicAddProperty(rq, O.plane, O.p_fb, O.buf[O.front].fb);
            drmModeAtomicAddProperty(rq, O.plane, O.p_crtc, O.crtc);
            drmModeAtomicAddProperty(rq, O.plane, O.p_sx, 0);
            drmModeAtomicAddProperty(rq, O.plane, O.p_sy, 0);
            drmModeAtomicAddProperty(rq, O.plane, O.p_sw, (uint64_t)O.w << 16);
            drmModeAtomicAddProperty(rq, O.plane, O.p_sh, (uint64_t)O.h << 16);
            drmModeAtomicAddProperty(rq, O.plane, O.p_cx, O.x);
            drmModeAtomicAddProperty(rq, O.plane, O.p_cy, O.y);
            drmModeAtomicAddProperty(rq, O.plane, O.p_cw, O.w);
            drmModeAtomicAddProperty(rq, O.plane, O.p_ch, O.h);
        } else {
            drmModeAtomicAddProperty(rq, O.plane, O.p_fb, 0);
            drmModeAtomicAddProperty(rq, O.plane, O.p_crtc, 0);
        }
        O.shown = O.want;
        O.dirty = false;
    }
    pthread_mutex_unlock(&O.mu);
}

void overlay_quit(void) {
    for (int i = 0; i < 2; i++) {
        DumbBuf *b = &O.buf[i];
        if (b->fb) drmModeRmFB(O.fd, b->fb);
        if (b->map) munmap(b->map, b->size);
        if (b->handle) drmIoctl(O.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){.handle = b->handle});
        memset(b, 0, sizeof *b);
    }
    free(O.ttf);
    O.ttf = NULL;
    O.plane = 0;
}
