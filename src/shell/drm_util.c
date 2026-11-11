/* NotaTV shell - DRM helpers: HDR signaling of the menu (SDR or "always HDR") */
#include "tv.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>

static void reset_card(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { close(fd); return; }
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeObjectProperties *props = drmModeObjectGetProperties(fd, res->connectors[i], DRM_MODE_OBJECT_CONNECTOR);
        if (!props) continue;
        for (uint32_t k = 0; k < props->count_props; k++) {
            drmModePropertyRes *p = drmModeGetProperty(fd, props->props[k]);
            if (!p) continue;
            if (!strcmp(p->name, "HDR_OUTPUT_METADATA") && props->prop_values[k] != 0) {
                int r = drmModeConnectorSetProperty(fd, res->connectors[i], p->prop_id, 0);
                SDL_Log("drm: HDR-metadata nulstillet på connector %u (%d)", res->connectors[i], r);
            } else if (!strcmp(p->name, "Colorspace") && props->prop_values[k] != 0) {
                drmModeConnectorSetProperty(fd, res->connectors[i], p->prop_id, 0);
            }
            drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(props);
    }
    drmModeFreeResources(res);
    close(fd);
}

void drm_reset_hdr(void) {
    char path[32];
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        if (access(path, F_OK) == 0) reset_card(path);
    }
}

/* find a property by name; enum values are looked up by name (enum_name != NULL) */
static uint32_t find_prop(int fd, uint32_t obj, uint32_t type, const char *name, const char *enum_name, uint64_t *val) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t k = 0; props && k < props->count_props; k++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[k]);
        if (!p) continue;
        if (!strcmp(p->name, name)) {
            id = p->prop_id;
            if (enum_name)
                for (int e = 0; e < p->count_enums; e++)
                    if (!strcmp(p->enums[e].name, enum_name)) *val = p->enums[e].value;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static void ui_hdr_card(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { close(fd); return; }
    /* an atomic commit needs master: normally the first open gets it automatically, but if another
       process (e.g. an mpv that has not closed yet) holds the card, the error must show up in the log */
    if (res->count_connectors > 0 && drmSetMaster(fd) != 0)
        SDL_Log("drm: drmSetMaster på %s: %s", path, strerror(errno));
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        bool on = c && c->connection == DRM_MODE_CONNECTED;
        drmModeFreeConnector(c);
        if (!on) continue;
        /* static HDR10 data: the same fixed values as the Apple TV and as mpv sends for movies
           (see NOTATV_HDR_STATIC in player.c) - so nothing changes for the TV between the UI and HDR movies */
        struct hdr_output_metadata md = {0};
        md.hdmi_metadata_type1.eotf = 2; /* SMPTE ST 2084 (PQ) */
        /* BT.2020 primaries and D65 in units of 0.00002 (same order as mpv: R, G, B).
           With zero primaries e.g. LG TVs ignore the HDR signal. */
        static const uint16_t prim[4][2] = {{35400, 14600}, {8500, 39850}, {6550, 2300}, {15635, 16450}};
        for (int k = 0; k < 3; k++) {
            md.hdmi_metadata_type1.display_primaries[k].x = prim[k][0];
            md.hdmi_metadata_type1.display_primaries[k].y = prim[k][1];
        }
        md.hdmi_metadata_type1.white_point.x = prim[3][0];
        md.hdmi_metadata_type1.white_point.y = prim[3][1];
        md.hdmi_metadata_type1.max_display_mastering_luminance = 1000;
        md.hdmi_metadata_type1.min_display_mastering_luminance = 50; /* 0,005 nits */
        md.hdmi_metadata_type1.max_cll = 4000;
        md.hdmi_metadata_type1.max_fall = 1000;
        uint32_t blob = 0;
        if (drmModeCreatePropertyBlob(fd, &md, sizeof md, &blob) != 0) continue;
        /* one atomic commit with ALLOW_MODESET: only a full modeset makes the HDMI hardware
           send the new HDR infoframes (individual properties only update the kernel's state) */
        uint32_t conn = res->connectors[i];
        uint64_t cs = 0, bpc = 12;
        uint32_t p_cs = find_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "Colorspace", "BT2020_RGB", &cs);
        uint32_t p_hdr = find_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA", NULL, NULL);
        uint32_t p_bpc = find_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "max bpc", NULL, NULL);
        /* if the TV is already in exactly this HDR signal (e.g. after an HDR movie), do nothing:
           any change here causes a modeset and thus a black screen on the TV */
        uint64_t cur_cs = 0, cur_hdr = 0;
        drmModeObjectProperties *cp = drmModeObjectGetProperties(fd, conn, DRM_MODE_OBJECT_CONNECTOR);
        for (uint32_t k = 0; cp && k < cp->count_props; k++) {
            if (cp->props[k] == p_cs) cur_cs = cp->prop_values[k];
            if (cp->props[k] == p_hdr) cur_hdr = cp->prop_values[k];
        }
        drmModeFreeObjectProperties(cp);
        bool same = false, cur_pq = false;
        if (cur_cs == cs && cur_hdr) {
            drmModePropertyBlobRes *b = drmModeGetPropertyBlob(fd, (uint32_t)cur_hdr);
            same = b && b->length == sizeof md && !memcmp(b->data, &md, sizeof md);
            cur_pq = b && b->length == sizeof md && ((struct hdr_output_metadata *)b->data)->hdmi_metadata_type1.eotf == 2;
            drmModeFreePropertyBlob(b);
        }
        if (same) {
            SDL_Log("drm: TV'et står allerede i HDR10 - ingen ændring");
            drmModeDestroyPropertyBlob(fd, blob);
            continue;
        }
        /* the same color space without HDR (e.g. tvshell restarted after an SDR mode) gives no modeset
           and thus no new infoframes in the hardware - in that case reset first. If the TV is already in
           HDR (PQ) with different light data, they are just set without a modeset (no black screen). */
        drmModeAtomicReq *rq;
        if (cur_cs == cs && !cur_pq) {
            uint64_t cs_def = 0;
            find_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "Colorspace", "Default", &cs_def);
            rq = drmModeAtomicAlloc();
            if (p_cs) drmModeAtomicAddProperty(rq, conn, p_cs, cs_def);
            if (p_hdr) drmModeAtomicAddProperty(rq, conn, p_hdr, 0);
            drmModeAtomicCommit(fd, rq, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
            drmModeAtomicFree(rq);
        }
        rq = drmModeAtomicAlloc();
        if (p_bpc) drmModeAtomicAddProperty(rq, conn, p_bpc, bpc);
        if (p_cs) drmModeAtomicAddProperty(rq, conn, p_cs, cs);
        if (p_hdr) drmModeAtomicAddProperty(rq, conn, p_hdr, blob);
        int r = drmModeAtomicCommit(fd, rq, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        SDL_Log("drm: HDR10 til UI på connector %u (%d%s%s)", conn, r, r ? ": " : "", r ? strerror(-r) : "");
        drmModeAtomicFree(rq);
        drmModeDestroyPropertyBlob(fd, blob);
    }
    drmModeFreeResources(res);
    close(fd);
}

