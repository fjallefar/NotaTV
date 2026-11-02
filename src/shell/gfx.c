/* NotaTV shell - graphics: primitives, text (stb_truetype atlas), images, icons */
#include "tv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <time.h>

#include "vendor/stb_image.h"
#include "vendor/stb_truetype.h"

const Color C_BG = {10, 12, 17, 255};
const Color C_BG2 = {20, 24, 33, 255};
const Color C_SURFACE = {28, 32, 42, 255};
const Color C_SURFACE2 = {40, 46, 60, 255};
const Color C_TEXT = {242, 244, 248, 255};
const Color C_TEXT2 = {160, 168, 184, 255};
const Color C_TEXT3 = {105, 112, 128, 255};
const Color C_ACCENT = {79, 140, 255, 255};
const Color C_FOCUS = {255, 255, 255, 255};
const Color C_DANGER = {235, 87, 87, 255};
const Color C_OK = {72, 199, 116, 255};

Color color_alpha(Color c, float a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    c.a = (uint8_t)(c.a * a);
    return c;
}

Color color_mix(Color a, Color b, float t) {
    Color c;
    c.r = (uint8_t)(a.r + (b.r - a.r) * t);
    c.g = (uint8_t)(a.g + (b.g - a.g) * t);
    c.b = (uint8_t)(a.b + (b.b - a.b) * t);
    c.a = (uint8_t)(a.a + (b.a - a.a) * t);
    return c;
}

static SDL_Renderer *R;
/* debugging (shown together with FPS): draw calls and new textures since last time */
static int st_geom, st_tex_new, st_imgs;
void gfx_stats(int *geom, int *tex_new, int *imgs) {
    *geom = st_geom; *tex_new = st_tex_new; *imgs = st_imgs;
    st_geom = st_tex_new = st_imgs = 0;
}
static float g_scale = 1.0f; /* physical pixels per logical pixel */
static int g_generation = 1;

SDL_Renderer *gfx_renderer(void) { return R; }
float gfx_scale(void) { return g_scale; }

/* ---------- HDR mode: the UI is drawn directly in HDR10 (PQ, BT.2020) ----------
   When the TV is always in HDR, all SDR colors are converted: sRGB -> linear -> BT.709->BT.2020 ->
   PQ, with SDR white at 203 nits (ITU-R BT.2408). Alpha blending then happens in PQ space. */
static bool g_hdr;
static float hdr_lin[256];          /* sRGB code -> linear 0..1 */
static uint8_t hdr_pq[4096];        /* linear (relative to SDR white) -> PQ code */
#define HDR_SDR_WHITE 203.0

static void hdr_init_lut(void) {
    if (hdr_pq[4095]) return;
    for (int i = 0; i < 256; i++) {
        double v = i / 255.0;
        hdr_lin[i] = (float)(v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
    }
    const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    for (int i = 0; i < 4096; i++) {
        double l = (i / 4095.0) * HDR_SDR_WHITE / 10000.0, p = pow(l, m1);
        hdr_pq[i] = (uint8_t)lround(pow((c1 + c2 * p) / (1 + c3 * p), m2) * 255.0);
    }
}

bool gfx_get_hdr(void) { return g_hdr; }

void gfx_set_hdr(bool on) {
    g_hdr = on;
    if (on) hdr_init_lut();
}

static inline uint8_t hdr_enc(float l) {
    if (l <= 0) return 0;
    if (l >= 1) return hdr_pq[4095];
    return hdr_pq[(int)(l * 4095.0f + 0.5f)];
}

static inline void hdr_rgb(uint8_t *r, uint8_t *g, uint8_t *b) {
    float R0 = hdr_lin[*r], G0 = hdr_lin[*g], B0 = hdr_lin[*b];
    *r = hdr_enc(0.6274f * R0 + 0.3293f * G0 + 0.0433f * B0);
    *g = hdr_enc(0.0691f * R0 + 0.9195f * G0 + 0.0114f * B0);
    *b = hdr_enc(0.0164f * R0 + 0.0880f * G0 + 0.8956f * B0);
}

static unsigned char *hdr_copy(const unsigned char *px, int w, int h) {
    size_t n = (size_t)w * h;
    unsigned char *hp = malloc(n * 4);
    if (!hp) return NULL;
    memcpy(hp, px, n * 4);
    for (size_t i = 0; i < n; i++) hdr_rgb(&hp[i * 4], &hp[i * 4 + 1], &hp[i * 4 + 2]);
    return hp;
}

static inline Color out_col(Color c) {
    if (g_hdr) hdr_rgb(&c.r, &c.g, &c.b);
    return c;
}

static inline SDL_FColor fcol(Color c) {
    c = out_col(c);
    return (SDL_FColor){c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
}

static inline float snap(float v) { return roundf(v * g_scale) / g_scale; }

/* ---------- Primitiver ---------- */
void fill_rect(float x, float y, float w, float h, Color c) {
    if (c.a == 0 || w <= 0 || h <= 0) return;
    c = out_col(c);
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_FRect rc = {x, y, w, h};
    SDL_RenderFillRect(R, &rc);
}

#define MAXV 512
static SDL_Vertex vbuf[MAXV];
static int ibuf[MAXV * 3];

static int seg_for(float r) { return r < 4 ? 1 : (r < 16 ? 5 : (r < 40 ? 8 : 12)); }

static int rrect_outline_seg(float x, float y, float w, float h, float r, int seg, float *px, float *py, int maxp) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    int n = 0;
    float cx[4] = {x + w - r, x + r, x + r, x + w - r};
    float cy[4] = {y + r, y + r, y + h - r, y + h - r};
    float a0[4] = {-M_PI / 2, -M_PI, M_PI / 2, 0};
    /* order: top-right, top-left, bottom-left, bottom-right (counterclockwise) */
    float start[4] = {0, -M_PI / 2, -M_PI, M_PI / 2};
    (void)a0;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i <= seg && n < maxp; i++) {
            float a = start[k] - (float)i / seg * (M_PI / 2);
            px[n] = cx[k] + cosf(a) * r;
            py[n] = cy[k] + sinf(a) * r;
            n++;
        }
    }
    return n;
}

/* Rounded rectangle as a mesh: a center patch, four edge patches and four small corner fans. A fan of
   long, thin triangles from the center (as before) wastes a lot on the Pi's GPU, which shades pixels in
   2x2 blocks - here almost every pixel is computed only once. Gives positions + indices. */
static int rrect_mesh(float x, float y, float w, float h, float r, float *vx, float *vy, int *idx, int *ni_out) {
    float rr = r;
    if (rr > w / 2) rr = w / 2;
    if (rr > h / 2) rr = h / 2;
    if (rr < 0) rr = 0;
    int seg = seg_for(rr);
    /* 0-3: corner centers (top-right, top-left, bottom-left, bottom-right) */
    float cx[4] = {x + w - rr, x + rr, x + rr, x + w - rr};
    float cy[4] = {y + rr, y + rr, y + h - rr, y + h - rr};
    for (int k = 0; k < 4; k++) { vx[k] = cx[k]; vy[k] = cy[k]; }
    int nv = 4, ni = 0;
    /* centerfelt */
    idx[ni++] = 0; idx[ni++] = 1; idx[ni++] = 2;
    idx[ni++] = 0; idx[ni++] = 2; idx[ni++] = 3;
    float start[4] = {0, -M_PI / 2, -M_PI, M_PI / 2};
    int first[4], last[4];
    for (int k = 0; k < 4; k++) {
        first[k] = nv;
        for (int i = 0; i <= seg; i++) {
            float a = start[k] - (float)i / seg * (M_PI / 2);
            vx[nv] = cx[k] + cosf(a) * rr;
            vy[nv] = cy[k] + sinf(a) * rr;
            if (i > 0) { idx[ni++] = k; idx[ni++] = nv - 1; idx[ni++] = nv; }
            nv++;
        }
        last[k] = nv - 1;
    }
    /* edge patches between the corner arcs */
    for (int k = 0; k < 4; k++) {
        int k2 = (k + 1) % 4;
        idx[ni++] = last[k]; idx[ni++] = first[k2]; idx[ni++] = k2;
        idx[ni++] = last[k]; idx[ni++] = k2; idx[ni++] = k;
    }
    *ni_out = ni;
    return nv;
}

void fill_rrect(float x, float y, float w, float h, float r, Color c) {
    if (c.a == 0 || w <= 0 || h <= 0) return;
    if (r < 0.5f) { fill_rect(x, y, w, h, c); return; }
    float vx[64], vy[64];
    int ni, nv = rrect_mesh(x, y, w, h, r, vx, vy, ibuf, &ni);
    SDL_FColor fc = fcol(c);
    for (int i = 0; i < nv; i++) vbuf[i] = (SDL_Vertex){{vx[i], vy[i]}, fc, {0, 0}};
    st_geom++, SDL_RenderGeometry(R, NULL, vbuf, nv, ibuf, ni);
}

void fill_rrect_vgrad(float x, float y, float w, float h, float r, Color top, Color bot) {
    if (w <= 0 || h <= 0) return;
    float vx[64], vy[64];
    int ni, nv = rrect_mesh(x, y, w, h, r, vx, vy, ibuf, &ni);
    for (int i = 0; i < nv; i++)
        vbuf[i] = (SDL_Vertex){{vx[i], vy[i]}, fcol(color_mix(top, bot, (vy[i] - y) / h)), {0, 0}};
    st_geom++, SDL_RenderGeometry(R, NULL, vbuf, nv, ibuf, ni);
}

