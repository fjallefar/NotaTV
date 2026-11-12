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

/* 4K at 60/50 Hz as YCbCr 4:2:0 in the EDID: VIC 97/96 in a Y420 Video Data Block (4:2:0 only), or in the SVD list and
   marked by a Y420 Capability Map. Bit 0 = 4K60, bit 1 = 4K50. */
static unsigned svd_vic(uint8_t v) { return v >= 129 && v <= 192 ? v & 0x7f : v; }
static unsigned vic_bit(unsigned vic) { return vic == 97 ? 1 : vic == 96 ? 2 : 0; }

static unsigned edid_y420(const uint8_t *e, size_t len) {
    unsigned r = 0;
    for (size_t b = 128; b + 128 <= len; b += 128) {
        const uint8_t *x = e + b;
        if (x[0] != 0x02) continue; /* CTA-861 extension */
        int end = x[2];
        if (end < 4 || end > 127) continue;
        uint8_t svd[64];
        int nsvd = 0;
        for (int i = 4; i < end;) {
            int tag = x[i] >> 5, n = x[i] & 31;
            if (i + 1 + n > end) break;
            if (tag == 2)
                for (int k = 0; k < n && nsvd < 64; k++) svd[nsvd++] = x[i + 1 + k];
            i += 1 + n;
        }
        for (int i = 4; i < end;) {
            int tag = x[i] >> 5, n = x[i] & 31;
            if (i + 1 + n > end) break;
            if (tag == 7 && n >= 1 && x[i + 1] == 14) /* Y420VDB */
                for (int k = 2; k <= n; k++) r |= vic_bit(svd_vic(x[i + k]));
            if (tag == 7 && n >= 1 && x[i + 1] == 15) { /* Y420CMDB: no bitmap = every SVD */
                for (int j = 0; j < nsvd; j++)
                    if (n == 1 || (j / 8 + 2 <= n && (x[i + 2 + j / 8] >> (j % 8)) & 1)) r |= vic_bit(svd_vic(svd[j]));
            }
            i += 1 + n;
        }
    }
    return r;
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

/* HDMI 2.0: above 340 MHz (4K50/60 in RGB) the TV must switch on scrambling when the kernel asks over SCDC. A TV
   that does not (e.g. a TCL that holds hot-plug low on its input while it is on, so its SCDC stays reset) shows
   nothing at all in such a mode - kms_present.c reads the TV's SCDC back and says so in the log. hdmi20=0 in
   notatv.conf keeps the menus and frame rate match at or below 340 MHz: 4K60 then comes as 4:2:0 where the TV
   offers it (297 MHz, see y420_mode), otherwise 1080p60. Films in 4K at 24/25/30 Hz are 297 MHz and stay 4K. */
#define HDMI14_MAX_KHZ 340000
bool display_hdmi20(void) { return cfg_int("hdmi20", 1) != 0; }
bool display_needs_hdmi20(const drmModeModeInfo *m) { return m->clock > HDMI14_MAX_KHZ && !display_is_y420(m); }

static const drmModeModeInfo *largest_at(const drmModeConnector *c, double hz, int only_w, int only_h, bool hdmi20) {
    const drmModeModeInfo *best = NULL;
    for (int i = 0; i < c->count_modes; i++) {
        const drmModeModeInfo *m = &c->modes[i];
        if (!hz_near(m, hz) || (only_w && (m->hdisplay != only_w || m->vdisplay != only_h))) continue;
        if (!hdmi20 && m->clock > HDMI14_MAX_KHZ) continue;
        if (!best) { best = m; continue; }
        long a = (long)m->hdisplay * m->vdisplay, b = (long)best->hdisplay * best->vdisplay;
        bool mp = m->type & DRM_MODE_TYPE_PREFERRED, bp = best->type & DRM_MODE_TYPE_PREFERRED;
        /* the same size: the TV's preferred one, then the rate closest to exactly hz (60.00 before 59.94) */
        if (a > b || (a == b && (mp > bp || (mp == bp && fabs(mode_hz(m) - hz) < fabs(mode_hz(best) - hz))))) best = m;
    }
    return best;
}

/* 4K60 (or 4K50) sent as YCbCr 4:2:0: the Pi's HDMI block has no 4:2:0 encoder, but 4:2:0 at 4K60 is on the cable
   exactly a 1920x2160 timing at 297 MHz where every "pixel" carries two Y values and Cb or Cr (every other line).
   Our kernel patch (0004) signals that mode as 4:2:0 VIC 97/96 and sends the pixels untouched, and kms_present.c packs
   the picture. Used for TVs that only offer 4K60 as 4:2:0 on the input (e.g. an HDMI port in "standard" mode), and
   for the Pi 4's second HDMI port (297 MHz is within its limit). The CEA timings with every horizontal value halved. */
static void y420_mode(drmModeModeInfo *m, bool hz50) {
    memset(m, 0, sizeof *m);
    m->clock = 297000;
    m->hdisplay = 1920;
    m->hsync_start = hz50 ? 2448 : 2008;
    m->hsync_end = hz50 ? 2492 : 2052;
    m->htotal = hz50 ? 2640 : 2200;
    m->vdisplay = 2160;
    m->vsync_start = 2168;
    m->vsync_end = 2178;
    m->vtotal = 2250;
    m->vrefresh = hz50 ? 50 : 60;
    m->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    m->type = DRM_MODE_TYPE_USERDEF;
    snprintf(m->name, sizeof m->name, "3840x2160 (4:2:0)");
}

/* does the TV take 4K60/50 as 4:2:0 (bits as edid_y420), and does the kernel have patch 0004 switched on?
   g_y420_id: a checksum of that TV's EDID - a TV where the picture did not come (y420_bad) does not get it again */
static uint32_t g_y420_id;
uint32_t display_y420_id(void) { return g_y420_id; }

static unsigned y420_caps(int fd, uint32_t conn) {
    if (fd < 0 || !cfg_int("y420", 1)) return 0;
    FILE *f = fopen("/sys/module/vc4/parameters/notatv_y420", "r");
    int on = f ? fgetc(f) : 0;
    if (f) fclose(f);
    if (on != 'Y') return 0;
    unsigned r = 0;
    drmModeObjectProperties *pr = drmModeObjectGetProperties(fd, conn, DRM_MODE_OBJECT_CONNECTOR);
    for (uint32_t k = 0; pr && k < pr->count_props; k++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, pr->props[k]);
        if (p && !strcmp(p->name, "EDID") && pr->prop_values[k]) {
            drmModePropertyBlobRes *b = drmModeGetPropertyBlob(fd, (uint32_t)pr->prop_values[k]);
            if (b) {
                r = edid_y420(b->data, b->length);
                uint32_t h = 2166136261u;
                for (uint32_t i = 0; i < b->length; i++) h = (h ^ ((const uint8_t *)b->data)[i]) * 16777619u;
                g_y420_id = h;
                char id[16];
                snprintf(id, sizeof id, "%08x", h);
                if (r && !strcmp(cfg_get("y420_bad", ""), id)) {
                    SDL_Log("drm: 4K60 som 4:2:0 blev fravalgt på dette tv - bruges ikke");
                    r = 0;
                }
            }
            drmModeFreePropertyBlob(b);
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(pr);
    return r;
}

bool display_is_y420(const drmModeModeInfo *m) { return m->hdisplay == 1920 && m->vdisplay == 2160 && m->clock == 297000; }

bool display_pick_mode(int fd, const drmModeConnector *c, drmModeModeInfo *out, bool *no_4k60) {
    *no_4k60 = false;
    if (!c || c->count_modes <= 0) return false;
    const drmModeModeInfo *pref = &c->modes[0];
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { pref = &c->modes[i]; break; }
    bool hdmi20 = display_hdmi20();
    const drmModeModeInfo *m = largest_at(c, 60, 3840, 2160, hdmi20);
    if (!m && (hz_near(pref, 60) || hz_near(pref, 50)) && (hdmi20 || pref->clock <= HDMI14_MAX_KHZ)) m = pref;
    if (!m) m = largest_at(c, 60, 0, 0, hdmi20);
    if (!m) m = largest_at(c, 50, 0, 0, hdmi20);
    if (!m) m = pref;
    *out = *m;
    bool uhd = m->hdisplay >= 3840 && (hz_near(m, 60) || hz_near(m, 50));
    if (!uhd) {
        unsigned y = y420_caps(fd, c->connector_id);
        if (y) {
            y420_mode(out, !(y & 1));
            return true;
        }
    }
    for (int i = 0; hdmi20 && i < c->count_modes; i++) /* hdmi20=0: chosen by the user, no hint about the TV's input */
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
    bool hdmi20 = display_hdmi20();
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
                    if (!hdmi20 && mi->clock > HDMI14_MAX_KHZ) continue;
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
