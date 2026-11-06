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

/* find a card with a connected display and an active CRTC */
static bool find_output(void) {
    for (int c = 0; c < 4; c++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", c);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *res = drmModeGetResources(fd);
        for (int i = 0; res && i < res->count_connectors; i++) {
            drmModeConnector *cn = drmModeGetConnector(fd, res->connectors[i]);
            if (cn && cn->connection == DRM_MODE_CONNECTED && cn->encoder_id) {
                drmModeEncoder *e = drmModeGetEncoder(fd, cn->encoder_id);
                if (e && e->crtc_id) {
                    drmModeCrtc *crtc = drmModeGetCrtc(fd, e->crtc_id);
                    if (crtc && crtc->mode_valid) {
                        K.fd = fd; K.conn = cn->connector_id; K.crtc = e->crtc_id;
                        K.mode_w = crtc->mode.hdisplay; K.mode_h = crtc->mode.vdisplay;
                    }
                    drmModeFreeCrtc(crtc);
                }
                drmModeFreeEncoder(e);
            }
            drmModeFreeConnector(cn);
            if (K.fd >= 0) break;
        }
        drmModeFreeResources(res);
        if (K.fd >= 0) return true;
        close(fd);
    }
    return false;
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
    if (!find_output()) { SDL_Log("kms: ingen aktiv skærm"); return false; }
    if (drmSetMaster(K.fd) != 0) SDL_Log("kms: drmSetMaster: %s (fortsætter)", strerror(errno));
    drmSetClientCap(K.fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (!find_plane()) { SDL_Log("kms: ingen primær plan"); kms_present_quit(); return false; }
    uint32_t pl = K.plane, t = DRM_MODE_OBJECT_PLANE;
    K.p_fb = prop(pl, t, "FB_ID"); K.p_crtc = prop(pl, t, "CRTC_ID");
    K.p_sx = prop(pl, t, "SRC_X"); K.p_sy = prop(pl, t, "SRC_Y"); K.p_sw = prop(pl, t, "SRC_W"); K.p_sh = prop(pl, t, "SRC_H");
    K.p_cx = prop(pl, t, "CRTC_X"); K.p_cy = prop(pl, t, "CRTC_Y"); K.p_cw = prop(pl, t, "CRTC_W"); K.p_ch = prop(pl, t, "CRTC_H");
    K.gbm = gbm_create_device(K.fd);
    if (!K.gbm) { kms_present_quit(); return false; }
    for (int i = 0; i < NBUF; i++)
        if (!make_buf(ren, &K.buf[i], w, h)) { kms_present_quit(); return false; }
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
    if (cfg_int("kms_direct", 1)) {
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
    if (!K.direct) {
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
    wait_flip(); /* at most one page flip at a time (= vsync) */
    drmModeAtomicReq *rq = drmModeAtomicAlloc();
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
    int r = drmModeAtomicCommit(K.fd, rq, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL);
    drmModeAtomicFree(rq);
    if (r) {
        static int errs;
        if (errs++ < 5) SDL_Log("kms: sideskift fejlede: %s", strerror(-r));
        return;
    }
    K.flip_pending = true;
    K.shown = K.cur;
    K.cur = (K.cur + 1) % NBUF;
}