void stroke_rrect(float x, float y, float w, float h, float r, float t, Color c) {
    if (c.a == 0) return;
    float ox[128], oy[128], ix[128], iy[128];
    float rr = r;
    if (rr > w / 2) rr = w / 2;
    if (rr > h / 2) rr = h / 2;
    int seg = seg_for(rr);
    int n = rrect_outline_seg(x, y, w, h, r, seg, ox, oy, 120);
    int m = rrect_outline_seg(x + t, y + t, w - 2 * t, h - 2 * t, r - t > 0 ? r - t : 0, seg, ix, iy, 120);
    if (n != m) return;
    SDL_FColor fc = fcol(c);
    for (int i = 0; i < n; i++) {
        vbuf[2 * i] = (SDL_Vertex){{ox[i], oy[i]}, fc, {0, 0}};
        vbuf[2 * i + 1] = (SDL_Vertex){{ix[i], iy[i]}, fc, {0, 0}};
    }
    int ni = 0;
    for (int i = 0; i < n; i++) {
        int a = 2 * i, b = 2 * i + 1, cc = 2 * ((i + 1) % n), d = 2 * ((i + 1) % n) + 1;
        ibuf[ni++] = a; ibuf[ni++] = b; ibuf[ni++] = cc;
        ibuf[ni++] = b; ibuf[ni++] = d; ibuf[ni++] = cc;
    }
    st_geom++, SDL_RenderGeometry(R, NULL, vbuf, 2 * n, ibuf, ni);
}

/* Vertical gradient without banding: computed once as a texture with ordered dithering - also in
   HDR, where the conversion happens in floating point before rounding. For large, dark areas like the background. */
static SDL_Texture *dg_tex;
static int dg_gen, dg_w, dg_h;
static Color dg_top, dg_bot;
static bool dg_hdr;

static float to_out(float v) { /* sRGB code (0-255) -> output code (0-255, floating point) */
    if (!g_hdr) return v;
    double c = v / 255.0, l = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    double p = pow(l * HDR_SDR_WHITE / 10000.0, 0.1593017578125);
    return (float)(pow((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p), 78.84375) * 255.0);
}

void fill_vgrad_dither(float x, float y, float w, float h, Color top, Color bot) {
    int W = (int)(w * g_scale), H = (int)(h * g_scale);
    if (W <= 0 || H <= 0) return;
    if (!dg_tex || dg_gen != g_generation || dg_w != W || dg_h != H || dg_hdr != g_hdr ||
        memcmp(&dg_top, &top, sizeof top) || memcmp(&dg_bot, &bot, sizeof bot)) {
        if (dg_tex && dg_gen == g_generation) SDL_DestroyTexture(dg_tex);
        static const float bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        uint8_t *px = malloc((size_t)W * H * 4);
        if (!px) return;
        for (int yy = 0; yy < H; yy++) {
            float t = H > 1 ? (float)yy / (H - 1) : 0;
            /* BT.709->BT.2020 is ~identity for grays; the background is almost gray, so per channel is enough */
            float r = to_out(top.r + (bot.r - top.r) * t), g = to_out(top.g + (bot.g - top.g) * t),
                  b = to_out(top.b + (bot.b - top.b) * t);
            uint8_t *row = px + (size_t)yy * W * 4;
            for (int xx = 0; xx < W; xx++) {
                float d = (bayer[yy & 3][xx & 3] + 0.5f) / 16.0f - 0.5f;
                row[xx * 4 + 0] = (uint8_t)fminf(255, fmaxf(0, r + d + 0.5f));
                row[xx * 4 + 1] = (uint8_t)fminf(255, fmaxf(0, g + d + 0.5f));
                row[xx * 4 + 2] = (uint8_t)fminf(255, fmaxf(0, b + d + 0.5f));
                row[xx * 4 + 3] = 255;
            }
        }
        dg_tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, W, H);
        if (dg_tex) { SDL_UpdateTexture(dg_tex, NULL, px, W * 4); SDL_SetTextureScaleMode(dg_tex, SDL_SCALEMODE_NEAREST); }
        free(px);
        dg_gen = g_generation; dg_w = W; dg_h = H; dg_top = top; dg_bot = bot; dg_hdr = g_hdr;
    }
    if (!dg_tex) { fill_vgrad(x, y, w, h, top, bot); return; }
    SDL_FRect dst = {x, y, w, h};
    SDL_RenderTexture(R, dg_tex, NULL, &dst);
}

void fill_vgrad(float x, float y, float w, float h, Color top, Color bot) {
    SDL_FColor a = fcol(top), b = fcol(bot);
    SDL_Vertex v[4] = {{{x, y}, a, {0, 0}}, {{x + w, y}, a, {0, 0}},
                       {{x + w, y + h}, b, {0, 0}}, {{x, y + h}, b, {0, 0}}};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    st_geom++, SDL_RenderGeometry(R, NULL, v, 4, idx, 6);
}

void fill_hgrad(float x, float y, float w, float h, Color l, Color r) {
    SDL_FColor a = fcol(l), b = fcol(r);
    SDL_Vertex v[4] = {{{x, y}, a, {0, 0}}, {{x + w, y}, b, {0, 0}},
                       {{x + w, y + h}, b, {0, 0}}, {{x, y + h}, a, {0, 0}}};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    st_geom++, SDL_RenderGeometry(R, NULL, v, 4, idx, 6);
}

void fill_circle(float cx, float cy, float rad, Color c) {
    fill_rrect(cx - rad, cy - rad, rad * 2, rad * 2, rad, c);
}

void fill_tri(float x1, float y1, float x2, float y2, float x3, float y3, Color c) {
    SDL_FColor f = fcol(c);
    SDL_Vertex v[3] = {{{x1, y1}, f, {0, 0}}, {{x2, y2}, f, {0, 0}}, {{x3, y3}, f, {0, 0}}};
    st_geom++, SDL_RenderGeometry(R, NULL, v, 3, NULL, 0);
}

/* a soft shadow as one texture per (radius, spread), drawn as a 9-grid: one draw call instead of 6 overlapping
   rounded rectangles over the whole tile. The falloff matches the old 6 layers (about 75 % black at the edge). */
typedef struct { int r, sp, gen; bool white; SDL_Texture *t; } ShadowTex;
static ShadowTex shadow_cache[8];
/* white = a white glow (tinted with a color mod) instead of a black shadow */
static SDL_Texture *shadow_tex_ex(int r, int sp, bool white) {
    for (int i = 0; i < 8; i++)
        if (shadow_cache[i].t && shadow_cache[i].r == r && shadow_cache[i].sp == sp && shadow_cache[i].white == white &&
            shadow_cache[i].gen == g_generation)
            return shadow_cache[i].t;
    int c = r + sp, n = 2 * c + 2;
    uint32_t *px = malloc((size_t)n * n * 4);
    if (!px) return NULL;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            /* distance from the rounded rectangle inset by sp (its corners have radius r) */
            float cx = x + 0.5f, cy = y + 0.5f;
            float qx = fabsf(cx - n / 2.0f) - (n / 2.0f - sp - r), qy = fabsf(cy - n / 2.0f) - (n / 2.0f - sp - r);
            float d = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
            float t = d <= 0 ? 0 : d >= sp ? 1 : d / sp, a = 0.75f * (1 - t) * (1 - t);
            px[y * n + x] = (uint32_t)(a * 255 + 0.5f) << 24 | (white ? 0xffffffu : 0); /* black/white, alpha */
        }
    SDL_Texture *t = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, n, n);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, n * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    free(px);
    int k = 0;
    for (int i = 0; i < 8; i++) if (!shadow_cache[i].t || shadow_cache[i].gen != g_generation) { k = i; break; }
    if (shadow_cache[k].t && shadow_cache[k].gen == g_generation) SDL_DestroyTexture(shadow_cache[k].t);
    shadow_cache[k] = (ShadowTex){r, sp, g_generation, white, t};
    return t;
}
static SDL_Texture *shadow_tex(int r, int sp) { return shadow_tex_ex(r, sp, false); }

/* a soft colored glow around a rounded rectangle (centered, not dropped like the shadow) */
void draw_glow(float x, float y, float w, float h, float r, float spread, Color c, float alpha) {
    int ri = (int)roundf(r), si = (int)roundf(spread);
    SDL_Texture *t = si > 0 && w >= 2 * r && h >= 2 * r && alpha > 0.004f ? shadow_tex_ex(ri, si, true) : NULL;
    if (!t) return;
    float cc = (float)(ri + si);
    SDL_FRect d = {x - si, y - si + spread * 0.2f, w + 2 * si, h + 2 * si};
    Color o = out_col(c);
    SDL_SetTextureColorMod(t, o.r, o.g, o.b);
    SDL_SetTextureAlphaModFloat(t, alpha > 1 ? 1 : alpha);
    st_geom++;
    SDL_RenderTexture9Grid(R, t, NULL, cc, cc, cc, cc, 1.0f, &d);
}

void draw_shadow(float x, float y, float w, float h, float r, float spread, float alpha) {
    int ri = (int)roundf(r), si = (int)roundf(spread);
    SDL_Texture *t = si > 0 && w >= 2 * r && h >= 2 * r ? shadow_tex(ri, si) : NULL;
    if (t) {
        float c = (float)(ri + si);
        SDL_FRect d = {x - si, y - si + spread * 0.35f, w + 2 * si, h + 2 * si};
        SDL_SetTextureAlphaModFloat(t, alpha);
        st_geom++;
        SDL_RenderTexture9Grid(R, t, NULL, c, c, c, c, 1.0f, &d);
        return;
    }
    const int layers = 6;
    for (int i = layers; i >= 1; i--) {
        float e = spread * i / layers;
        float a = alpha * (1.0f - (float)(i - 1) / layers) * 0.35f;
        fill_rrect(x - e, y - e + spread * 0.35f, w + 2 * e, h + 2 * e, r + e, RGBA(0, 0, 0, (uint8_t)(a * 255)));
    }
}