void drm_set_ui_hdr(bool on) {
    if (!on) { drm_reset_hdr(); return; }
    char path[32];
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        if (access(path, F_OK) == 0) ui_hdr_card(path);
    }
}

/* ---------- what can the connected display do? (read from its EDID) ----------
   HDR: a CTA-861 "HDR Static Metadata" block (extended tag 6) with PQ or HLG. Without it (e.g. an ordinary SDR
   monitor) an HDR signal gives wrong colors.
   Audio: the Short Audio Descriptors (tag 1) list the formats the display accepts (AC-3, DTS, TrueHD ...).
   If the EDID cannot be read, everything is assumed to work, so TVs that hide their EDID lose nothing. */
typedef struct { bool hdr; unsigned audio; } EdidCaps; /* audio: bit n = CTA audio format code n */

static void edid_caps(const uint8_t *e, size_t len, EdidCaps *c) {
    c->hdr = false;
    c->audio = 0;
    for (size_t b = 128; b + 128 <= len; b += 128) {
        const uint8_t *x = e + b;
        if (x[0] != 0x02) continue; /* CTA-861 extension */
        int end = x[2];
        if (end < 4 || end > 127) continue;
        for (int i = 4; i < end;) {
            int tag = x[i] >> 5, n = x[i] & 31;
            if (i + 1 + n > 128) break;
            if (tag == 7 && n >= 3 && x[i + 1] == 6 && (x[i + 2] & 0x0C)) c->hdr = true; /* PQ (ST 2084) or HLG */
            if (tag == 1)
                for (int k = 0; k + 3 <= n; k += 3) c->audio |= 1u << ((x[i + 1 + k] >> 3) & 15);
            i += 1 + n;
        }
    }
}

