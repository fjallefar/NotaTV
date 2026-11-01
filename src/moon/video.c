/* tvmoon - video: hardware decoding to DRM PRIME and display directly on the screen's primary plane.
 *
 * The Pi 4's GPU cannot draw 1080p60 video, so the frames bypass it:
 * HEVC is decoded by v4l2-request (rpi-ffmpeg's hwaccel), H.264 by v4l2m2m. Both give dmabufs
 * (SAND128/NV12/P030), which are put directly on the primary plane with atomic KMS and scaled by
 * the display hardware (HVS) to the screen's resolution.
 *
 * Latency: each frame is shown right away at the next vblank. If a new frame arrives before the
 * previous one reached the screen, the new one replaces the old one (the old one is skipped) - never a queue.
 */
#define _GNU_SOURCE
#include "tvmoon.h"
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct {
    AVFrame *frame;
    uint32_t fb;
    uint64_t recv_us; /* when the frame was received from the network (LiGetMicroseconds) */
} Shown;

static struct {
    VideoOpts opts;
    /* display */
    int fd;
    uint32_t conn, crtc, plane;
    drmModeModeInfo mode;
    uint32_t mode_blob;
    uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch, p_enc, p_range;
    uint32_t p_mode, p_active, p_conn_crtc, p_cs, p_hdr, p_bpc;
    uint64_t enc_709, enc_2020, range_lim, cs_def, cs_2020;
    bool need_modeset;
    bool hdr_req, hdr_cur;
    /* dekoder */
    AVCodecContext *cc;
    AVBufferRef *hw;
    AVPacket *pkt;
    uint8_t *buf;
    size_t cap;
    uint64_t t_recv[256], t_submit[256];
    /* display thread */
    pthread_t th;
    bool th_on;
    pthread_mutex_t mu;
    int evfd;
    volatile bool quit;
    Shown next;     /* newest decoded frame, waiting to go on screen */
    Shown queued;   /* sent to the display, waiting for vblank */
    Shown onscreen; /* shown now */
    bool flip_pending;
    bool ov_only;   /* the pending commit only touched the overlay */
    /* statistics (per second) */
    int n_rx, n_dec, n_shown, n_skip, n_err;
    uint64_t bytes;
    int fmt, sw, sh;
    double sum_dec_ms, sum_total_ms, max_total_ms, sum_host_ms;
    int n_host;
} V = {.fd = -1, .evfd = -1, .mu = PTHREAD_MUTEX_INITIALIZER};

void video_configure(const VideoOpts *o) { V.opts = *o; }