/* ---------- Clipping (stak) ---------- */
static SDL_Rect clip_stack[16];
static int clip_n;
void clip_push(float x, float y, float w, float h) {
    SDL_Rect rc = {(int)x, (int)y, (int)ceilf(w), (int)ceilf(h)};
    if (clip_n > 0) {
        SDL_Rect out;
        if (!SDL_GetRectIntersection(&rc, &clip_stack[clip_n - 1], &out)) out = (SDL_Rect){0, 0, 0, 0};
        rc = out;
    }
    if (clip_n < 16) clip_stack[clip_n++] = rc;
    SDL_SetRenderClipRect(R, &rc);
}
void clip_pop(void) {
    if (clip_n > 0) clip_n--;
    SDL_SetRenderClipRect(R, clip_n ? &clip_stack[clip_n - 1] : NULL);
}

/* ---------- Layers: something that rarely changes is drawn once into a texture ----------
   (e.g. a background with gradients on top - drawing it every frame costs too much fill on the Pi's GPU) */
struct Layer { SDL_Texture *tex, *prev; int w, h, gen; bool opaque; };

Layer *layer_new(int w, int h) {
    Layer *l = calloc(1, sizeof *l);
    l->w = w; l->h = h;
    return l;
}
bool layer_valid(const Layer *l) { return l && l->tex && l->gen == g_generation; }
bool layer_begin(Layer *l) {
    if (!R || !l) return false;
    if (!layer_valid(l)) {
        l->tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, l->w, l->h);
        if (!l->tex) return false;
        SDL_SetTextureBlendMode(l->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(l->tex, SDL_SCALEMODE_LINEAR);
        l->gen = g_generation;
    }
    l->prev = SDL_GetRenderTarget(R);
    SDL_SetRenderTarget(R, l->tex);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 0);
    SDL_RenderClear(R);
    return true;
}
void layer_end(Layer *l) { SDL_SetRenderTarget(R, l->prev); }
void layer_set_opaque(Layer *l, bool on) { if (l) l->opaque = on; }
void layer_draw(Layer *l, float x, float y, float alpha) {
    if (!layer_valid(l)) return;
    /* fully opaque layer: no blending (the GPU does not have to read what is underneath) */
    SDL_SetTextureBlendMode(l->tex, l->opaque && alpha >= 1 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaModFloat(l->tex, alpha);
    SDL_FRect d = {x, y, (float)l->w, (float)l->h};
    SDL_RenderTexture(R, l->tex, NULL, &d);
}
void layer_free(Layer *l) {
    if (!l) return;
    if (layer_valid(l)) SDL_DestroyTexture(l->tex);
    free(l);
}

/* ---------- Drift: OLED burn-in protection that glides ----------
   What is drawn between gfx_drift_begin and gfx_drift_end (screen coordinates, inside the area) goes into a texture,
   which is then drawn moved by (ox, oy) - fractions of a pixel too, with linear filtering. So it glides smoothly
   instead of jumping a whole pixel at a time. One texture (grows to the largest area asked for); not nested. */
static SDL_Texture *drift_tex, *drift_prev;
static int drift_w, drift_h, drift_gen;
static SDL_FRect drift_area; /* logical, whole pixels */
static bool drift_on;
bool gfx_drift_begin(float x, float y, float w, float h) {
    if (!R || drift_on) return false;
    float x0 = floorf(x), y0 = floorf(y);
    w = ceilf(x + w) - x0; h = ceilf(y + h) - y0;
    int W = (int)ceilf(w * g_scale), H = (int)ceilf(h * g_scale);
    if (W <= 0 || H <= 0) return false;
    if (drift_tex && (drift_gen != g_generation || W > drift_w || H > drift_h)) {
        if (drift_gen == g_generation) SDL_DestroyTexture(drift_tex);
        drift_tex = NULL;
    }
    if (!drift_tex) {
        if (drift_gen != g_generation) drift_w = drift_h = 0;
        if (W > drift_w) drift_w = W;
        if (H > drift_h) drift_h = H;
        drift_tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, drift_w, drift_h);
        if (!drift_tex) return false;
        /* the texture holds premultiplied color (drawing with alpha over transparent black) */
        SDL_SetTextureBlendMode(drift_tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(drift_tex, SDL_SCALEMODE_LINEAR);
        drift_gen = g_generation;
        st_tex_new++;
    }
    drift_prev = SDL_GetRenderTarget(R);
    SDL_SetRenderTarget(R, drift_tex);
    SDL_SetRenderViewport(R, NULL);
    SDL_SetRenderClipRect(R, NULL);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 0);
    SDL_RenderClear(R);
    SDL_SetRenderScale(R, g_scale, g_scale);
    /* screen coordinates: the area's corner lands at the texture's corner (the clip rectangles move along) */
    SDL_Rect vp = {(int)-x0, (int)-y0, UI_W, UI_H};
    SDL_SetRenderViewport(R, &vp);
    drift_area = (SDL_FRect){x0, y0, w, h};
    drift_on = true;
    return true;
}
void gfx_drift_end(float ox, float oy) {
    if (!drift_on) return;
    drift_on = false;
    SDL_SetRenderViewport(R, NULL);
    SDL_SetRenderTarget(R, drift_prev);
    SDL_FRect src = {0, 0, ceilf(drift_area.w * g_scale), ceilf(drift_area.h * g_scale)};
    SDL_FRect dst = {drift_area.x + ox, drift_area.y + oy, src.w / g_scale, src.h / g_scale};
    SDL_SetTextureAlphaModFloat(drift_tex, 1);
    SDL_RenderTexture(R, drift_tex, &src, &dst);
}

/* ---------- Text ---------- */
typedef struct { uint32_t cp; int16_t x, y, w, h, xoff, yoff; float adv; int glyph; } Glyph;
#define ATLAS_SZ 1024      /* texture size for big text; small sizes (<= ATLAS_SMALL_PX) get 512 x 512 */
#define ATLAS_SMALL_PX 30
#define GL_HASH 1024
#define KERN_HASH 2048     /* kerning pairs looked up once (Inter's GPOS kerning is slow in stb_truetype) */
typedef struct { int a, b; float k; } Kern;
typedef struct {
    int px; FontId font; int gen, sz;
    SDL_Texture *tex;
    int pen_x, pen_y, row_h;
    unsigned long last_used; /* atlas_tick when last asked for: the least recently used is thrown out */
    Glyph glyphs[GL_HASH];
    bool used[GL_HASH];
    Kern kern[KERN_HASH];
    bool kused[KERN_HASH];
    float scale, ascent, lineh;
} Atlas;
static unsigned long atlas_tick;

static stbtt_fontinfo fonts[F_COUNT];
static unsigned char *font_data[F_COUNT];
#define MAX_ATLAS 24
static Atlas *atlases[MAX_ATLAS];
static int n_atlas;

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    unsigned char *b = n >= 0 && fseek(f, 0, SEEK_SET) == 0 ? malloc((size_t)n + 1) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

const char *asset_dir(void) {
    const char *e = getenv("TVSHELL_ASSETS");
    return e ? e : "/usr/share/tvshell";
}

static bool load_font(FontId id, const char *file) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", asset_dir(), file);
    font_data[id] = read_file(path, NULL);
    if (!font_data[id]) {
        SDL_Log("font mangler: %s", path);
        return false;
    }
    return stbtt_InitFont(&fonts[id], font_data[id], stbtt_GetFontOffsetForIndex(font_data[id], 0));
}