/* reads the EDID of a connected connector (sysfs dir); false if not connected or no EDID */
static bool connector_caps(const char *dir, EdidCaps *c) {
    char p[300], st[16] = "";
    snprintf(p, sizeof p, "%s/status", dir);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    if (!fgets(st, sizeof st, f)) st[0] = 0;
    fclose(f);
    if (strncmp(st, "connected", 9)) return false;
    snprintf(p, sizeof p, "%s/edid", dir);
    f = fopen(p, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 128) return false;
    edid_caps(buf, n, c);
    return true;
}

/* capabilities of the connected display, cached for 3 s; returns false if it is unknown.
   HDMI connectors first, then the rest. */
static bool display_caps(EdidCaps *out) {
    static EdidCaps cached;
    static bool known;
    static Uint64 at;
    Uint64 now = SDL_GetTicks();
    if (at && now - at < 3000) { *out = cached; return known; }
    at = now ? now : 1;
    bool found = false;
    EdidCaps nc = {0};
    static const char *const pats[] = {"/sys/class/drm/card*-HDMI-*", "/sys/class/drm/card*-DP-*", "/sys/class/drm/card*-*"};
    for (size_t i = 0; i < SDL_arraysize(pats) && !found; i++) {
        glob_t g;
        if (glob(pats[i], 0, NULL, &g) != 0) continue;
        for (size_t k = 0; k < g.gl_pathc && !found; k++)
            if (!strstr(g.gl_pathv[k], "Writeback")) found = connector_caps(g.gl_pathv[k], &nc);
        globfree(&g);
    }
    if (found && (!known || nc.hdr != cached.hdr || nc.audio != cached.audio))
        SDL_Log("drm: skærmen (EDID): HDR=%d lydformater=0x%x", nc.hdr, nc.audio);
    if (found) cached = nc;
    known = found;
    *out = cached;
    return known;
}

bool display_known(void) {
    EdidCaps c;
    return display_caps(&c);
}

bool display_hdr_ok(void) {
    EdidCaps c;
    return !display_caps(&c) || c.hdr;
}

/* does the display accept this audio format? CTA codes: 2 AC-3, 7 DTS, 10 E-AC-3, 11 DTS-HD, 12 TrueHD (MAT) */
bool display_audio_ok(DisplayAudio a) {
    EdidCaps c;
    if (!display_caps(&c)) return true;
    switch (a) {
    case DA_AC3: return c.audio & (1u << 2);
    case DA_EAC3: return c.audio & (1u << 10);
    case DA_DTS: return c.audio & ((1u << 7) | (1u << 11)); /* a DTS-HD receiver also takes plain DTS */
    case DA_DTSHD: return c.audio & (1u << 11);
    case DA_TRUEHD: return c.audio & (1u << 12);
    case DA_HD: return c.audio & ((1u << 11) | (1u << 12));
    }
    return true;
}

/* an HDR setting counts only if the display can do HDR */
bool hdr_cfg(const char *key, int def) { return cfg_int(key, def) != 0 && display_hdr_ok(); }
/* a passthrough setting counts only if the display accepts the format */
bool audio_cfg(const char *key, int def, DisplayAudio a) { return cfg_int(key, def) != 0 && display_audio_ok(a); }

/* ---------- which display modes does the connected display have? ----------
   The frame rate match used to guess a mode ("1920x1080@23.976", then "@24") and let mpv fail if the display did not
   have it (about 1 s extra start-up each time, e.g. the Dell has no 1080p@24). Now the modes are read first and mpv
   is only asked for one that exists. Rate as mpv computes it (clock / htotal / vtotal, x2 when interlaced), and mpv
   compares rates rounded to 1/100 Hz - so "%.2f" of the same number always matches. */
static double mode_hz(const drmModeModeInfo *m) {
    if (!m->htotal || !m->vtotal) return 0;
    double hz = m->clock * 1000.0 / m->htotal / m->vtotal;
    return (m->flags & DRM_MODE_FLAG_INTERLACE) ? hz * 2 : hz;
}

double display_mode_hz(const drmModeModeInfo *m) { return mode_hz(m); }

/* the mode the menus run in. A 4K TV runs 4K at 60 Hz when this input offers it; otherwise the display's own
   preferred mode if that is 50/60 Hz, otherwise its largest 60 (then 50) Hz mode - never 24/30 Hz for the menus (a 4K TV
   whose input only offers 4K30 gets 1080p60). Only when the display has nothing at 50/60 Hz at all is its preferred
   mode used anyway, so there is always a picture. *no_4k60: the display has 4K, but not at 60 Hz on this input.
   false = no modes (no display). */