/* ---------- DRM ---------- */

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name, const char *enum_name, uint64_t *enum_val) {
    drmModeObjectProperties *pr = drmModeObjectGetProperties(V.fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; pr && i < pr->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(V.fd, pr->props[i]);
        if (p && !strcmp(p->name, name)) {
            id = p->prop_id;
            if (enum_name && enum_val)
                for (int e = 0; e < p->count_enums; e++)
                    if (!strcmp(p->enums[e].name, enum_name)) *enum_val = p->enums[e].value;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(pr);
    return id;
}

static bool pick_mode(drmModeConnector *cn, drmModeCrtc *crtc) {
    int want_w = 0, want_h = 0, want_r = 60;
    if (V.opts.mode) sscanf(V.opts.mode, "%dx%d@%d", &want_w, &want_h, &want_r);
    /* without a request: keep the mode the display is in (usually the UI's 4K60) - no switch on the TV */
    if (!want_w && crtc && crtc->mode_valid) { V.mode = crtc->mode; return true; }
    int best = -1, best_score = -1;
    for (int i = 0; i < cn->count_modes; i++) {
        drmModeModeInfo *m = &cn->modes[i];
        int score = 0;
        if (want_w) {
            if (m->hdisplay != want_w || m->vdisplay != want_h) continue;
        } else if (m->type & DRM_MODE_TYPE_PREFERRED) score += 4;
        if ((int)m->vrefresh == want_r) score += 8; /* e.g. 1080p120: half the wait for vblank */
        else if (m->vrefresh == 60) score += 2;
        if (!(m->flags & DRM_MODE_FLAG_INTERLACE)) score += 1;
        if (score > best_score) { best = i; best_score = score; }
    }
    if (best < 0) {
        if (crtc && crtc->mode_valid) { V.mode = crtc->mode; return true; }
        return false;
    }
    V.mode = cn->modes[best];
    return true;
}

static bool drm_open(void) {
    for (int c = 0; c < 4 && V.fd < 0; c++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", c);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *res = drmModeGetResources(fd);
        for (int i = 0; res && i < res->count_connectors && V.fd < 0; i++) {
            drmModeConnector *cn = drmModeGetConnector(fd, res->connectors[i]);
            if (cn && cn->connection == DRM_MODE_CONNECTED && cn->count_modes > 0) {
                uint32_t crtc_id = 0;
                drmModeEncoder *e = cn->encoder_id ? drmModeGetEncoder(fd, cn->encoder_id) : NULL;
                if (e && e->crtc_id) crtc_id = e->crtc_id;
                drmModeFreeEncoder(e);
                for (int k = 0; !crtc_id && k < cn->count_encoders; k++) {
                    drmModeEncoder *en = drmModeGetEncoder(fd, cn->encoders[k]);
                    for (int j = 0; en && j < res->count_crtcs && !crtc_id; j++)
                        if (en->possible_crtcs & (1u << j)) crtc_id = res->crtcs[j];
                    drmModeFreeEncoder(en);
                }
                if (crtc_id) {
                    V.fd = fd;
                    V.conn = cn->connector_id;
                    V.crtc = crtc_id;
                    drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
                    if (!pick_mode(cn, crtc)) V.fd = -1;
                    drmModeFreeCrtc(crtc);
                }
            }
            drmModeFreeConnector(cn);
        }
        drmModeFreeResources(res);
        if (V.fd < 0) close(fd);
    }
    if (V.fd < 0) { tlog("video: ingen tilsluttet skærm"); return false; }
    /* tvshell releases the display just before we start: wait a little for DRM master */
    for (int i = 0; i < 60 && drmSetMaster(V.fd) != 0; i++) usleep(50000);
    if (drmSetClientCap(V.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) || drmSetClientCap(V.fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        tlog("video: atomic KMS ikke tilgængelig");
        return false;
    }
    /* primary plane for our CRTC (vc4: overlay planes' zpos starts at 1; video on the primary) */
    drmModeRes *res = drmModeGetResources(V.fd);
    int crtc_idx = -1;
    for (int i = 0; res && i < res->count_crtcs; i++) if (res->crtcs[i] == V.crtc) crtc_idx = i;
    drmModeFreeResources(res);
    drmModePlaneRes *pr = drmModeGetPlaneResources(V.fd);
    for (uint32_t i = 0; pr && i < pr->count_planes && !V.plane; i++) {
        drmModePlane *p = drmModeGetPlane(V.fd, pr->planes[i]);
        if (p && crtc_idx >= 0 && (p->possible_crtcs & (1u << crtc_idx))) {
            uint64_t primary = DRM_PLANE_TYPE_PRIMARY, val = ~0ull;
            uint32_t tp = prop_id(p->plane_id, DRM_MODE_OBJECT_PLANE, "type", NULL, NULL);
            drmModeObjectProperties *op = drmModeObjectGetProperties(V.fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            for (uint32_t k = 0; op && k < op->count_props; k++) if (op->props[k] == tp) val = op->prop_values[k];
            drmModeFreeObjectProperties(op);
            if (val == primary) V.plane = p->plane_id;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    if (!V.plane) { tlog("video: intet primært plan"); return false; }
    overlay_init(V.fd, V.crtc, crtc_idx, V.mode.hdisplay, V.mode.vdisplay);
    uint32_t pl = V.plane, t = DRM_MODE_OBJECT_PLANE;
    V.p_fb = prop_id(pl, t, "FB_ID", NULL, NULL);
    V.p_crtc = prop_id(pl, t, "CRTC_ID", NULL, NULL);
    V.p_sx = prop_id(pl, t, "SRC_X", NULL, NULL);
    V.p_sy = prop_id(pl, t, "SRC_Y", NULL, NULL);
    V.p_sw = prop_id(pl, t, "SRC_W", NULL, NULL);
    V.p_sh = prop_id(pl, t, "SRC_H", NULL, NULL);
    V.p_cx = prop_id(pl, t, "CRTC_X", NULL, NULL);
    V.p_cy = prop_id(pl, t, "CRTC_Y", NULL, NULL);
    V.p_cw = prop_id(pl, t, "CRTC_W", NULL, NULL);
    V.p_ch = prop_id(pl, t, "CRTC_H", NULL, NULL);
    V.enc_709 = 1; V.enc_2020 = 2; V.range_lim = 0;
    V.p_enc = prop_id(pl, t, "COLOR_ENCODING", "ITU-R BT.709 YCbCr", &V.enc_709);
    prop_id(pl, t, "COLOR_ENCODING", "ITU-R BT.2020 YCbCr", &V.enc_2020);
    V.p_range = prop_id(pl, t, "COLOR_RANGE", "YCbCr limited range", &V.range_lim);
    V.p_mode = prop_id(V.crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL, NULL);
    V.p_active = prop_id(V.crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL, NULL);
    t = DRM_MODE_OBJECT_CONNECTOR;
    V.p_conn_crtc = prop_id(V.conn, t, "CRTC_ID", NULL, NULL);
    V.p_cs = prop_id(V.conn, t, "Colorspace", "Default", &V.cs_def);
    prop_id(V.conn, t, "Colorspace", "BT2020_RGB", &V.cs_2020);
    V.p_hdr = prop_id(V.conn, t, "HDR_OUTPUT_METADATA", NULL, NULL);
    V.p_bpc = prop_id(V.conn, t, "max bpc", NULL, NULL);
    drmModeCreatePropertyBlob(V.fd, &V.mode, sizeof V.mode, &V.mode_blob);
    V.need_modeset = true;
    tlog("video: skærm %dx%d@%d, plan %u, crtc %u", V.mode.hdisplay, V.mode.vdisplay, V.mode.vrefresh, V.plane, V.crtc);
    return true;
}

/* HDR10 infoframe from the host's metadata (like drm_util.c in tvshell, but with the game's values) */
static uint32_t hdr_blob(void) {
    struct hdr_output_metadata md = {0};
    md.metadata_type = 0;
    md.hdmi_metadata_type1.eotf = 2; /* PQ */
    SS_HDR_METADATA h;
    if (LiGetHdrMetadata(&h) && h.displayPrimaries[0].x) {
        for (int k = 0; k < 3; k++) {
            md.hdmi_metadata_type1.display_primaries[k].x = h.displayPrimaries[k].x;
            md.hdmi_metadata_type1.display_primaries[k].y = h.displayPrimaries[k].y;
        }
        md.hdmi_metadata_type1.white_point.x = h.whitePoint.x;
        md.hdmi_metadata_type1.white_point.y = h.whitePoint.y;
        md.hdmi_metadata_type1.max_display_mastering_luminance = h.maxDisplayLuminance;
        md.hdmi_metadata_type1.min_display_mastering_luminance = h.minDisplayLuminance;
        md.hdmi_metadata_type1.max_cll = h.maxContentLightLevel;
        md.hdmi_metadata_type1.max_fall = h.maxFrameAverageLightLevel;
    } else {
        /* the host sent nothing: the same fixed values as the UI (BT.2020, 1000 nits) */
        static const uint16_t prim[4][2] = {{35400, 14600}, {8500, 39850}, {6550, 2300}, {15635, 16450}};
        for (int k = 0; k < 3; k++) {
            md.hdmi_metadata_type1.display_primaries[k].x = prim[k][0];
            md.hdmi_metadata_type1.display_primaries[k].y = prim[k][1];
        }
        md.hdmi_metadata_type1.white_point.x = prim[3][0];
        md.hdmi_metadata_type1.white_point.y = prim[3][1];
        md.hdmi_metadata_type1.max_display_mastering_luminance = 1000;
        md.hdmi_metadata_type1.min_display_mastering_luminance = 50;
        md.hdmi_metadata_type1.max_cll = 1000;
        md.hdmi_metadata_type1.max_fall = 400;
    }
    uint32_t blob = 0;
    drmModeCreatePropertyBlob(V.fd, &md, sizeof md, &blob);
    return blob;
}

void video_set_hdr(bool on) {
    pthread_mutex_lock(&V.mu);
    V.hdr_req = on;
    pthread_mutex_unlock(&V.mu);
}

static void shown_free(Shown *s) {
    if (s->fb) drmModeRmFB(V.fd, s->fb);
    av_frame_free(&s->frame);
    memset(s, 0, sizeof *s);
}

/* frame -> framebuffer (the dmabuf is imported; the FB holds its own reference to the buffer) */
static uint32_t frame_fb(AVFrame *f) {
    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->data[0];
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0}, bo[AV_DRM_MAX_PLANES] = {0};
    uint64_t mods[4] = {0};
    bool has_mod = false;
    for (int i = 0; i < d->nb_objects; i++) {
        if (drmPrimeFDToHandle(V.fd, d->objects[i].fd, &bo[i])) return 0;
        if (d->objects[i].format_modifier != DRM_FORMAT_MOD_LINEAR && d->objects[i].format_modifier != DRM_FORMAT_MOD_INVALID)
            has_mod = true;
    }
    int n = 0;
    for (int l = 0; l < d->nb_layers; l++)
        for (int p = 0; p < d->layers[l].nb_planes && n < 4; p++, n++) {
            const AVDRMPlaneDescriptor *pd = &d->layers[l].planes[p];
            handles[n] = bo[pd->object_index];
            pitches[n] = (uint32_t)pd->pitch;
            offsets[n] = (uint32_t)pd->offset;
            mods[n] = d->objects[pd->object_index].format_modifier;
        }
    uint32_t fb = 0;
    int r = drmModeAddFB2WithModifiers(V.fd, f->width, f->height, d->layers[0].format, handles, pitches, offsets,
                                       has_mod ? mods : NULL, &fb, has_mod ? DRM_MODE_FB_MODIFIERS : 0);
    for (int i = 0; i < d->nb_objects; i++)
        if (bo[i]) drmIoctl(V.fd, DRM_IOCTL_GEM_CLOSE, &(struct drm_gem_close){.handle = bo[i]});
    if (r) {
        static int errs;
        if (errs++ < 5) tlog("video: AddFB2 fejlede (%s, format %.4s, mod 0x%llx)", strerror(errno),
                             (const char *)&d->layers[0].format, (unsigned long long)d->objects[0].format_modifier);
        return 0;
    }
    return fb;
}

/* sent to the display; called in the display thread with the lock held */
static void commit(Shown *s) {
    AVFrame *f = s->frame;
    s->fb = frame_fb(f);
    if (!s->fb) { shown_free(s); V.n_err++; return; }
    int sw = (int)(f->width - f->crop_left - f->crop_right), sh = (int)(f->height - f->crop_top - f->crop_bottom);
    /* keep the aspect ratio within the screen */
    int mw = V.mode.hdisplay, mh = V.mode.vdisplay, dw = mw, dh = mh;
    if ((int64_t)sw * mh > (int64_t)sh * mw) dh = (int)((int64_t)mw * sh / sw) & ~1;
    else dw = (int)((int64_t)mh * sw / sh) & ~1;
    drmModeAtomicReq *rq = drmModeAtomicAlloc();
    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    uint32_t hdrblob = 0;
    bool hdr = V.hdr_req;
    if (V.need_modeset || hdr != V.hdr_cur) {
        /* first frame (and HDR on/off): full mode including HDMI signaling. If the TV is already
           in the same mode, the kernel does no modeset (no black screen) */
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
        drmModeAtomicAddProperty(rq, V.crtc, V.p_mode, V.mode_blob);
        drmModeAtomicAddProperty(rq, V.crtc, V.p_active, 1);
        drmModeAtomicAddProperty(rq, V.conn, V.p_conn_crtc, V.crtc);
        if (hdr) hdrblob = hdr_blob();
        if (V.p_cs) drmModeAtomicAddProperty(rq, V.conn, V.p_cs, hdr ? V.cs_2020 : V.cs_def);
        if (V.p_hdr) drmModeAtomicAddProperty(rq, V.conn, V.p_hdr, hdrblob);
        if (V.p_bpc && hdr) drmModeAtomicAddProperty(rq, V.conn, V.p_bpc, 12);
        if (V.p_enc) drmModeAtomicAddProperty(rq, V.plane, V.p_enc, hdr ? V.enc_2020 : V.enc_709);
        if (V.p_range) drmModeAtomicAddProperty(rq, V.plane, V.p_range, V.range_lim);
    }
    drmModeAtomicAddProperty(rq, V.plane, V.p_fb, s->fb);
    drmModeAtomicAddProperty(rq, V.plane, V.p_crtc, V.crtc);
    drmModeAtomicAddProperty(rq, V.plane, V.p_sx, (uint64_t)f->crop_left << 16);
    drmModeAtomicAddProperty(rq, V.plane, V.p_sy, (uint64_t)f->crop_top << 16);
    drmModeAtomicAddProperty(rq, V.plane, V.p_sw, (uint64_t)sw << 16);
    drmModeAtomicAddProperty(rq, V.plane, V.p_sh, (uint64_t)sh << 16);
    drmModeAtomicAddProperty(rq, V.plane, V.p_cx, (mw - dw) / 2);
    drmModeAtomicAddProperty(rq, V.plane, V.p_cy, (mh - dh) / 2);
    drmModeAtomicAddProperty(rq, V.plane, V.p_cw, dw);
    drmModeAtomicAddProperty(rq, V.plane, V.p_ch, dh);
    overlay_commit(rq);
    int r = drmModeAtomicCommit(V.fd, rq, flags, NULL);
    drmModeAtomicFree(rq);
    if (hdrblob) drmModeDestroyPropertyBlob(V.fd, hdrblob);
    if (r) {
        static int errs;
        if (errs++ < 5) tlog("video: atomic commit fejlede: %s%s", strerror(-r), flags & DRM_MODE_ATOMIC_ALLOW_MODESET ? " (modeset)" : "");
        shown_free(s);
        V.n_err++;
        return;
    }
    if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
        tlog("video: %s på skærmen (%dx%d -> %dx%d)", hdr ? "HDR10" : "SDR", sw, sh, dw, dh);
        V.need_modeset = false;
        V.hdr_cur = hdr;
    }
    V.queued = *s;
    memset(s, 0, sizeof *s);
    V.flip_pending = true;
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void *ud) {
    pthread_mutex_lock(&V.mu);
    V.flip_pending = false;
    if (V.ov_only) { /* only the overlay changed: the same video frame is still shown */
        V.ov_only = false;
        if (V.next.frame) commit(&V.next);
        pthread_mutex_unlock(&V.mu);
        return;
    }
    shown_free(&V.onscreen);
    V.onscreen = V.queued;
    memset(&V.queued, 0, sizeof V.queued);
    if (V.onscreen.frame) {
        V.n_shown++;
        if (V.onscreen.recv_us) {
            double ms = (LiGetMicroseconds() - V.onscreen.recv_us) / 1000.0;
            V.sum_total_ms += ms;
            if (ms > V.max_total_ms) V.max_total_ms = ms;
        }
    }
    if (V.next.frame) commit(&V.next);
    pthread_mutex_unlock(&V.mu);
}

/* overlay changed, but no new video frame on the way (e.g. a still desktop): send it alone */
static void commit_overlay_only(void) {
    drmModeAtomicReq *rq = drmModeAtomicAlloc();
    overlay_commit(rq);
    if (drmModeAtomicCommit(V.fd, rq, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
        V.flip_pending = true;
        V.ov_only = true;
    }
    drmModeAtomicFree(rq);
}

static void set_rt(int prio) {
    struct sched_param sp = {.sched_priority = prio};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) tlog("video: realtidsprioritet ikke mulig");
}

static void *display_thread(void *arg) {
    drmEventContext ev = {.version = 2, .page_flip_handler = on_flip};
    set_rt(12);
    while (!V.quit) {
        struct pollfd p[2] = {{V.fd, POLLIN, 0}, {V.evfd, POLLIN, 0}};
        if (poll(p, 2, 100) < 0 && errno != EINTR) break;
        if (p[0].revents & POLLIN) drmHandleEvent(V.fd, &ev);
        if (p[1].revents & POLLIN) {
            uint64_t v;
            if (read(V.evfd, &v, sizeof v) < 0) { /* nothing */ }
            pthread_mutex_lock(&V.mu);
            if (!V.flip_pending && V.next.frame) commit(&V.next);
            pthread_mutex_unlock(&V.mu);
        }
        pthread_mutex_lock(&V.mu);
        if (!V.flip_pending && !V.next.frame && overlay_dirty() && !V.need_modeset) commit_overlay_only();
        pthread_mutex_unlock(&V.mu);
    }
    return NULL;
}

/* ---------- dekoder ---------- */

static enum AVPixelFormat get_format(AVCodecContext *c, const enum AVPixelFormat *fmts) {
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_DRM_PRIME) return *p;
    tlog("video: dekoderen tilbyder ikke DRM PRIME (første format: %s)", av_get_pix_fmt_name(fmts[0]));
    return AV_PIX_FMT_NONE;
}

static void decoder_close(void) {
    if (V.cc) avcodec_free_context(&V.cc);
    av_buffer_unref(&V.hw);
    av_packet_free(&V.pkt);
    free(V.buf);
    V.buf = NULL;
    V.cap = 0;
}

static bool decoder_open(int fmt, int w, int h) {
    const AVCodec *codec;
    if (fmt & VIDEO_FORMAT_MASK_H265) {
        codec = avcodec_find_decoder(AV_CODEC_ID_HEVC); /* + v4l2-request hwaccel via the DRM device */
        if (av_hwdevice_ctx_create(&V.hw, AV_HWDEVICE_TYPE_DRM, NULL, NULL, 0) < 0) {
            tlog("video: kunne ikke oprette DRM-hwdevice");
            return false;
        }
    } else if (fmt & VIDEO_FORMAT_MASK_H264) {
        codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    } else {
        tlog("video: format 0x%x understøttes ikke", fmt);
        return false;
    }
    if (!codec) { tlog("video: dekoder mangler"); return false; }
    V.cc = avcodec_alloc_context3(codec);
    V.cc->width = w;
    V.cc->height = h;
    V.cc->get_format = get_format;
    V.cc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    V.cc->thread_count = 1; /* frame threads = extra frames of latency */
    V.cc->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    if (V.hw) V.cc->hw_device_ctx = av_buffer_ref(V.hw);
    int r = avcodec_open2(V.cc, codec, NULL);
    if (r < 0) {
        tlog("video: avcodec_open2(%s) fejlede: %s", codec->name, av_err2str(r));
        return false;
    }
    V.pkt = av_packet_alloc();
    tlog("video: dekoder %s%s, %dx%d", codec->name, V.hw ? " + v4l2-request" : "", w, h);
    return true;
}

static int dr_setup(int fmt, int w, int h, int rate, void *ctx, int flags) {
    tlog("video: setup format 0x%x %dx%d@%d", fmt, w, h, rate);
    V.fmt = fmt; V.sw = w; V.sh = h;
    if (!drm_open() || !decoder_open(fmt, w, h)) return -1;
    V.evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    V.quit = false;
    if (pthread_create(&V.th, NULL, display_thread, NULL) == 0) V.th_on = true;
    return 0;
}

static void dr_start(void) {}

static void dr_stop(void) {
    V.quit = true;
    if (V.th_on) pthread_join(V.th, NULL);
    V.th_on = false;
}

static void dr_cleanup(void) {
    dr_stop();
    pthread_mutex_lock(&V.mu);
    shown_free(&V.next);
    shown_free(&V.queued);
    shown_free(&V.onscreen);
    pthread_mutex_unlock(&V.mu);
    decoder_close();
    if (V.evfd >= 0) close(V.evfd);
    V.evfd = -1;
    overlay_quit();
    if (V.mode_blob) drmModeDestroyPropertyBlob(V.fd, V.mode_blob);
    V.mode_blob = 0;
    if (V.fd >= 0) close(V.fd); /* the FBs are removed, the plane is turned off; tvshell takes the display back */
    V.fd = -1;
    V.plane = 0;
}

static void hand_off(AVFrame *f) {
    int idx = (int)(f->pts & 255);
    if (f->pts != AV_NOPTS_VALUE && V.t_submit[idx]) V.sum_dec_ms += (LiGetMicroseconds() - V.t_submit[idx]) / 1000.0;
    V.n_dec++;
    pthread_mutex_lock(&V.mu);
    if (V.next.frame) { shown_free(&V.next); V.n_skip++; } /* the display did not reach the previous one: skip */
    V.next.frame = f;
    V.next.recv_us = f->pts != AV_NOPTS_VALUE ? V.t_recv[idx] : 0;
    pthread_mutex_unlock(&V.mu);
    uint64_t one = 1;
    if (write(V.evfd, &one, sizeof one) < 0) { /* nothing */ }
}

static int drain(void) {
    for (;;) {
        AVFrame *f = av_frame_alloc();
        int r = avcodec_receive_frame(V.cc, f);
        if (r < 0) {
            av_frame_free(&f);
            return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : r;
        }
        if (f->format != AV_PIX_FMT_DRM_PRIME) { av_frame_free(&f); continue; }
        hand_off(f);
    }
}

static int dr_submit(PDECODE_UNIT du) {
    static bool rt;
    if (!rt) { rt = true; set_rt(10); } /* directly from the receive thread: keep it ahead of everything else */
    V.n_rx++;
    V.bytes += (uint64_t)du->fullLength;
    if (du->frameHostProcessingLatency) { V.sum_host_ms += du->frameHostProcessingLatency / 10.0; V.n_host++; }
    size_t need = (size_t)du->fullLength + AV_INPUT_BUFFER_PADDING_SIZE;
    if (need > V.cap) {
        free(V.buf);
        V.cap = need * 2;
        V.buf = malloc(V.cap);
    }
    size_t off = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) {
        memcpy(V.buf + off, e->data, (size_t)e->length);
        off += (size_t)e->length;
    }
    memset(V.buf + off, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    int idx = du->frameNumber & 255;
    V.t_recv[idx] = du->receiveTimeUs;
    V.t_submit[idx] = LiGetMicroseconds();
    V.pkt->data = V.buf;
    V.pkt->size = (int)off;
    V.pkt->pts = du->frameNumber;
    V.pkt->flags = du->frameType == FRAME_TYPE_IDR ? AV_PKT_FLAG_KEY : 0;
    int r;
    while ((r = avcodec_send_packet(V.cc, V.pkt)) == AVERROR(EAGAIN)) {
        /* the decoder's queue is full (v4l2m2m): pull frames out and try again */
        if (drain() < 0) break;
        usleep(1000);
    }
    if (r < 0) {
        V.n_err++;
        static int errs;
        if (errs++ < 10) tlog("video: send_packet: %s - beder om nyt nøglebillede", av_err2str(r));
        return DR_NEED_IDR;
    }
    if (drain() < 0) { V.n_err++; return DR_NEED_IDR; }
    return DR_OK;
}

void video_stats(char *out, size_t n, VStat *st) {
    pthread_mutex_lock(&V.mu);
    VStat v = {
        .rx = V.n_rx, .shown = V.n_shown, .skip = V.n_skip, .err = V.n_err,
        .dec_ms = V.n_dec ? V.sum_dec_ms / V.n_dec : 0,
        .total_ms = V.n_shown ? V.sum_total_ms / V.n_shown : 0, .max_ms = V.max_total_ms,
        .host_ms = V.n_host ? V.sum_host_ms / V.n_host : 0,
        .mbit = V.bytes * 8 / 1e6, .w = V.sw, .h = V.sh, .hz = (int)V.mode.vrefresh,
        .disp_h = V.mode.vdisplay, .hevc = (V.fmt & VIDEO_FORMAT_MASK_H265) != 0, .hdr = V.hdr_cur,
    };
    snprintf(out, n, "video ind %d dek %d vist %d sprunget %d fejl %d | dekod %.1f ms | modt->skærm %.1f ms (maks %.1f) | vært %.1f ms | %.1f Mbit/s",
             V.n_rx, V.n_dec, V.n_shown, V.n_skip, V.n_err, v.dec_ms, v.total_ms, v.max_ms, v.host_ms, v.mbit);
    V.n_rx = V.n_dec = V.n_shown = V.n_skip = V.n_err = V.n_host = 0;
    V.bytes = 0;
    V.sum_dec_ms = V.sum_total_ms = V.max_total_ms = V.sum_host_ms = 0;
    pthread_mutex_unlock(&V.mu);
    if (st) *st = v;
}

DECODER_RENDERER_CALLBACKS video_callbacks = {
    .setup = dr_setup,
    .start = dr_start,
    .stop = dr_stop,
    .cleanup = dr_cleanup,
    .submitDecodeUnit = dr_submit,
    .capabilities = CAPABILITY_DIRECT_SUBMIT, /* no extra queue/thread between network and decoder */
};