static Atlas *atlas_get(FontId f, float size) {
    /* body text (<= 32) a bit bigger than drawn - on the TV at a distance it was on the small side */
    if (size <= 32) size *= 1.06f;
    int px = (int)roundf(size * g_scale);
    if (px < 6) px = 6;
    atlas_tick++;
    for (int i = 0; i < n_atlas; i++) {
        Atlas *a = atlases[i];
        if (a->font == f && a->px == px) {
            a->last_used = atlas_tick;
            if (a->gen != g_generation) { /* renderer genskabt */
                a->tex = NULL;
                memset(a->used, 0, sizeof a->used);
                a->pen_x = a->pen_y = a->row_h = 0;
                a->gen = g_generation;
            }
            return a;
        }
    }
    if (n_atlas == MAX_ATLAS) { /* throw out the least recently used (not the oldest: that may be on screen) */
        int k = 0;
        for (int i = 1; i < n_atlas; i++) if (atlases[i]->last_used < atlases[k]->last_used) k = i;
        if (atlases[k]->tex && atlases[k]->gen == g_generation) SDL_DestroyTexture(atlases[k]->tex);
        free(atlases[k]);
        memmove(atlases + k, atlases + k + 1, sizeof(Atlas *) * (size_t)(n_atlas - 1 - k));
        n_atlas--;
    }
    Atlas *a = calloc(1, sizeof(Atlas));
    if (!a) return atlases[0];
    a->px = px; a->font = f; a->gen = g_generation; a->last_used = atlas_tick;
    a->sz = px <= ATLAS_SMALL_PX ? 512 : ATLAS_SZ;
    a->scale = stbtt_ScaleForPixelHeight(&fonts[f], (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&fonts[f], &asc, &desc, &gap);
    a->ascent = asc * a->scale;
    a->lineh = (asc - desc + gap) * a->scale;
    atlases[n_atlas++] = a;
    return a;
}

static Glyph *glyph_get(Atlas *a, uint32_t cp) {
    unsigned h = (cp * 2654435761u) & (GL_HASH - 1);
    for (int probe = 0; probe < GL_HASH; probe++) {
        unsigned i = (h + probe) & (GL_HASH - 1);
        if (a->used[i] && a->glyphs[i].cp == cp) return &a->glyphs[i];
        if (!a->used[i]) {
            stbtt_fontinfo *fi = &fonts[a->font];
            int gi = stbtt_FindGlyphIndex(fi, cp);
            bool skip = gi == 0 && cp != ' '; /* e.g. emoji: leave out instead of showing '?' */
            int adv, lsb, x0, y0, x1, y1;
            stbtt_GetGlyphHMetrics(fi, gi, &adv, &lsb);
            stbtt_GetGlyphBitmapBox(fi, gi, a->scale, a->scale, &x0, &y0, &x1, &y1);
            int w = x1 - x0, hh = y1 - y0;
            Glyph *g = &a->glyphs[i];
            memset(g, 0, sizeof *g);
            g->cp = cp; g->glyph = gi; g->adv = skip ? 0 : adv * a->scale;
            g->w = skip ? 0 : w; g->h = skip ? 0 : hh; g->xoff = x0; g->yoff = y0;
            if (skip) { a->used[i] = true; return g; }
            if (w > 0 && hh > 0) {
                if (!a->tex) {
                    a->tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, a->sz, a->sz);
                    SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(a->tex, SDL_SCALEMODE_LINEAR);
                    void *zero = calloc((size_t)a->sz * a->sz, 4);
                    if (zero) SDL_UpdateTexture(a->tex, NULL, zero, a->sz * 4);
                    free(zero);
                }
                if (a->pen_x + w + 2 > a->sz) { a->pen_x = 0; a->pen_y += a->row_h + 2; a->row_h = 0; }
                if (w + 2 > a->sz || hh + 2 > a->sz) { a->used[i] = true; g->w = g->h = 0; return g; } /* larger than the atlas */
                if (a->pen_y + hh + 2 > a->sz) {
                    /* atlas full: reset (rare) */
                    memset(a->used, 0, sizeof a->used);
                    a->pen_x = a->pen_y = a->row_h = 0;
                    return glyph_get(a, cp);
                }
                unsigned char *mono = malloc(w * hh);
                stbtt_MakeGlyphBitmap(fi, mono, w, hh, w, a->scale, a->scale, gi);
                uint32_t *rgba = malloc(w * hh * 4);
                for (int k = 0; k < w * hh; k++) rgba[k] = 0x00FFFFFFu | ((uint32_t)mono[k] << 24);
                SDL_Rect dst = {a->pen_x, a->pen_y, w, hh};
                SDL_UpdateTexture(a->tex, &dst, rgba, w * 4);
                free(mono); free(rgba);
                g->x = a->pen_x; g->y = a->pen_y;
                a->pen_x += w + 2;
                if (hh > a->row_h) a->row_h = hh;
            }
            a->used[i] = true;
            return g;
        }
    }
    return NULL;
}

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s;
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xE0) == 0xC0 && s[1]) { *p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) { *p += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *p += 4;
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *p += 1;
    return '?';
}

/* kerning between two glyphs, in atlas pixels (looked up in the font once per pair) */
static float kern_get(Atlas *a, int g1, int g2) {
    unsigned h = ((unsigned)g1 * 2654435761u ^ (unsigned)g2 * 40503u) & (KERN_HASH - 1);
    for (int probe = 0; probe < 8; probe++) {
        unsigned i = (h + probe) & (KERN_HASH - 1);
        if (!a->kused[i]) {
            a->kused[i] = true;
            a->kern[i] = (Kern){g1, g2, stbtt_GetGlyphKernAdvance(&fonts[a->font], g1, g2) * a->scale};
            return a->kern[i].k;
        }
        if (a->kern[i].a == g1 && a->kern[i].b == g2) return a->kern[i].k;
    }
    return stbtt_GetGlyphKernAdvance(&fonts[a->font], g1, g2) * a->scale; /* full around here: not kept */
}

/* the width one more character adds (with the kerning to the one before); *prev = the glyph before, 0 at the start */
static float advance_of(Atlas *a, int *prev, uint32_t cp) {
    Glyph *g = glyph_get(a, cp);
    if (!g) return 0;
    float w = (*prev ? kern_get(a, *prev, g->glyph) : 0) + g->adv;
    *prev = g->glyph;
    return w;
}

static float measure_n(Atlas *a, const char *s, const char *end) {
    float w = 0;
    int prev = 0;
    while (*s && (!end || s < end)) w += advance_of(a, &prev, utf8_next(&s));
    return w / g_scale;
}

float text_width(FontId f, float size, const char *s) {
    if (!s) return 0;
    return measure_n(atlas_get(f, size), s, NULL);
}

static void draw_run(Atlas *a, float x, float y, Color c, const char *s, const char *end) {
    /* y = top of the line; we place the baseline at ascent */
    float pen = x * g_scale;
    float base = roundf(y * g_scale + a->ascent);
    int prev = 0;
    c = out_col(c);
    SDL_Texture *colored = NULL; /* the color is set once (again only if the first glyph made the texture) */
    while (*s && (!end || s < end)) {
        uint32_t cp = utf8_next(&s);
        Glyph *g = glyph_get(a, cp);
        if (!g) continue;
        if (prev) pen += kern_get(a, prev, g->glyph);
        if (g->w > 0 && a->tex) {
            if (colored != a->tex) {
                SDL_SetTextureColorMod(a->tex, c.r, c.g, c.b);
                SDL_SetTextureAlphaMod(a->tex, c.a);
                colored = a->tex;
            }
            SDL_FRect src = {g->x, g->y, g->w, g->h};
            SDL_FRect dst = {roundf(pen + g->xoff) / g_scale, (base + g->yoff) / g_scale, g->w / g_scale, g->h / g_scale};
            SDL_RenderTexture(R, a->tex, &src, &dst);
        }
        pen += g->adv;
        prev = g->glyph;
    }
}

float text_draw(FontId f, float size, float x, float y, Color c, Align al, const char *s) {
    if (!s || !*s) return 0;
    Atlas *a = atlas_get(f, size);
    float w = measure_n(a, s, NULL);
    if (al == AL_CENTER) x -= w / 2;
    else if (al == AL_RIGHT) x -= w;
    draw_run(a, snap(x), snap(y), c, s, NULL);
    return w;
}

float text_draw_clip(FontId f, float size, float x, float y, float maxw, Color c, Align al, const char *s) {
    if (!s || !*s) return 0;
    Atlas *a = atlas_get(f, size);
    float w = measure_n(a, s, NULL);
    if (w <= maxw) return text_draw(f, size, x, y, c, al, s);
    float ell = measure_n(a, "…", NULL), run = 0; /* measured as we go (not from the start for every character) */
    int prev = 0;
    const char *p = s, *best = s;
    while (*p) {
        const char *q = p;
        run += advance_of(a, &prev, utf8_next(&q)) / g_scale;
        if (run + ell > maxw) break;
        best = q;
        p = q;
    }
    size_t n = best - s;
    char *tmp = malloc(n + 4);
    memcpy(tmp, s, n);
    memcpy(tmp + n, "…", 4);
    float r = text_draw(f, size, x, y, c, al, tmp);
    free(tmp);
    return r;
}

int text_draw_wrap(FontId f, float size, float x, float y, float w, int maxlines, float lh,
                   Color c, const char *s) {
    if (!s) return 0;
    Atlas *a = atlas_get(f, size);
    int lines = 0;
    const char *p = s;
    while (*p && lines < maxlines) {
        while (*p == ' ') p++;
        const char *line = p, *brk = NULL, *q = p;
        float run = 0; /* the line's width so far, added up character by character */
        int prev = 0;
        while (*q && *q != '\n') {
            const char *nx = q;
            run += advance_of(a, &prev, utf8_next(&nx)) / g_scale;
            if (run > w) break;
            if (*q == ' ') brk = q;
            q = nx;
        }
        const char *end;
        if (!*q || *q == '\n') end = q;
        else end = brk && brk > line ? brk : q;
        if (end == line) { utf8_next(&end); }
        bool last = (lines == maxlines - 1) && *end && *end != '\n' ? true : false;
        if (last) {
            size_t n = strlen(line);
            char *tmp = malloc(n + 1);
            memcpy(tmp, line, n + 1);
            for (char *nl = tmp; *nl; nl++) if (*nl == '\n') *nl = ' ';
            text_draw_clip(f, size, x, y + lines * lh, w, c, AL_LEFT, tmp);
            free(tmp);
        } else {
            draw_run(a, snap(x), snap(y + lines * lh), c, line, end);
        }
        lines++;
        p = end;
        if (*p == '\n') p++;
    }
    return lines;
}

/* ---------- Images (asynchronous loading) ---------- */
struct Image {
    char *url;
    ImgState state;
    int w, h;
    unsigned char *pixels; /* RGBA, freed once the texture is made (fetched again from /tmp/tvimg if it is lost) */
    unsigned char *hdr_pixels; /* the same image converted to HDR (made in the background thread) */
    bool skipped;              /* skipped in the queue (was not on screen) - requested again */
    int pending;               /* jobs on their way for this image (it is never freed before they are done) */
    SDL_Texture *tex;
    SDL_Texture *mip[4]; /* half, quarter ... size copies (made on the GPU when the image is drawn much smaller) */
    int gen;
    double last_used;
    struct Image *next;
};
static Image *img_list;
static int img_count;
#define IMG_MAX_BYTES (16u << 20)        /* max file size for an image */
#define IMG_MAX_PIXELS (4096L * 4096L)   /* max resolution (64 MB RGBA) */
#define IMG_DISK_MAX (120LL << 20)       /* /tmp/tvimg is trimmed down to IMG_DISK_KEEP above this limit */
#define IMG_DISK_KEEP (80LL << 20)

