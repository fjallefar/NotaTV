/* NotaTV shell - own display presentation on the Raspberry Pi.
 *
 * SDL's KMSDRM driver always draws at the display's full resolution and shows the frame with SetCrtc/PageFlip.
 * In 4K the Pi 4's GPU cannot deliver that faster than ~20 fps (every frame must be converted from the GPU's
 * tiled format into a 4K image). Here SDL instead runs without a display ("offscreen") and draws into one of three
 * 1080p buffers, which the display hardware scales to 4K by itself - the same principle mpv uses. The display
 * therefore stays in 4K60 (and HDR) all the time, and the UI runs at 60 fps.
 *
 * Buffers: GBM buffer on the display card (vc4, can be shown directly) -> dmabuf -> EGLImage -> GL renderbuffer.
 * SDL draws into the texture K.ui, but its framebuffer gets the display buffer of the frame attached instead of the
 * texture (kms_present_begin), so the UI is drawn straight into the buffer the display shows - no full-screen copy
 * per frame (P5: ~8 MB read + 8 MB written each frame on the Pi 4). If the driver refuses that, or kms_direct=0
 * is set in notatv.conf, the old path is used: draw into the texture and copy it into the display buffer.
 */
#include "tv.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <glob.h>
#include <stdint.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define NBUF 3

typedef struct {
    struct gbm_bo *bo;
    uint32_t fb;
    EGLImageKHR img;
    GLuint rb, fbo; /* renderbuffer on the display buffer + framebuffer to copy into */
} Buf;

static struct {
    bool on;
    int fd;
    struct gbm_device *gbm;
    uint32_t crtc, plane, conn;
    int mode_w, mode_h;
    drmModeModeInfo mode; /* the mode the menus run in (display_pick_mode) */
    drmModeModeInfo safe; /* if setting it fails: the mode the display was in, else the TV's own preferred one */
    bool fell_back;
    bool modeset;         /* the display is not in that mode yet: the first frame sets it */
    bool headless;        /* no display connected: draw, but show nothing until one is (kms_present_display_changed) */
    uint32_t mode_blob, p_mode_id, p_active, p_conn_crtc;
    char sig[160];        /* the displays as they were when this was set up (display_signature) */
    bool y420;            /* 4K60 as packed YCbCr 4:2:0 (display_is_y420): every frame goes through pack_y420 */
    bool reinit;          /* the 4:2:0 mode failed: set the video up again without it */
    uint32_t p_bpc, p_cs, p_hdr;
    GLuint prog, vbo;
    GLint u_tex, u_par;
    float y420_par;
    uint32_t y420_id;     /* display_y420_id of the TV (main.c asks once per TV whether the picture came) */
    uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch;
    Buf buf[NBUF];
    SDL_Texture *ui;  /* SDL draws the UI here (ordinary GPU texture) */
    GLuint read_fbo;  /* framebuffer with the UI texture, which is copied to the display buffer */
    GLuint ui_fbo;    /* SDL's own framebuffer for K.ui (direct mode: gets the display buffer attached) */
    GLuint ui_gltex;  /* K.ui's GL texture (attached again when direct mode is not used) */
    bool direct;      /* drawing straight into the display buffers */
    int cur;          /* the buffer being drawn into now */
    int shown;        /* the buffer on screen (-1: none yet) */
    bool flip_pending;
} K = {.fd = -1, .shown = -1};