static bool hz_near(const drmModeModeInfo *m, double hz) {
    return !(m->flags & DRM_MODE_FLAG_INTERLACE) && fabs(mode_hz(m) - hz) < 0.6;
}

static const drmModeModeInfo *largest_at(const drmModeConnector *c, double hz, int only_w, int only_h) {
    const drmModeModeInfo *best = NULL;
    for (int i = 0; i < c->count_modes; i++) {
        const drmModeModeInfo *m = &c->modes[i];
        if (!hz_near(m, hz) || (only_w && (m->hdisplay != only_w || m->vdisplay != only_h))) continue;
        if (!best) { best = m; continue; }
        long a = (long)m->hdisplay * m->vdisplay, b = (long)best->hdisplay * best->vdisplay;
        bool mp = m->type & DRM_MODE_TYPE_PREFERRED, bp = best->type & DRM_MODE_TYPE_PREFERRED;
        /* the same size: the TV's preferred one, then the rate closest to exactly hz (60.00 before 59.94) */
        if (a > b || (a == b && (mp > bp || (mp == bp && fabs(mode_hz(m) - hz) < fabs(mode_hz(best) - hz))))) best = m;
    }
    return best;
}

bool display_pick_mode(const drmModeConnector *c, drmModeModeInfo *out, bool *no_4k60) {
    *no_4k60 = false;
    if (!c || c->count_modes <= 0) return false;
    const drmModeModeInfo *pref = &c->modes[0];
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { pref = &c->modes[i]; break; }
    const drmModeModeInfo *m = largest_at(c, 60, 3840, 2160);
    if (!m && (hz_near(pref, 60) || hz_near(pref, 50))) m = pref;
    if (!m) m = largest_at(c, 60, 0, 0);
    if (!m) m = largest_at(c, 50, 0, 0);
    if (!m) m = pref;
    *out = *m;
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].hdisplay >= 3840 && m->hdisplay < 3840) *no_4k60 = true;
    return true;
}

/* how well a mode rate fits the wanted rate; lower is better, < 0 = not usable.
   0: same rate. 1: an exact multiple (e.g. 47.952 for 23.976 - no judder). 2: almost the same (24 for 23.976 -
   one repeated frame every ~40 s, still better than 60 Hz) */
static int rate_fit(double hz, double want) {
    if (hz <= 0 || want <= 0) return -1;
    if (fabs(hz - want) < 0.005) return 0;
    double k = round(hz / want);
    if (k >= 2 && k <= 5 && fabs(hz - want * k) < 0.005 * k) return 1;
    if (fabs(hz - want) < 0.06) return 2;
    return -1;
}

bool display_mode_for(int w, int h, double want, char *out, size_t n) {
    int best_fit = 99;
    double best_hz = 0;
    for (int card = 0; card < 4; card++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", card);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *res = drmModeGetResources(fd);
        for (int i = 0; res && i < res->count_connectors; i++) {
            /* "Current": no new probe of the display (a probe re-reads the EDID and can take a while) */
            drmModeConnector *c = drmModeGetConnectorCurrent(fd, res->connectors[i]);
            if (!c) continue;
            if (c->connection == DRM_MODE_CONNECTED && c->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
                for (int m = 0; m < c->count_modes; m++) {
                    const drmModeModeInfo *mi = &c->modes[m];
                    if (mi->hdisplay != w || mi->vdisplay != h || (mi->flags & DRM_MODE_FLAG_INTERLACE)) continue;
                    double hz = mode_hz(mi);
                    int fit = rate_fit(hz, want);
                    /* the same fit: the lower rate (a 2x multiple before 5x) */
                    if (fit >= 0 && (fit < best_fit || (fit == best_fit && hz < best_hz))) {
                        best_fit = fit;
                        best_hz = hz;
                    }
                }
            drmModeFreeConnector(c);
        }
        drmModeFreeResources(res);
        close(fd);
    }
    if (best_fit == 99) {
        SDL_Log("drm: skærmen har ingen %dx%d-tilstand, der passer til %.3f Hz - beholder den nuværende", w, h, want);
        return false;
    }
    snprintf(out, n, "%dx%d@%.2f", w, h, best_hz);
    SDL_Log("drm: %.3f Hz ønsket -> tilstand %s", want, out);
    return true;
}