/* /tmp/tvimg (RAM): delete the oldest files when the directory gets too big */
typedef struct { char name[24]; time_t t; off_t sz; } ImgFile;
static int imgfile_cmp(const void *a, const void *b) {
    const ImgFile *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t;
}
static void tvimg_trim_work(void *arg) {
    (void)arg;
    DIR *d = opendir("/tmp/tvimg");
    if (!d) return;
    ImgFile *f = NULL;
    int n = 0, cap = 0;
    long long total = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof f[0].name) continue;
        char p[64];
        snprintf(p, sizeof p, "/tmp/tvimg/%s", e->d_name);
        struct stat st;
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 256; f = realloc(f, sizeof *f * (size_t)cap); }
        snprintf(f[n].name, sizeof f[n].name, "%s", e->d_name);
        f[n].t = st.st_mtime;
        f[n].sz = st.st_size;
        total += st.st_size;
        n++;
    }
    closedir(d);
    if (total > IMG_DISK_MAX) {
        qsort(f, (size_t)n, sizeof *f, imgfile_cmp);
        int del = 0;
        for (int i = 0; i < n && total > IMG_DISK_KEEP; i++) {
            char p[64];
            snprintf(p, sizeof p, "/tmp/tvimg/%s", f[i].name);
            if (unlink(p) == 0) { total -= f[i].sz; del++; }
        }
        SDL_Log("billedcache: slettede %d gamle filer i /tmp/tvimg", del);
    }
    free(f);
}
void tvimg_trim(void) {
    static double last = -1000;
    if (app_time() - last < 600) return; /* at most every 10 minutes */
    last = app_time();
    job_submit_q(JQ_SLOW, tvimg_trim_work, NULL, NULL);
}

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

/* ---------- channel logos ----------
   Logos from IPTV providers come in every shape: dark text on a transparent background (invisible on our dark
   menus), a white background baked in, or a small logo in a big empty frame. An image asked for as "logo:<url>"
   (img_get_logo) is cleaned once when it is loaded: a white background becomes transparent, dark grey/black parts
   become light (coloured parts keep their colour), and the empty frame is cut away - so it can stand on its own
   without a white box. Logos with their own coloured background are kept as they are (only cropped). */
#define LOGO_PREFIX "logo:"
static const char *logo_strip(const char *url, bool *is_logo) {
    bool l = url && !strncmp(url, LOGO_PREFIX, sizeof LOGO_PREFIX - 1);
    if (is_logo) *is_logo = l;
    return l ? url + sizeof LOGO_PREFIX - 1 : url;
}
static float px_lum(const unsigned char *p) { return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f; }
static float px_sat(const unsigned char *p) {
    int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
    int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
    return mx ? (float)(mx - mn) / mx : 0;
}
/* Providers often send logos only 100 px wide (TV 2 NEWS: 100x18) that we show 2-3x bigger: the GPU's simple
   blend makes them blocky. A small logo is made bigger once here with a smooth cubic filter (Catmull-Rom, on
   premultiplied colour so the edges against transparency stay clean); the GPU then only makes it a little smaller. */
static float cubic_w(float t) {
    t = fabsf(t);
    return t < 1 ? 1.5f * t * t * t - 2.5f * t * t + 1 : t < 2 ? -0.5f * t * t * t + 2.5f * t * t - 4 * t + 2 : 0;
}
static void resample_1d(const float *src, int sw, int sstride, float *dst, int dw, int dstride) {
    float k = (float)sw / dw;
    for (int x = 0; x < dw; x++) {
        float c = (x + 0.5f) * k - 0.5f;
        int i0 = (int)floorf(c) - 1;
        float acc[4] = {0}, ws = 0;
        for (int i = i0; i < i0 + 4; i++) {
            float wt = cubic_w(c - i);
            int ii = i < 0 ? 0 : i >= sw ? sw - 1 : i;
            for (int ch = 0; ch < 4; ch++) acc[ch] += src[(size_t)ii * sstride + ch] * wt;
            ws += wt;
        }
        for (int ch = 0; ch < 4; ch++) dst[(size_t)x * dstride + ch] = acc[ch] / ws;
    }
}
static unsigned char *logo_upscale(unsigned char *px, int *pw, int *ph) {
    int w = *pw, h = *ph;
    float s = fminf(480.0f / w, 270.0f / h); /* up to about the biggest place a logo is shown, at most 4x */
    if (s > 4) s = 4;
    if (s < 1.3f) return px;
    int nw = (int)(w * s + 0.5f), nh = (int)(h * s + 0.5f);
    float *src = malloc((size_t)w * h * 4 * sizeof(float)), *mid = malloc((size_t)nw * h * 4 * sizeof(float)),
          *dst = malloc((size_t)nw * nh * 4 * sizeof(float));
    unsigned char *out = malloc((size_t)nw * nh * 4);
    if (!src || !mid || !dst || !out) { free(src); free(mid); free(dst); free(out); return px; }
    for (size_t i = 0; i < (size_t)w * h; i++) { /* premultiply */
        float a = px[i * 4 + 3] / 255.0f;
        src[i * 4] = px[i * 4] * a; src[i * 4 + 1] = px[i * 4 + 1] * a; src[i * 4 + 2] = px[i * 4 + 2] * a; src[i * 4 + 3] = a * 255;
    }
    for (int y = 0; y < h; y++) resample_1d(src + (size_t)y * w * 4, w, 4, mid + (size_t)y * nw * 4, nw, 4);
    for (int x = 0; x < nw; x++) resample_1d(mid + (size_t)x * 4, h, nw * 4, dst + (size_t)x * 4, nh, nw * 4);
    for (size_t i = 0; i < (size_t)nw * nh; i++) {
        float a = dst[i * 4 + 3];
        a = a < 0 ? 0 : a > 255 ? 255 : a;
        for (int c = 0; c < 3; c++) {
            float v = a > 0.5f ? dst[i * 4 + c] * 255 / a : 0;
            out[i * 4 + c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
        }
        out[i * 4 + 3] = (unsigned char)(a + 0.5f);
    }
    free(src); free(mid); free(dst);
    stbi_image_free(px);
    *pw = nw;
    *ph = nh;
    return out;
}

static unsigned char *logo_clean(unsigned char *px, int *pw, int *ph) {
    int w = *pw, h = *ph;
    if (w < 4 || h < 4) return px;
    /* the frame: transparent, white or another solid colour? */
    int n = 0, n_clear = 0, n_white = 0;
    long sr = 0, sg = 0, sb = 0;
    for (int i = 0; i < 2 * (w + h); i++) {
        int x = i < w ? i : i < 2 * w ? i - w : i < 2 * w + h ? 0 : w - 1;
        int y = i < w ? 0 : i < 2 * w ? h - 1 : i < 2 * w + h ? i - 2 * w : i - 2 * w - h;
        const unsigned char *p = px + ((size_t)y * w + x) * 4;
        n++;
        if (p[3] < 40) { n_clear++; continue; }
        if (px_lum(p) > 0.9f && px_sat(p) < 0.12f) n_white++;
        sr += p[0]; sg += p[1]; sb += p[2];
    }
    bool clear = n_clear * 10 >= n * 6, white = !clear && n_white * 10 >= n * 6;
    if (white) { /* the white background becomes transparent (the colour is "un-mixed" from the white) */
        for (size_t i = 0; i < (size_t)w * h; i++) {
            unsigned char *p = px + i * 4;
            int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
            float a = (1.0f - mn / 255.0f) * 1.6f; /* how far from white: colours and dark parts are opaque */
            if (a > 1) a = 1;
            a *= p[3] / 255.0f;
            if (a < 0.02f) { p[0] = p[1] = p[2] = 255; p[3] = 0; continue; }
            for (int c = 0; c < 3; c++) {
                float v = (p[c] - (1 - a) * 255) / a;
                p[c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            p[3] = (unsigned char)(a * 255);
        }
    }
    if (clear || white) {
        /* dark neutral parts (black/grey text) become light: they would disappear on the dark menus */
        double lum = 0, wsum = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            const unsigned char *p = px + i * 4;
            if (p[3] < 40) continue;
            lum += px_lum(p) * p[3];
            wsum += p[3];
        }
        if (wsum > 0 && lum / wsum < 0.5) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                unsigned char *p = px + i * 4;
                if (p[3] < 8 || px_sat(p) > 0.25f) continue;
                float l = px_lum(p);
                if (l >= 0.55f) continue;
                unsigned char v = (unsigned char)(255 * (1 - l * 0.6f)); /* black -> white, dark grey -> light grey */
                p[0] = p[1] = p[2] = v;
            }
        }
    }
    /* cut away the empty frame (what is the same as the background), with a small margin */
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    int br = n - n_clear ? (int)(sr / (n - n_clear)) : 0, bg = n - n_clear ? (int)(sg / (n - n_clear)) : 0, bb = n - n_clear ? (int)(sb / (n - n_clear)) : 0;
    bool solid = !clear && !white;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const unsigned char *p = px + ((size_t)y * w + x) * 4;
            bool content = solid ? abs(p[0] - br) + abs(p[1] - bg) + abs(p[2] - bb) > 60 : p[3] > 24;
            if (!content) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    if (solid) return logo_upscale(px, pw, ph); /* its own coloured box: keep it whole (cropping would cut its corners) */
    if (x1 < x0 || y1 < y0) return logo_upscale(px, pw, ph);
    int m = ((x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0)) / 25 + 1;
    x0 = x0 - m < 0 ? 0 : x0 - m; y0 = y0 - m < 0 ? 0 : y0 - m;
    x1 = x1 + m >= w ? w - 1 : x1 + m; y1 = y1 + m >= h ? h - 1 : y1 + m;
    int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    if (nw == w && nh == h) return logo_upscale(px, pw, ph);
    unsigned char *out = malloc((size_t)nw * nh * 4); /* freed with stbi_image_free (= free) like the decoded image */
    if (!out) return logo_upscale(px, pw, ph);
    for (int y = 0; y < nh; y++) memcpy(out + (size_t)y * nw * 4, px + ((size_t)(y + y0) * w + x0) * 4, (size_t)nw * 4);
    stbi_image_free(px);
    *pw = nw;
    *ph = nh;
    return logo_upscale(out, pw, ph);
}

