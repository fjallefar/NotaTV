/* NotaTV shell - DRM helpers: HDR signaling of the menu (SDR or "always HDR") */
#include "tv.h"
#include <errno.h>
#include <fcntl.h>
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