static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC p_glEGLImageTargetRenderbufferStorageOES;
typedef void (*BlitFn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static BlitFn p_glBlitFramebuffer;
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif

static uint32_t prop(uint32_t obj, uint32_t type, const char *name) {
    drmModeObjectProperties *pr = drmModeObjectGetProperties(K.fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; pr && i < pr->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(K.fd, pr->props[i]);
        if (p && !strcmp(p->name, name)) id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(pr);
    return id;
}

static void display_signature(char *out, size_t n);
static bool y420_failed;   /* the 4:2:0 mode did not work on this box: not again until restart */
static char rgb_mode[48];  /* "WxH@R" for mpv while the menus run in 4:2:0 (kms_present_rgb_mode) */

static bool same_mode(const drmModeModeInfo *a, const drmModeModeInfo *b) {
    return a->hdisplay == b->hdisplay && a->vdisplay == b->vdisplay && a->clock == b->clock && a->htotal == b->htotal &&
           a->vtotal == b->vtotal && a->flags == b->flags;
}

/* the CRTC a connector is on now, else the first one it can use */
static uint32_t conn_crtc(int fd, const drmModeRes *res, const drmModeConnector *cn, drmModeCrtc **cur) {
    *cur = NULL;
    if (cn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(fd, cn->encoder_id);
        uint32_t id = e ? e->crtc_id : 0;
        drmModeFreeEncoder(e);
        if (id) { *cur = drmModeGetCrtc(fd, id); return id; }
    }
    for (int k = 0; k < cn->count_encoders; k++) {
        drmModeEncoder *pe = drmModeGetEncoder(fd, cn->encoders[k]);
        for (int j = 0; pe && j < res->count_crtcs; j++)
            if (pe->possible_crtcs & (1u << j)) { drmModeFreeEncoder(pe); return res->crtcs[j]; }
        drmModeFreeEncoder(pe);
    }
    return 0;
}

/* Find the display and choose the mode the menus run in. Before, the mode the kernel's console set at boot was
   simply used - if the TV was in standby or on another input then (no EDID), the box stayed in an emergency mode
   (e.g. 1024x768) or found no display at all until it was restarted. Now the mode is chosen here every time the
   video is set up (display_pick_mode: 4K60 on a 4K TV, never 24/30 Hz), and set if the display is not in it.
   No display connected: the first HDMI connector is used "headless" until one is (kms_present_display_changed). */
static bool find_output(void) {
    rgb_mode[0] = 0;
    for (int pass = 0; pass < 2 && K.fd < 0; pass++)
        for (int c = 0; c < 4 && K.fd < 0; c++) {
            char path[32];
            snprintf(path, sizeof path, "/dev/dri/card%d", c);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            drmModeRes *res = drmModeGetResources(fd);
            for (int i = 0; res && i < res->count_connectors && K.fd < 0; i++) {
                /* a full probe (re-reads the EDID): the TV may have woken up since the kernel last looked */
                drmModeConnector *cn = drmModeGetConnector(fd, res->connectors[i]);
                bool use = cn && (pass == 0 ? cn->connection == DRM_MODE_CONNECTED && cn->count_modes > 0
                                            : cn->connector_type == DRM_MODE_CONNECTOR_HDMIA);
                drmModeCrtc *cur = NULL;
                uint32_t crtc = use ? conn_crtc(fd, res, cn, &cur) : 0;
                if (crtc) {
                    K.fd = fd; K.conn = cn->connector_id; K.crtc = crtc;
                    display_signature(K.sig, sizeof K.sig); /* after the probe above */
                    bool no_4k60 = false;
                    if (pass == 0 && display_pick_mode(y420_failed ? -1 : fd, cn, &K.mode, &no_4k60)) {
                        K.modeset = !(cur && cur->mode_valid && same_mode(&cur->mode, &K.mode));
                        K.safe = cn->modes[0];
                        for (int m = 0; m < cn->count_modes; m++)
                            if (cn->modes[m].type & DRM_MODE_TYPE_PREFERRED) { K.safe = cn->modes[m]; break; }
                        if (cur && cur->mode_valid && !display_is_y420(&cur->mode)) K.safe = cur->mode;
                        K.y420 = display_is_y420(&K.mode);
                        K.y420_id = K.y420 ? display_y420_id() : 0;
                        rgb_mode[0] = 0;
                        if (K.y420) {
                            /* what the menus would run in without 4:2:0 (and what mpv gets when no other mode fits) */
                            bool dummy;
                            drmModeModeInfo rgb;
                            if (display_pick_mode(-1, cn, &rgb, &dummy)) {
                                snprintf(rgb_mode, sizeof rgb_mode, "%dx%d@%.2f", rgb.hdisplay, rgb.vdisplay, display_mode_hz(&rgb));
                                K.safe = rgb;
                            }
                            no_4k60 = false;
                        }
                        SDL_Log("kms: skærm %s-%u: %s@%.2f%s (nu: %s)", cn->connector_type == DRM_MODE_CONNECTOR_HDMIA ? "HDMI-A" : "conn",
                                cn->connector_type_id, K.mode.name, display_mode_hz(&K.mode), no_4k60 ? ", 4K60 findes ikke på denne indgang" : "",
                                cur && cur->mode_valid ? cur->mode.name : "slukket");
                        if (no_4k60) {
                            static char warned[160];
                            if (strcmp(warned, K.sig)) { /* once per display, not after every movie */
                                snprintf(warned, sizeof warned, "%s", K.sig);
                                if (board_pi() == 4 && cn->connector_type == DRM_MODE_CONNECTOR_HDMIA && cn->connector_type_id == 2)
                                    app_toast("%s", T("For 4K at 60 Hz, plug the TV into the HDMI port next to the power socket"));
                                else
                                    app_toast("%s", T("The TV does not offer 4K at 60 Hz on this input. Turn on the enhanced HDMI format for the input in the TV's settings, and use an Ultra High Speed HDMI cable"));
                            }
                        }
                    } else {
                        K.headless = true;
                        SDL_Log("kms: ingen skærm tilsluttet - tegner uden skærm, indtil en bliver det");
                    }
                    K.mode_w = K.mode.hdisplay ? K.mode.hdisplay : 1920;
                    K.mode_h = K.mode.vdisplay ? K.mode.vdisplay : 1080;
                }
                drmModeFreeCrtc(cur);
                drmModeFreeConnector(cn);
            }
            drmModeFreeResources(res);
            if (K.fd < 0) close(fd);
        }
    return K.fd >= 0;
}

/* ---------- the display changing while the menus run ----------
   The kernel reads the EDID again on a hotplug (and retrains the 4K60 link), but the mode stays what we set. A
   short text per HDMI connector (status, EDID length and checksum) shows when that is no longer the right one:
   the TV woke up with its EDID, another display was plugged in, or a display was plugged into a box without one. */
static void display_signature(char *out, size_t n) {
    out[0] = 0;
    glob_t g;
    if (glob("/sys/class/drm/card*-HDMI-A-*", 0, NULL, &g) != 0) return;
    size_t len = 0;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char p[300], st[16] = "";
        snprintf(p, sizeof p, "%s/status", g.gl_pathv[i]);
        FILE *f = fopen(p, "r");
        if (f) { if (!fgets(st, sizeof st, f)) st[0] = 0; fclose(f); }
        uint32_t h = 2166136261u;
        long el = 0;
        snprintf(p, sizeof p, "%s/edid", g.gl_pathv[i]);
        f = fopen(p, "rb");
        if (f) {
            int ch;
            while ((ch = fgetc(f)) != EOF) { h = (h ^ (uint8_t)ch) * 16777619u; el++; }
            fclose(f);
        }
        const char *name = strrchr(g.gl_pathv[i], '-');
        len += (size_t)snprintf(out + len, len < n ? n - len : 0, "%s:%c%ld/%08x ", name ? name + 1 : "?", st[0] ? st[0] : '?', el, h);
        if (len >= n) break;
    }
    globfree(&g);
}

/* connected, but no EDID: some TVs (e.g. Google TV in standby) keep the hotplug line up and only answer again when
   asked, so the kernel never reads the EDID by itself. A probe on our own (master) file asks it to, on a thread
   (reading an EDID takes tens of ms). */
static SDL_AtomicInt probing;
static int probe_thread(void *ud) {
    int fd = (int)(intptr_t)ud;
    drmModeRes *res = drmModeGetResources(fd);
    for (int i = 0; res && i < res->count_connectors; i++) drmModeFreeConnector(drmModeGetConnector(fd, res->connectors[i]));
    drmModeFreeResources(res);
    close(fd);
    SDL_SetAtomicInt(&probing, 0);
    return 0;
}

bool kms_present_display_changed(void) {
    if (!K.on) return false;
    if (K.reinit) return true;
    char s[sizeof K.sig];
    display_signature(s, sizeof s);
    static char pending[sizeof K.sig];
    if (!strcmp(s, K.sig)) { pending[0] = 0; }
    else if (strcmp(s, pending)) { snprintf(pending, sizeof pending, "%s", s); return false; } /* the same twice in a row: a waking TV flaps */
    else {
        SDL_Log("kms: skærmen er ændret (%s -> %s) - sætter skærmen op igen", K.sig, s);
        pending[0] = 0;
        y420_failed = false; /* another TV may take it */
        return true;
    }
    /* unchanged: connected without an EDID (or a box running without a display) -> ask the TV again now and then */
    static Uint64 last_probe;
    Uint64 now = SDL_GetTicks();
    bool no_edid = strstr(s, ":c0/") != NULL;
    if ((no_edid || K.headless) && now - last_probe > 5000 && SDL_GetAtomicInt(&probing) == 0) {
        last_probe = now;
        int fd = dup(K.fd);
        if (fd >= 0) {
            SDL_SetAtomicInt(&probing, 1);
            SDL_Thread *t = SDL_CreateThread(probe_thread, "drm-probe", (void *)(intptr_t)fd);
            if (t) SDL_DetachThread(t);
            else { close(fd); SDL_SetAtomicInt(&probing, 0); }
        }
    }
    return false;
}

/* the size of the mode the menus last ran in (frame rate match during playback keeps it) */
static int last_w, last_h;
bool kms_present_last_mode(int *w, int *h) {
    if (!last_w) return false;
    *w = last_w; *h = last_h;
    return true;
}

bool kms_present_y420(void) { return K.on && K.y420; }
uint32_t kms_present_y420_id(void) { return K.on && K.y420 ? K.y420_id : 0; }
const char *kms_present_rgb_mode(void) { return rgb_mode; }

/* ---------- packing the UI as YCbCr 4:2:0 (see y420_mode in drm_util.c) ----------
   Output pixel x on line y carries luma of the 4K pixels 2x and 2x+1 and the chroma of their 2x2 block: Cb on even
   lines, Cr on odd ones. HDMI puts them on TMDS channel 1 (first Y), 2 (second Y) and 0 (chroma) - the channels RGB
   uses for G, R and B. Values are BT.709 limited range; the kernel patch sends them unchanged. y420_parity=1 in
   notatv.conf swaps Cb and Cr, should a TV want them the other way round.
   The UI is 1080p, so in 4K every UI pixel is a 2x2 block: both Y values and the chroma come from the same UI
   pixel (one nearest fetch per output pixel - the GPU is the bottleneck on a Pi 4, and an exact 2x scale loses
   nothing). */
static const char *pack_vs = "attribute vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *pack_fs =
    "precision highp float;\n"
    "uniform sampler2D t;\n"
    "uniform float par;\n"
    "const vec3 KY = vec3(65.481, 128.553, 24.966) / 255.0;\n"
    "const vec3 KB = vec3(-37.797, -74.203, 112.0) / 255.0;\n"
    "const vec3 KR = vec3(112.0, -93.786, -18.214) / 255.0;\n"
    "void main() {\n"
    "  vec3 a = texture2D(t, gl_FragCoord.xy * vec2(1.0 / 1920.0, 1.0 / 2160.0)).rgb;\n"
    "  float y = 16.0 / 255.0 + dot(a, KY);\n"
    "  float c = 128.0 / 255.0 + mix(dot(a, KB), dot(a, KR), mod(floor(gl_FragCoord.y) + par, 2.0));\n"
    "  gl_FragColor = vec4(y, y, c, 1.0);\n"
    "}\n";

static GLuint shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        SDL_Log("kms: 4:2:0-shader fejlede: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool pack_init(void) {
    GLuint vs = shader(GL_VERTEX_SHADER, pack_vs), fs = shader(GL_FRAGMENT_SHADER, pack_fs);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return false; }
    GLint prev_prog = 0, prev_buf = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
    K.prog = glCreateProgram();
    glAttachShader(K.prog, vs);
    glAttachShader(K.prog, fs);
    glBindAttribLocation(K.prog, 0, "p");
    glLinkProgram(K.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(K.prog, GL_LINK_STATUS, &ok);
    if (!ok) { SDL_Log("kms: 4:2:0-program fejlede"); glDeleteProgram(K.prog); K.prog = 0; return false; }
    K.u_tex = glGetUniformLocation(K.prog, "t");
    K.u_par = glGetUniformLocation(K.prog, "par");
    K.y420_par = cfg_int("y420_parity", 0) ? 1.0f : 0.0f;
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &K.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, K.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
    glUseProgram((GLuint)prev_prog);
    return true;
}

/* K.ui (1920x1080) -> packed 4:2:0 in the display buffer (1920x2160). SDL's GLES2 renderer keeps its own idea of the
   GL state, so everything touched here is put back exactly as it was. */
static void pack_y420(Buf *b) {
    GLint prog, abuf, atex, tex, vp[4], fbo, en, size, type, norm, stride, vbuf;
    void *ptr = NULL;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &atex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &vbuf);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
    GLboolean blend = glIsEnabled(GL_BLEND), sc = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    glViewport(0, 0, 1920, 2160);
    if (blend) glDisable(GL_BLEND);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glUseProgram(K.prog);
    glBindTexture(GL_TEXTURE_2D, K.ui_gltex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(K.u_tex, 0);
    glUniform1f(K.u_par, K.y420_par);
    glBindBuffer(GL_ARRAY_BUFFER, K.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    /* SDL keeps its own idea of the texture's filter (linear) - put it back */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vbuf);
    glVertexAttribPointer(0, size, (GLenum)type, (GLboolean)norm, stride, ptr);
    if (!en) glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glActiveTexture((GLenum)atex);
    glUseProgram((GLuint)prog);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (blend) glEnable(GL_BLEND);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
}

/* primary plane for our CRTC */
static bool find_plane(void) {
    drmSetClientCap(K.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModeRes *res = drmModeGetResources(K.fd);
    int crtc_idx = -1;
    for (int i = 0; res && i < res->count_crtcs; i++) if (res->crtcs[i] == K.crtc) crtc_idx = i;
    drmModeFreeResources(res);
    drmModePlaneRes *pr = drmModeGetPlaneResources(K.fd);
    for (uint32_t i = 0; pr && i < pr->count_planes && !K.plane; i++) {
        drmModePlane *p = drmModeGetPlane(K.fd, pr->planes[i]);
        if (p && crtc_idx >= 0 && (p->possible_crtcs & (1u << crtc_idx))) {
            drmModeObjectProperties *op = drmModeObjectGetProperties(K.fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            uint32_t tp = prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "type");
            for (uint32_t k = 0; op && k < op->count_props; k++)
                if (op->props[k] == tp && op->prop_values[k] == DRM_PLANE_TYPE_PRIMARY) K.plane = p->plane_id;
            drmModeFreeObjectProperties(op);
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    return K.plane != 0;
}

static bool make_buf(SDL_Renderer *ren, Buf *b, int w, int h) {
    b->bo = gbm_bo_create(K.gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!b->bo) { SDL_Log("kms: gbm_bo_create fejlede"); return false; }
    uint32_t handles[4] = {gbm_bo_get_handle(b->bo).u32}, pitches[4] = {gbm_bo_get_stride(b->bo)}, offsets[4] = {0};
    uint64_t mod = gbm_bo_get_modifier(b->bo), mods[4] = {mod};
    int r = mod != DRM_FORMAT_MOD_INVALID
                ? drmModeAddFB2WithModifiers(K.fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, mods, &b->fb, DRM_MODE_FB_MODIFIERS)
                : drmModeAddFB2(K.fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &b->fb, 0);
    if (r) { SDL_Log("kms: AddFB2 fejlede: %s", strerror(errno)); return false; }
    int dfd = gbm_bo_get_fd(b->bo);
    EGLint at[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
                   EGL_DMA_BUF_PLANE0_FD_EXT, dfd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                   EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitches[0],
                   EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
                   EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE};
    if (mod == DRM_FORMAT_MOD_INVALID) at[12] = EGL_NONE;
    b->img = p_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
    close(dfd);
    if (b->img == EGL_NO_IMAGE_KHR) { SDL_Log("kms: eglCreateImage fejlede (0x%x)", eglGetError()); return false; }
    /* render surface directly on the display buffer's memory (like an EGL window surface) - a texture on
       it does not work: the GPU then draws into its own copy and never writes back */
    glGenRenderbuffers(1, &b->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, b->rb);
    p_glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, b->img);
    glGenFramebuffers(1, &b->fbo);
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, b->rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    if (st != GL_FRAMEBUFFER_COMPLETE) { SDL_Log("kms: framebuffer ufuldstændig (0x%x)", st); return false; }
    return true;
}

static void free_buf(Buf *b) {
    if (b->fbo) glDeleteFramebuffers(1, &b->fbo);
    if (b->rb) glDeleteRenderbuffers(1, &b->rb);
    if (b->img) p_eglDestroyImageKHR(eglGetCurrentDisplay(), b->img);
    if (b->fb) drmModeRmFB(K.fd, b->fb);
    if (b->bo) gbm_bo_destroy(b->bo);
    memset(b, 0, sizeof *b);
}

void kms_present_quit(void) {
    if (K.fd < 0) return;
    /* a running probe holds a copy of our file (and thus DRM master): let it finish, so mpv can take over */
    for (int i = 0; i < 100 && SDL_GetAtomicInt(&probing); i++) SDL_Delay(10);
    if (K.mode_blob) drmModeDestroyPropertyBlob(K.fd, K.mode_blob);
    if (K.prog) glDeleteProgram(K.prog);
    if (K.vbo) glDeleteBuffers(1, &K.vbo);
    for (int i = 0; i < NBUF; i++) free_buf(&K.buf[i]);
    if (K.read_fbo) glDeleteFramebuffers(1, &K.read_fbo);
    if (K.ui) SDL_DestroyTexture(K.ui);
    if (K.gbm) gbm_device_destroy(K.gbm);
    close(K.fd); /* gives up DRM master (mpv can take over) */
    memset(&K, 0, sizeof K);
    K.fd = -1; K.shown = -1;
}

bool kms_present_init(SDL_Renderer *ren, int w, int h) {
    p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetRenderbufferStorageOES =
        (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    p_glBlitFramebuffer = (BlitFn)eglGetProcAddress("glBlitFramebuffer");
    SDL_Log("kms: %s", (const char *)glGetString(GL_VERSION));
    if (!p_eglCreateImageKHR || !p_glEGLImageTargetRenderbufferStorageOES || !p_glBlitFramebuffer ||
        eglGetCurrentDisplay() == EGL_NO_DISPLAY) {
        SDL_Log("kms: EGL-udvidelser mangler");
        return false;
    }
    if (!find_output()) { SDL_Log("kms: intet HDMI-udgang fundet"); return false; }
    if (drmSetMaster(K.fd) != 0) SDL_Log("kms: drmSetMaster: %s (fortsætter)", strerror(errno));
    drmSetClientCap(K.fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (!find_plane()) { SDL_Log("kms: ingen primær plan"); kms_present_quit(); return false; }
    if (K.y420) {
        /* 4:2:0 is 8 bit SDR in RGB form for the kernel (so 297 MHz, no YUV 4:2:2 for HDR): the connector must have
           max bpc 8, no BT.2020 and no HDR metadata - after an HDR movie it may not, and then the mode is set again */
        K.p_bpc = prop(K.conn, DRM_MODE_OBJECT_CONNECTOR, "max bpc");
        K.p_cs = prop(K.conn, DRM_MODE_OBJECT_CONNECTOR, "Colorspace");
        K.p_hdr = prop(K.conn, DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA");
        drmModeObjectProperties *cp = drmModeObjectGetProperties(K.fd, K.conn, DRM_MODE_OBJECT_CONNECTOR);
        for (uint32_t k = 0; cp && k < cp->count_props; k++)
            if ((cp->props[k] == K.p_bpc && cp->prop_values[k] != 8) || (cp->props[k] == K.p_cs && cp->prop_values[k]) ||
                (cp->props[k] == K.p_hdr && cp->prop_values[k]))
                K.modeset = true;
        drmModeFreeObjectProperties(cp);
        gfx_set_hdr(false);
    }
    if (K.modeset) {
        K.p_mode_id = prop(K.crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
        K.p_active = prop(K.crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
        K.p_conn_crtc = prop(K.conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        if (!K.p_mode_id || !K.p_active || !K.p_conn_crtc || drmModeCreatePropertyBlob(K.fd, &K.mode, sizeof K.mode, &K.mode_blob)) {
            SDL_Log("kms: kan ikke sætte skærmtilstand (mangler egenskaber)");
            kms_present_quit();
            return false;
        }
    }
    if (!K.headless) { last_w = K.y420 ? 3840 : K.mode_w; last_h = K.mode_h; }
    uint32_t pl = K.plane, t = DRM_MODE_OBJECT_PLANE;
    K.p_fb = prop(pl, t, "FB_ID"); K.p_crtc = prop(pl, t, "CRTC_ID");
    K.p_sx = prop(pl, t, "SRC_X"); K.p_sy = prop(pl, t, "SRC_Y"); K.p_sw = prop(pl, t, "SRC_W"); K.p_sh = prop(pl, t, "SRC_H");
    K.p_cx = prop(pl, t, "CRTC_X"); K.p_cy = prop(pl, t, "CRTC_Y"); K.p_cw = prop(pl, t, "CRTC_W"); K.p_ch = prop(pl, t, "CRTC_H");
    K.gbm = gbm_create_device(K.fd);
    if (!K.gbm) { kms_present_quit(); return false; }
    for (int i = 0; i < NBUF; i++)
        if (!make_buf(ren, &K.buf[i], K.y420 ? 1920 : w, K.y420 ? 2160 : h)) { kms_present_quit(); return false; }
    K.ui = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
    GLuint uitex = K.ui ? (GLuint)SDL_GetNumberProperty(SDL_GetTextureProperties(K.ui), SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0) : 0;
    if (!uitex) { SDL_Log("kms: UI-tekstur fejlede"); kms_present_quit(); return false; }
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glGenFramebuffers(1, &K.read_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, K.read_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, uitex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    K.ui_gltex = uitex;
    K.on = true;
    K.cur = 0;
    /* direct mode: find SDL's framebuffer for K.ui (SDL binds it at once in SetRenderTarget) and see whether the
       driver takes a display buffer as its color attachment */
    if (K.y420 && !pack_init()) {
        SDL_Log("kms: 4:2:0 kan ikke pakkes - bruger det almindelige signal");
        y420_failed = true;
        kms_present_quit();
        return kms_present_init(ren, w, h);
    }
    if (cfg_int("kms_direct", 1) && !K.y420) {
        SDL_Texture *was = SDL_GetRenderTarget(ren);
        SDL_SetRenderTarget(ren, NULL);
        SDL_SetRenderTarget(ren, K.ui);
        GLint fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        if (fbo > 0) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, K.buf[0].rb);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                K.ui_fbo = (GLuint)fbo;
                K.direct = true;
            } else {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, uitex, 0);
            }
        }
        SDL_SetRenderTarget(ren, was);
    }
    if (K.y420) SDL_Log("kms: UI i %dx%d, pakkes som 4K60 YCbCr 4:2:0 (mpv: %s)", w, h, rgb_mode[0] ? rgb_mode : "?");
    else
        SDL_Log("kms: UI i %dx%d, skaleres af skærmhardwaren til %dx%d (%s)", w, h, K.mode_w, K.mode_h,
                K.direct ? "tegnes direkte i skærmbufferen" : "kopieres til skærmbufferen");
    return true;
}

bool kms_present_active(void) { return K.on; }

SDL_Texture *kms_present_target(void) { return K.on ? K.ui : NULL; }

/* before a frame is drawn: point K.ui's framebuffer at the display buffer this frame goes into. That buffer is two
   flips old (NBUF 3, at most one flip pending), so the display no longer reads it. */
void kms_present_begin(SDL_Renderer *ren) {
    if (!K.on || !K.direct) return;
    SDL_FlushRenderer(ren); /* everything queued for the previous buffer goes there first */
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, K.ui_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, K.buf[K.cur].rb);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
}

/* copy the frame on screen into the current render target (same size), e.g. the photo under a new screen. In
   direct mode K.ui's texture is never drawn into, so the picture must come from the last display buffer.
   false = not in direct mode (the caller copies K.ui as before). */
bool kms_present_copy_shown(SDL_Renderer *ren) {
    if (!K.on || !K.direct || K.shown < 0) return false;
    SDL_FlushRenderer(ren);
    Buf *b = &K.buf[K.shown];
    GLint rf = 0, df = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
    if ((GLuint)df == K.ui_fbo || df == 0) return false; /* the target must be another texture */
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    int w = gbm_bo_get_width(b->bo), h = gbm_bo_get_height(b->bo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo);
    p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rf);
    if (sc) glEnable(GL_SCISSOR_TEST);
    return true;
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void *ud) { K.flip_pending = false; }

static void wait_flip(void) {
    drmEventContext ev = {.version = 2, .page_flip_handler = on_flip};
    while (K.flip_pending) {
        struct pollfd p = {K.fd, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) { K.flip_pending = false; break; } /* safety: never hang */
        drmHandleEvent(K.fd, &ev);
    }
}

/* show the finished buffer; the next frame is drawn into another one */
void kms_present_frame(SDL_Renderer *ren) {
    if (!K.on) return;
    SDL_FlushRenderer(ren);
    Buf *b = &K.buf[K.cur];
    if (K.y420) pack_y420(b);
    else if (!K.direct) {
    /* copy the UI texture into the display buffer; save/restore the GL state SDL relies on */
    GLint rf = 0, df = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    int w = gbm_bo_get_width(b->bo), h = gbm_bo_get_height(b->bo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, K.read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo);
    /* SDL's target texture and the display buffer have the same row order - copy directly */
    p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rf);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)df);
    if (sc) glEnable(GL_SCISSOR_TEST);
    }
    /* The display must not show the buffer before the GPU is done. vc4 (display) waits for v3d's fence on the buffer
       by itself before the flip (implicit sync), so glFlush is enough and the CPU can start on the next frame
       meanwhile. kms_finish=1 in notatv.conf goes back to glFinish (stalls the CPU every frame), should a TV show
       torn frames. */
    static int finish = -1;
    if (finish < 0) finish = cfg_int("kms_finish", 0) != 0;
    if (finish) glFinish();
    else glFlush();
    if (K.headless) { SDL_Delay(16); return; } /* no display: keep ~60 fps without showing anything */
    wait_flip(); /* at most one page flip at a time (= vsync) */
    drmModeAtomicReq *rq = drmModeAtomicAlloc();
    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    if (K.modeset) { /* the first frame also puts the display in the chosen mode */
        drmModeAtomicAddProperty(rq, K.crtc, K.p_mode_id, K.mode_blob);
        drmModeAtomicAddProperty(rq, K.crtc, K.p_active, 1);
        drmModeAtomicAddProperty(rq, K.conn, K.p_conn_crtc, K.crtc);
        if (K.y420) {
            if (K.p_bpc) drmModeAtomicAddProperty(rq, K.conn, K.p_bpc, 8);
            if (K.p_cs) drmModeAtomicAddProperty(rq, K.conn, K.p_cs, 0);
            if (K.p_hdr) drmModeAtomicAddProperty(rq, K.conn, K.p_hdr, 0);
        }
        flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
    }
    drmModeAtomicAddProperty(rq, K.plane, K.p_fb, b->fb);
    drmModeAtomicAddProperty(rq, K.plane, K.p_crtc, K.crtc);
    drmModeAtomicAddProperty(rq, K.plane, K.p_sx, 0);
    drmModeAtomicAddProperty(rq, K.plane, K.p_sy, 0);
    drmModeAtomicAddProperty(rq, K.plane, K.p_sw, (uint64_t)gbm_bo_get_width(b->bo) << 16);
    drmModeAtomicAddProperty(rq, K.plane, K.p_sh, (uint64_t)gbm_bo_get_height(b->bo) << 16);
    drmModeAtomicAddProperty(rq, K.plane, K.p_cx, 0);
    drmModeAtomicAddProperty(rq, K.plane, K.p_cy, 0);
    drmModeAtomicAddProperty(rq, K.plane, K.p_cw, K.mode_w);
    drmModeAtomicAddProperty(rq, K.plane, K.p_ch, K.mode_h);
    int r = drmModeAtomicCommit(K.fd, rq, flags, NULL);
    drmModeAtomicFree(rq);
    if (K.modeset) {
        K.modeset = false;
        if (!r) SDL_Log("kms: skærmen sat til %s@%.2f", K.mode.name, display_mode_hz(&K.mode));
        else {
            SDL_Log("kms: skærmtilstand %s fejlede: %s", K.mode.name, strerror(-r));
            if (K.y420) { /* the packed buffers only fit 4:2:0: set everything up again without it */
                y420_failed = true;
                K.reinit = true;
                return;
            }
            /* never a black screen: once more with the safe mode (the next frame) */
            uint32_t blob = 0;
            if (!K.fell_back && !same_mode(&K.safe, &K.mode) && !drmModeCreatePropertyBlob(K.fd, &K.safe, sizeof K.safe, &blob)) {
                K.fell_back = true;
                drmModeDestroyPropertyBlob(K.fd, K.mode_blob);
                K.mode_blob = blob;
                K.mode = K.safe;
                K.mode_w = last_w = K.mode.hdisplay;
                K.mode_h = last_h = K.mode.vdisplay;
                K.modeset = true;
            }
        }
    }
    if (r) {
        static int errs;
        if (errs++ < 5) SDL_Log("kms: sideskift fejlede: %s", strerror(-r));
        return;
    }
    K.flip_pending = true;
    K.shown = K.cur;
    K.cur = (K.cur + 1) % NBUF;
}