/* fetch (cache in /tmp/tvimg) and decode an image; any thread. *hdr gets an HDR copy when the menus are in HDR */
static unsigned char *img_fetch(const char *url, int *ow, int *oh, unsigned char **hdr) {
    bool is_logo;
    url = logo_strip(url, &is_logo);
    unsigned char *data = NULL, *out = NULL;
    size_t len = 0;
    if (hdr) *hdr = NULL;
    if (strncmp(url, "http", 4) == 0) {
        char path[256];
        mkdir("/tmp/tvimg", 0755);
        snprintf(path, sizeof path, "/tmp/tvimg/%016lx", hash_str(url));
        data = read_file(path, &len);
        if (data) utimes(path, NULL); /* used: the cleanup deletes the oldest first */
        if (!data) {
            HttpBuf b = {0};
            if (http_request("GET", url, NULL, NULL, &b, 8) && b.status == 200 && b.len > 0 && b.len <= IMG_MAX_BYTES) {
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(b.data, 1, b.len, f); fclose(f); }
                data = (unsigned char *)b.data;
                len = b.len;
                b.data = NULL;
            }
            http_free(&b);
        }
    } else {
        data = read_file(url, &len);
    }
    if (data && len > IMG_MAX_BYTES) { free(data); data = NULL; }
    if (data) {
        int w, h, n;
        /* check the size before decoding: an "image" of 30000x30000 would otherwise use 3.6 GB */
        unsigned char *px = NULL;
        if (stbi_info_from_memory(data, (int)len, &w, &h, &n) && (long)w * h <= IMG_MAX_PIXELS)
            px = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
        if (px && is_logo) px = logo_clean(px, &w, &h);
        if (px) {
            out = px; *ow = w; *oh = h;
            /* HDR conversion here in the background thread - on the main thread scrolling stutters */
            if (g_hdr && hdr) *hdr = hdr_copy(px, w, h);
        }
        free(data);
    }
    return out;
}

/* the worker only fills in the job; the Image itself is only changed on the main thread (img_done) */
typedef struct { Image *im; char *url; bool skipped; unsigned char *px, *hdr; int w, h; } ImgJob;

static void img_work(void *arg) {
    ImgJob *j = arg;
    /* not shown for a while (scrolled away): skip, so the queue is not blocked - the image is requested
       again if it comes back on screen. (last_used is only read here; the Image stays while the job is pending.) */
    if (app_time() - j->im->last_used > 2.0) { j->skipped = true; return; }
    j->px = img_fetch(j->url, &j->w, &j->h, &j->hdr);
}

static void img_done(void *arg) {
    ImgJob *j = arg;
    Image *im = j->im;
    im->pending--;
    if (j->skipped) im->skipped = true; /* stays IMG_LOADING; img_get requests it again when needed */
    else if (im->state == IMG_LOADING && !im->pixels) {
        im->pixels = j->px; im->hdr_pixels = j->hdr; im->w = j->w; im->h = j->h;
        im->state = im->pixels ? IMG_READY : IMG_FAILED;
        j->px = j->hdr = NULL;
    }
    stbi_image_free(j->px);
    free(j->hdr);
    free(j->url);
    free(j);
}

static void img_submit(Image *im) {
    ImgJob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->im = im;
    j->url = str_dup(im->url);
    im->skipped = false;
    im->pending++;
    job_submit_q(JQ_IMAGE, img_work, img_done, j);
}

Image *img_get_logo(const char *url) {
    if (!url || !*url) return NULL;
    char key[1024];
    snprintf(key, sizeof key, LOGO_PREFIX "%s", url);
    return img_get(key);
}

Image *img_get(const char *url) {
    if (!url || !*url) return NULL;
    for (Image *im = img_list; im; im = im->next)
        if (strcmp(im->url, url) == 0) {
            im->last_used = app_time();
            if (im->skipped && !im->pending) img_submit(im);
            return im;
        }
    Image *im = calloc(1, sizeof *im);
    im->url = str_dup(url);
    im->state = IMG_LOADING;
    im->last_used = app_time();
    im->next = img_list;
    img_list = im;
    img_count++;
    img_submit(im);
    return im;
}

ImgState img_state(Image *im) { return im ? im->state : IMG_FAILED; }

static SDL_Texture *img_tex(Image *im) {
    if (!im || im->state != IMG_READY) return NULL;
    st_imgs++;
    if (im->tex && im->gen == g_generation) return im->tex;
    if (!im->pixels) { /* the texture is gone (renderer made again, HDR switched): load the image again */
        im->tex = NULL;
        if (!im->pending) { im->state = IMG_LOADING; img_submit(im); }
        return NULL;
    }
    st_tex_new++;
    memset(im->mip, 0, sizeof im->mip); /* the old ones went with the old renderer */
    im->tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im->w, im->h);
    if (!im->tex) return NULL;
    /* im->pixels stays SDR (also used for mpv's pause screen); the HDR copy is normally made when
       loading, but may be missing if "always HDR" was turned on afterwards */
    if (g_hdr && !im->hdr_pixels) im->hdr_pixels = hdr_copy(im->pixels, im->w, im->h);
    SDL_UpdateTexture(im->tex, NULL, g_hdr && im->hdr_pixels ? im->hdr_pixels : im->pixels, im->w * 4);
    SDL_SetTextureBlendMode(im->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(im->tex, SDL_SCALEMODE_LINEAR);
    im->gen = g_generation;
    /* the pixels are on the GPU now: a full-screen background no longer takes 8 MB (+8 MB HDR) of RAM as well */
    stbi_image_free(im->pixels);
    free(im->hdr_pixels);
    im->pixels = im->hdr_pixels = NULL;
    return im->tex;
}

static void draw_tex_rrect(SDL_Texture *t, float x, float y, float w, float h, float r,
                           float u0, float v0, float u1, float v1, float alpha) {
    SDL_FColor fc = {1, 1, 1, alpha};
    if (r < 0.5f) {
        SDL_Vertex v[4] = {{{x, y}, fc, {u0, v0}}, {{x + w, y}, fc, {u1, v0}},
                           {{x + w, y + h}, fc, {u1, v1}}, {{x, y + h}, fc, {u0, v1}}};
        int idx[6] = {0, 1, 2, 0, 2, 3};
        st_geom++, SDL_RenderGeometry(R, t, v, 4, idx, 6);
        return;
    }
    float vx[64], vy[64];
    int ni, nv = rrect_mesh(x, y, w, h, r, vx, vy, ibuf, &ni);
    for (int i = 0; i < nv; i++)
        vbuf[i] = (SDL_Vertex){{vx[i], vy[i]}, fc, {u0 + (vx[i] - x) / w * (u1 - u0), v0 + (vy[i] - y) / h * (v1 - v0)}};
    st_geom++, SDL_RenderGeometry(R, t, vbuf, nv, ibuf, ni);
}

bool draw_app_art(const char *id, float x, float y, float w, float h, float r, float alpha) {
    if (!id || !*id || strchr(id, '/') || strchr(id, '.')) return false;
    char path[512];
    snprintf(path, sizeof path, "%s/icons/%s.png", asset_dir(), id);
    static char missing[32][48]; /* ids without art: not looked for again every frame */
    static int n_missing;
    for (int i = 0; i < n_missing; i++) if (!strcmp(missing[i], id)) return false;
    if (access(path, R_OK) != 0) {
        if (n_missing < 32) snprintf(missing[n_missing++], sizeof missing[0], "%s", id);
        return false;
    }
    Image *im = img_get(path);
    if (!im || img_state(im) != IMG_READY) return false;
    return img_draw_cover(im, x, y, w, h, r, alpha);
}

void gfx_tex_rrect(SDL_Texture *t, float x, float y, float w, float h, float r, float alpha) {
    if (t) draw_tex_rrect(t, x, y, w, h, r, 0, 0, 1, 1, alpha);
}

/* the texture to draw an image with when it is shown dw x dh (UI units) big: a big image drawn small is sampled
   from a half/quarter-size copy, made once on the GPU (2x2 average) - without it a 1280 px picture or a big channel
   logo shown at 400 px looks grainy and jagged (the GPU only blends the 4 nearest pixels) */
static SDL_Texture *img_tex_sized(Image *im, float dw, float dh) {
    SDL_Texture *t = img_tex(im);
    if (!t) return NULL;
    float need_w = dw * g_scale, need_h = dh * g_scale;
    int w = im->w, h = im->h;
    for (int k = 0; k < 4; k++) {
        int nw = w / 2, nh = h / 2;
        if (nw < need_w || nh < need_h || nw < 8 || nh < 8) break; /* the next level would be too small */
        if (!im->mip[k]) {
            SDL_Texture *m = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, nw, nh);
            if (!m) break;
            SDL_SetTextureBlendMode(m, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(m, SDL_SCALEMODE_LINEAR);
            SDL_Texture *prev = SDL_GetRenderTarget(R);
            SDL_Rect clip;
            bool clipped = SDL_RenderClipEnabled(R);
            if (clipped) SDL_GetRenderClipRect(R, &clip);
            SDL_SetRenderTarget(R, m);
            SDL_SetRenderClipRect(R, NULL);
            SDL_SetRenderDrawColor(R, 0, 0, 0, 0);
            SDL_RenderClear(R);
            SDL_BlendMode bm;
            SDL_GetTextureBlendMode(t, &bm);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE); /* a copy, alpha included */
            SDL_SetTextureAlphaModFloat(t, 1);
            SDL_SetTextureColorModFloat(t, 1, 1, 1);
            SDL_RenderTexture(R, t, NULL, NULL);
            SDL_SetTextureBlendMode(t, bm);
            SDL_SetRenderTarget(R, prev);
            if (clipped) SDL_SetRenderClipRect(R, &clip);
            im->mip[k] = m;
            st_tex_new++;
        }
        t = im->mip[k];
        w = nw; h = nh;
    }
    return t;
}

bool img_draw(Image *im, float x, float y, float w, float h, float radius, float alpha) {
    SDL_Texture *t = img_tex(im);
    if (!t) return false;
    /* bevar forhold: tilpas inden for rektanglet */
    float ar = (float)im->w / im->h, br = w / h;
    float dw = w, dh = h;
    if (ar > br) dh = w / ar; else dw = h * ar;
    t = img_tex_sized(im, dw, dh);
    draw_tex_rrect(t, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, radius, 0, 0, 1, 1, alpha);
    return true;
}

bool img_draw_cover(Image *im, float x, float y, float w, float h, float radius, float alpha) {
    SDL_Texture *t = img_tex(im);
    if (!t) return false;
    float ar = (float)im->w / im->h, br = w / h;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (ar > br) { float k = br / ar; u0 = (1 - k) / 2; u1 = 1 - u0; }
    else { float k = ar / br; v0 = (1 - k) / 2; v1 = 1 - v0; }
    t = img_tex_sized(im, w / (u1 - u0), h / (v1 - v0)); /* the size the whole image is drawn at */
    draw_tex_rrect(t, x, y, w, h, radius, u0, v0, u1, v1, alpha);
    return true;
}

bool img_size(Image *im, int *w, int *h) {
    if (!im || im->state != IMG_READY) return false;
    *w = im->w; *h = im->h;
    return true;
}

bool draw_title_logo(const char *url, float x, float y, float maxw, float maxh, float alpha) {
    Image *im = img_get(url);
    int iw, ih;
    if (!img_size(im, &iw, &ih) || iw <= 0 || ih <= 0) return false;
    float sc = fminf(maxw / iw, maxh / ih), w = iw * sc, h = ih * sc;
    return img_draw(im, x, y + maxh - h, w, h, 0, alpha); /* bundjusteret, venstre */
}

bool img_cached(const char *url) {
    if (!url || !*url) return false;
    for (Image *im = img_list; im; im = im->next)
        if (!strcmp(im->url, url) && im->state == IMG_READY && im->pixels) return true; /* else: on disk? */
    url = logo_strip(url, NULL);
    if (strncmp(url, "http", 4)) return access(url, R_OK) == 0;
    char path[256];
    snprintf(path, sizeof path, "/tmp/tvimg/%016lx", hash_str(url));
    return access(path, R_OK) == 0;
}

bool img_export_bgra(const char *url, const char *path, int maxw, int maxh, int *ow, int *oh) {
    return img_export_bgra_ex(url, path, maxw, maxh, ow, oh, false);
}

/* hdr: convert to PQ/BT.2020 (203 nits white) - for mpv's OSD plane when the TV is in HDR */
bool img_export_bgra_ex(const char *url, const char *path, int maxw, int maxh, int *ow, int *oh, bool hdr) {
    if (hdr) hdr_init_lut();
    if (!url || !*url) return false;
    /* any thread: never looks at the image list (main thread only); fetched from /tmp/tvimg when it was shown before */
    int w = 0, h = 0;
    bool keep_alpha = false;
    logo_strip(url, &keep_alpha); /* a cleaned logo keeps its transparency (no white box behind it) */
    unsigned char *px = img_fetch(url, &w, &h, NULL);
    if (!px) return false;
    bool owned = true;
    float sc = fminf((float)maxw / w, (float)maxh / h);
    if (sc > 1) sc = 1;
    int dw = (int)(w * sc), dh = (int)(h * sc);
    if (dw < 1 || dh < 1) { if (owned) stbi_image_free(px); return false; }
    unsigned char *out = malloc((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        int sy0 = (int)(y / sc), sy1 = (int)((y + 1) / sc);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int x = 0; x < dw; x++) {
            /* the average of the source pixels this one covers (smooth when made smaller), alpha-weighted */
            int sx0 = (int)(x / sc), sx1 = (int)((x + 1) / sc);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            unsigned long ar = 0, ag = 0, ab = 0, aa = 0, cnt = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    const unsigned char *q = px + ((size_t)yy * w + xx) * 4;
                    ar += q[0] * q[3]; ag += q[1] * q[3]; ab += q[2] * q[3]; aa += q[3]; cnt++;
                }
            unsigned char avg[4] = {aa ? (unsigned char)(ar / aa) : 0, aa ? (unsigned char)(ag / aa) : 0,
                                    aa ? (unsigned char)(ab / aa) : 0, (unsigned char)(aa / cnt)};
            const unsigned char *s = avg;
            unsigned char *d = out + ((size_t)y * dw + x) * 4;
            uint8_t r = s[0], g = s[1], b = s[2];
            if (hdr) hdr_rgb(&r, &g, &b);
            if (keep_alpha) { /* premultiplied BGRA, as mpv's overlay-add wants it */
                unsigned a = s[3];
                d[0] = (uint8_t)(b * a / 255); d[1] = (uint8_t)(g * a / 255); d[2] = (uint8_t)(r * a / 255); d[3] = (uint8_t)a;
            } else {
                d[0] = b; d[1] = g; d[2] = r; d[3] = 255; /* RGBA -> BGRA (opaque) */
            }
        }
    }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(out, 1, (size_t)dw * dh * 4, f) == (size_t)dw * dh * 4;
    if (f) fclose(f);
    free(out);
    if (owned) stbi_image_free(px);
    *ow = dw;
    *oh = dh;
    return ok;
}

void img_cache_trim(void) {
    /* keep the latest ~120 images in memory (textures), the rest are fetched again from /tmp/tvimg */
    tvimg_trim();
    if (img_count < 150) return;
    double now = app_time();
    Image **pp = &img_list;
    while (*pp) {
        Image *im = *pp;
        if (!im->pending && (im->state != IMG_LOADING || im->skipped) && now - im->last_used > 60) {
            *pp = im->next;
            if (im->tex && im->gen == g_generation) {
                SDL_DestroyTexture(im->tex);
                for (int k = 0; k < 4; k++) if (im->mip[k]) SDL_DestroyTexture(im->mip[k]);
            }
            stbi_image_free(im->pixels);
            free(im->hdr_pixels);
            free(im->url);
            free(im);
            img_count--;
        } else {
            pp = &im->next;
        }
    }
}

/* ---------- Ikoner (vektor) ---------- */
static void ring(float cx, float cy, float r, float t, Color c) {
    stroke_rrect(cx - r, cy - r, 2 * r, 2 * r, r, t, c);
}

void draw_icon(IconId id, float cx, float cy, float s, Color c) {
    float h = s / 2;
    switch (id) {
    case ICON_TV:
        stroke_rrect(cx - h, cy - h * 0.72f, s, s * 0.62f, s * 0.08f, s * 0.07f, c);
        fill_rrect(cx - h * 0.45f, cy + h * 0.68f, s * 0.45f, s * 0.07f, s * 0.035f, c);
        break;
    case ICON_PLEX: {
        /* chevron */
        float w = s * 0.17f;
        SDL_FColor f = fcol(c);
        SDL_Vertex v[6] = {
            {{cx - h * 0.55f, cy - h * 0.85f}, f, {0, 0}}, {{cx - h * 0.55f + w * 1.6f, cy - h * 0.85f}, f, {0, 0}},
            {{cx + h * 0.55f, cy}, f, {0, 0}}, {{cx - h * 0.55f + w * 1.6f, cy + h * 0.85f}, f, {0, 0}},
            {{cx - h * 0.55f, cy + h * 0.85f}, f, {0, 0}}, {{cx + h * 0.55f - w * 1.6f, cy}, f, {0, 0}}};
        int idx[12] = {0, 1, 5, 1, 2, 5, 5, 2, 3, 5, 3, 4};
        st_geom++, SDL_RenderGeometry(R, NULL, v, 6, idx, 12);
        break;
    }
    case ICON_YOUTUBE:
        fill_rrect(cx - h, cy - h * 0.7f, s, s * 0.7f, s * 0.18f, c);
        fill_tri(cx - h * 0.25f, cy - h * 0.33f, cx - h * 0.25f, cy + h * 0.33f, cx + h * 0.33f, cy, RGB(230, 33, 23));
        break;
    case ICON_FOLDER:
        /* tab + folder, vertically centered as a whole (-0.80h .. +0.80h) */
        fill_rrect(cx - h, cy - h * 0.80f, s * 0.42f, s * 0.2f, s * 0.05f, c);
        fill_rrect(cx - h, cy - h * 0.64f, s, s * 0.72f, s * 0.07f, c);
        break;
    case ICON_GEAR:
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4;
            float tx = cx + cosf(a) * h * 0.78f, ty = cy + sinf(a) * h * 0.78f;
            fill_circle(tx, ty, s * 0.12f, c);
        }
        fill_circle(cx, cy, h * 0.75f, c);
        fill_circle(cx, cy, h * 0.3f, RGB(30, 34, 44));
        break;
    case ICON_PLAY:
        fill_tri(cx - h * 0.6f, cy - h * 0.75f, cx - h * 0.6f, cy + h * 0.75f, cx + h * 0.75f, cy, c);
        break;
    case ICON_WIFI:
        for (int i = 0; i < 3; i++) {
            float r = h * (0.35f + i * 0.33f);
            clip_push(cx - s, cy - s, 2 * s, s + h * 0.3f);
            ring(cx, cy + h * 0.55f, r, s * 0.09f, c);
            clip_pop();
        }
        fill_circle(cx, cy + h * 0.55f, s * 0.08f, c);
        break;
    case ICON_ETH:
        stroke_rrect(cx - h * 0.8f, cy - h * 0.6f, s * 0.8f, s * 0.75f, s * 0.06f, s * 0.08f, c);
        fill_rect(cx - h * 0.3f, cy + h * 0.15f, s * 0.3f, s * 0.12f, c);
        break;
    case ICON_STAR: {
        float px[10], py[10];
        for (int i = 0; i < 10; i++) {
            float a = -M_PI / 2 + i * M_PI / 5;
            float r = (i % 2) ? h * 0.42f : h;
            px[i] = cx + cosf(a) * r; py[i] = cy + sinf(a) * r;
        }
        for (int i = 0; i < 10; i++) fill_tri(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], c);
        break;
    }
    case ICON_SEARCH:
        ring(cx - h * 0.15f, cy - h * 0.15f, h * 0.6f, s * 0.1f, c);
        {
            float x1 = cx + h * 0.28f, y1 = cy + h * 0.28f, x2 = cx + h * 0.85f, y2 = cy + h * 0.85f, t = s * 0.07f;
            SDL_FColor f = fcol(c);
            SDL_Vertex v[4] = {{{x1 - t, y1 + t}, f, {0, 0}}, {{x1 + t, y1 - t}, f, {0, 0}},
                               {{x2 + t, y2 - t}, f, {0, 0}}, {{x2 - t, y2 + t}, f, {0, 0}}};
            int idx[6] = {0, 1, 2, 0, 2, 3};
            st_geom++, SDL_RenderGeometry(R, NULL, v, 4, idx, 6);
        }
        break;
    case ICON_POWER:
        clip_push(cx - s, cy - h * 0.35f, 2 * s, 2 * s);
        ring(cx, cy + h * 0.1f, h * 0.75f, s * 0.1f, c);
        clip_pop();
        clip_push(cx - s, cy - s, s - s * 0.2f, 2 * s);
        ring(cx, cy + h * 0.1f, h * 0.75f, s * 0.1f, c);
        clip_pop();
        clip_push(cx + s * 0.2f, cy - s, s, 2 * s);
        ring(cx, cy + h * 0.1f, h * 0.75f, s * 0.1f, c);
        clip_pop();
        fill_rrect(cx - s * 0.05f, cy - h * 0.95f, s * 0.1f, h * 0.95f, s * 0.05f, c);
        break;
    case ICON_MOON:
        fill_circle(cx, cy, h * 0.8f, c);
        fill_circle(cx + h * 0.4f, cy - h * 0.3f, h * 0.65f, C_SURFACE2);
        break;
    case ICON_BACK:
        fill_tri(cx + h * 0.4f, cy - h * 0.7f, cx + h * 0.4f, cy + h * 0.7f, cx - h * 0.5f, cy, c);
        break;
    case ICON_USB:
        fill_rrect(cx - h * 0.35f, cy - h * 0.2f, s * 0.35f, s * 0.6f, s * 0.05f, c);
        stroke_rrect(cx - h * 0.25f, cy - h * 0.8f, s * 0.25f, s * 0.32f, s * 0.02f, s * 0.05f, c);
        break;
    case ICON_MUSIC:
        fill_circle(cx - h * 0.35f, cy + h * 0.5f, h * 0.3f, c);
        fill_rect(cx - h * 0.12f, cy - h * 0.75f, s * 0.08f, s * 0.65f, c);
        fill_tri(cx - h * 0.12f, cy - h * 0.75f, cx + h * 0.6f, cy - h * 0.45f, cx - h * 0.12f, cy - h * 0.2f, c);
        break;
    case ICON_FILM:
        stroke_rrect(cx - h * 0.8f, cy - h * 0.6f, s * 0.8f, s * 0.6f, s * 0.05f, s * 0.07f, c);
        fill_tri(cx - h * 0.2f, cy - h * 0.3f, cx - h * 0.2f, cy + h * 0.3f, cx + h * 0.3f, cy, c);
        break;
    case ICON_CHECK: {
        float t = s * 0.07f;
        SDL_FColor f = fcol(c);
        float ax = cx - h * 0.6f, ay = cy, bx = cx - h * 0.15f, by = cy + h * 0.45f, ex = cx + h * 0.65f, ey = cy - h * 0.5f;
        SDL_Vertex v[6] = {{{ax, ay - t}, f, {0, 0}}, {{ax, ay + t}, f, {0, 0}}, {{bx, by - t * 1.6f}, f, {0, 0}},
                           {{bx, by + t * 1.4f}, f, {0, 0}}, {{ex, ey - t}, f, {0, 0}}, {{ex + t, ey + t}, f, {0, 0}}};
        int idx[12] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
        st_geom++, SDL_RenderGeometry(R, NULL, v, 6, idx, 12);
        break;
    }
    case ICON_LOCK:
        fill_rrect(cx - h * 0.6f, cy - h * 0.1f, s * 0.6f, s * 0.5f, s * 0.06f, c);
        clip_push(cx - s, cy - s, 2 * s, s * 0.95f);
        ring(cx, cy - h * 0.15f, h * 0.38f, s * 0.08f, c);
        clip_pop();
        break;
    case ICON_RESTART: /* a circle arrow, open at the top right */
        clip_push(cx - s, cy - s, s, 2 * s);
        ring(cx, cy, h * 0.75f, s * 0.1f, c);
        clip_pop();
        clip_push(cx, cy, s, s);
        ring(cx, cy, h * 0.75f, s * 0.1f, c);
        clip_pop();
        fill_tri(cx + h * 0.2f, cy - h * 1.05f, cx + h * 0.2f, cy - h * 0.45f, cx + h * 0.75f, cy - h * 0.75f, c);
        fill_rrect(cx - s * 0.02f, cy - h * 0.8f, h * 0.3f, s * 0.1f, s * 0.04f, c);
        break;
    case ICON_CLOCK: /* clock face with hands (10:10 is too busy - the hands point at 12 and 3) */
        ring(cx, cy, h * 0.85f, s * 0.08f, c);
        fill_rrect(cx - s * 0.04f, cy - h * 0.55f, s * 0.08f, h * 0.6f, s * 0.04f, c);
        fill_rrect(cx - s * 0.04f, cy - s * 0.04f, h * 0.48f, s * 0.08f, s * 0.04f, c);
        break;
    }
}

/* ---------- Init ---------- */
bool gfx_init(SDL_Renderer *r, int out_w, int out_h) {
    hdr_init_lut(); /* once here, so image exports in other threads only read it */
    R = r;
    g_generation++;
    g_scale = (float)out_h / UI_H;
    if ((float)out_w / UI_W < g_scale) g_scale = (float)out_w / UI_W;
    if (g_scale <= 0) g_scale = 1;
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    if (!font_data[F_REG]) {
        bool ok = load_font(F_REG, "Inter-Regular.ttf");
        ok &= load_font(F_SEMI, "Inter-SemiBold.ttf");
        ok &= load_font(F_BOLD, "Inter-Bold.ttf");
        if (!ok) return false;
    }
    SDL_Log("gfx: output %dx%d scale %.2f", out_w, out_h, g_scale);
    return true;
}

void gfx_lost(void) {
    /* textures belong to a renderer that is gone - forget them (without destroying) */
    g_generation++;
    for (Image *im = img_list; im; im = im->next) im->tex = NULL;
    for (int i = 0; i < n_atlas; i++) atlases[i]->tex = NULL;
    clip_n = 0;
    R = NULL;
}

void gfx_shutdown(void) { gfx_lost(); }

/* ---------- QR-kode ---------- */
#include "vendor/qrcodegen.h"
void draw_qr(const char *text, float x, float y, float size) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) return;
    int n = qrcodegen_getSize(qr);
    float quiet = 2, cell = size / (n + quiet * 2);
    fill_rrect(x, y, size, size, 16, RGB(255, 255, 255));
    for (int r = 0; r < n; r++) /* a run of dark modules in a row is one rectangle (was one call per module) */
        for (int c = 0; c < n;) {
            if (!qrcodegen_getModule(qr, c, r)) { c++; continue; }
            int e = c;
            while (e < n && qrcodegen_getModule(qr, e, r)) e++;
            fill_rect(x + (c + quiet) * cell, y + (r + quiet) * cell, (e - c) * cell + 0.5f, cell + 0.5f, RGB(0, 0, 0));
            c = e;
        }
}
