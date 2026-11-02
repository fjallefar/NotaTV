/* NotaTV app package "tesla" - Tesla: the car's battery, charging and status from the user's own TeslaMate.
 * The data comes live over TeslaMate's MQTT server (mqtt.c). The server is found on the network or typed in.
 * Settings: cfg tesla_host / tesla_port / tesla_user / tesla_pass (wiped by tesla_forget). Values are only kept
 * in memory, never on disk. */
#define _GNU_SOURCE
#include "tesla.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXROW 17

enum { M_LIST, M_CAR };

typedef struct {
    int mode, sel;
    bool searching, probing;
    TmFound found[MAXROW];
    int n;
    TView view;
    /* the server being set up (manual entry / login) */
    char host[128], user[128], pass[128];
    int port;
} Tesla;

static Tesla *G;

/* ---------- choosing the server ---------- */

static void open_view(Tesla *g) {
    g->mode = M_CAR;
    g->sel = 0;
    view_init(&g->view);
}

static void discover_cb(void *ud, const TmFound *f, int n) {
    Tesla *g = ud;
    g->searching = false;
    g->n = n < MAXROW - 1 ? n : MAXROW - 1;
    memcpy(g->found, f, g->n * sizeof *f);
    if (g->sel > g->n) g->sel = g->n;
    app_request_redraw();
}

static void search(Tesla *g) {
    if (g->searching) return;
    g->searching = true;
    g->n = 0;
    g->sel = 0;
    tm_discover(discover_cb, g);
}

static void open_list(Tesla *g) {
    g->mode = M_LIST;
    search(g);
}

static void save_server(Tesla *g) {
    cfg_set("tesla_host", g->host);
    cfg_set_int("tesla_port", g->port);
    if (*g->user) cfg_set("tesla_user", g->user); else cfg_unset("tesla_user");
    if (*g->pass) cfg_set("tesla_pass", g->pass); else cfg_unset("tesla_pass");
    memset(g->pass, 0, sizeof g->pass);
    tm_conn_apply();
    open_view(g);
}

static void ask_login(Tesla *g);

static void probe_cb(void *ud, int r, const char *cars) {
    Tesla *g = ud;
    g->probing = false;
    if (r == 0) {
        if (*cars) app_toast(T("Found: %s"), cars);
        save_server(g);
    } else if (r == 1) {
        if (*g->user) app_toast(T("The server refused the username or password"));
        ask_login(g);
    } else if (r == 2) {
        if (*g->user) app_toast(T("Connected, but there is no TeslaMate data on %s"), g->host);
        else ask_login(g); /* the data may only be shown to a user with a login */
    } else app_toast(T("Can't reach %s"), g->host);
    app_request_redraw();
}

static void try_server(Tesla *g) {
    g->probing = true;
    tm_probe(g->host, g->port, g->user, g->pass, probe_cb, g);
}

static void pass_cb(void *ud, const char *text) {
    Tesla *g = ud;
    if (!text) { memset(g->user, 0, sizeof g->user); return; }
    snprintf(g->pass, sizeof g->pass, "%s", text);
    try_server(g);
}

static void user_cb(void *ud, const char *text) {
    Tesla *g = ud;
    if (!text || !*text) { g->user[0] = 0; return; }
    snprintf(g->user, sizeof g->user, "%s", text);
    keyboard_open(T("MQTT password"), "", true, pass_cb, g);
}

static void ask_login(Tesla *g) {
    memset(g->pass, 0, sizeof g->pass);
    keyboard_open(T("MQTT username (from TeslaMate's setup)"), g->user, false, user_cb, g);
}

/* "192.168.1.5", "nas.local:1884", "[fd00::5]:1883" -> host + port */
static bool parse_addr(const char *in, char *host, size_t hn, int *port) {
    char t[160];
    snprintf(t, sizeof t, "%s", in);
    str_trim(t);
    char *p = t;
    if (!strncmp(p, "mqtt://", 7)) p += 7;
    else if (!strncmp(p, "http://", 7)) p += 7;
    char *sl = strchr(p, '/');
    if (sl) *sl = 0;
    *port = 1883;
    if (*p == '[') {
        char *e = strchr(p, ']');
        if (!e) return false;
        *e = 0;
        if (e[1] == ':') *port = atoi(e + 2);
        p++;
    } else {
        char *c = strchr(p, ':');
        if (c && !strchr(c + 1, ':')) { *c = 0; *port = atoi(c + 1); }
    }
    if (*port == 4000) *port = 1883; /* the web page's port: MQTT is on the same machine */
    if (!*p || *port <= 0 || *port > 65535) return false;
    snprintf(host, hn, "%s", p);
    return true;
}

static void manual_cb(void *ud, const char *text) {
    Tesla *g = ud;
    if (!text || !*text) return;
    if (!parse_addr(text, g->host, sizeof g->host, &g->port)) { app_toast(T("That is not an address")); return; }
    g->user[0] = 0;
    memset(g->pass, 0, sizeof g->pass);
    try_server(g);
}

static void webonly_cb(void *ud, bool yes) { (void)ud; (void)yes; }

static void pick(Tesla *g) {
    if (g->sel >= g->n) {
        char init[160] = "";
        const char *h = cfg_get("tesla_host", "");
        if (*h) snprintf(init, sizeof init, "%s", h);
        keyboard_open(T("IP address of the machine running TeslaMate"), init, false, manual_cb, g);
        return;
    }
    TmFound *f = &g->found[g->sel];
    if (f->kind == TD_WEBONLY) {
        static const char *pts[3];
        pts[0] = T("TeslaMate sends the car's data through its MQTT server (mosquitto), and that server must be reachable from the network");
        pts[1] = T("In TeslaMate's docker-compose.yml, add this under mosquitto: ports: - 1883:1883");
        pts[2] = T("Then run docker compose up -d and search again");
        notes_open("TESLA", T("TeslaMate's MQTT can't be reached"), f->addr, pts, 3, T("OK"), "", webonly_cb, g);
        return;
    }
    snprintf(g->host, sizeof g->host, "%s", f->addr);
    g->port = f->port;
    g->user[0] = 0;
    memset(g->pass, 0, sizeof g->pass);
    if (f->kind == TD_TESLAMATE) {
        if (*f->cars) app_toast(T("Found: %s"), f->cars);
        save_server(g);
    } else ask_login(g);
}

static void draw_list(Tesla *g) {
    draw_topbar(T("Tesla - choose TeslaMate"));
    text_draw(F_REG, 24, 96, 118, C_TEXT3, AL_LEFT, T("TeslaMate servers on the network"));
    int rows = g->n + 1;
    for (int i = 0; i < rows; i++) {
        float y = 170 + i * 84;
        bool f = i == g->sel && !g->probing;
        if (f) fill_rrect(96, y, UI_W - 192, 74, 14, C_FOCUS);
        Color tc = f ? RGB(10, 10, 10) : C_TEXT, rc = f ? RGB(60, 60, 60) : C_TEXT3;
        char l[220];
        const char *r = "";
        if (i == g->n) {
            snprintf(l, sizeof l, "%s", T("Enter the address yourself"));
        } else {
            TmFound *x = &g->found[i];
            if (x->kind == TD_TESLAMATE) {
                if (*x->cars) snprintf(l, sizeof l, "%s  (%s)", x->cars, x->addr);
                else snprintf(l, sizeof l, "TeslaMate  (%s)", x->addr);
                r = T("TeslaMate - OK to use");
            } else {
                snprintf(l, sizeof l, "%s", x->addr);
                r = x->kind == TD_LOGIN ? T("Needs a login - OK") : x->kind == TD_NOTM ? T("MQTT, no TeslaMate data seen - OK to log in")
                    : T("TeslaMate, but its MQTT can't be reached - OK for help");
            }
        }
        text_draw_clip(F_SEMI, 28, 146, y + 20, 1000, tc, AL_LEFT, l);
        text_draw_clip(F_REG, 24, UI_W - 130, y + 23, 640, rc, AL_RIGHT, r);
    }
    float y = 170 + rows * 84 + 30;
    if (g->searching) {
        draw_spinner(130, y + 18, 16, C_TEXT2);
        text_draw(F_REG, 24, 160, y + 4, C_TEXT3, AL_LEFT, T("Searching the network…"));
    } else if (g->probing) {
        draw_spinner(130, y + 18, 16, C_TEXT2);
        text_draw(F_REG, 24, 160, y + 4, C_TEXT3, AL_LEFT, T("Connecting…"));
    } else if (!g->n)
        text_draw_wrap(F_REG, 24, 96, y, UI_W - 192, 3, 34, C_TEXT3,
                       T("No TeslaMate found. If it runs on another network or in Docker without an open MQTT port, enter the machine's IP address yourself."));
    draw_hint(96, 990, "Red", T("Search again"));
}

/* ---------- screen ---------- */

static void tesla_enter(Screen *s) {
    Tesla *g = s->st;
    if (*cfg_get("tesla_host", "")) { tm_conn_apply(); open_view(g); }
    else open_list(g);
}

static void tesla_draw(Screen *s) {
    Tesla *g = s->st;
    draw_background();
    if (g->mode == M_LIST) draw_list(g);
    else view_draw(&g->view);
}

static void tesla_update(Screen *s, float dt) {
    Tesla *g = s->st;
    if (g->mode == M_CAR) view_update(&g->view, dt);
    /* the view shows live data and "since" times; the list shows a spinner */
    if (g->mode == M_CAR || g->searching || g->probing) app_request_redraw();
}

static bool tesla_input(Screen *s, const InputEvent *ev) {
    Tesla *g = s->st;
    if (g->probing && ev->a != A_BACK && ev->a != A_HOME) return true;
    if (g->mode == M_CAR) {
        int r = view_input(&g->view, ev);
        if (r == VIEW_SERVER) open_list(g);
        else if (r == VIEW_CLOSE) app_pop();
        return r >= 0;
    }
    switch (ev->a) {
    case A_UP: if (g->sel > 0) g->sel--; return true;
    case A_DOWN: if (g->sel < g->n) g->sel++; return true;
    case A_OK: if (!g->searching || g->sel == g->n) pick(g); return true;
    case A_MENU: search(g); return true;
    case A_BACK: case A_LEFT:
        if (*cfg_get("tesla_host", "")) open_view(g); /* back to the car with the server we already have */
        else app_pop();
        return true;
    default: return false;
    }
}

static void tesla_destroy(Screen *s) { free(s); }

Screen *tesla_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    if (!G) G = calloc(1, sizeof *G); /* lives on: discovery and probes call back into it */
    s->name = "tesla";
    s->st = G;
    s->enter = tesla_enter;
    s->update = tesla_update;
    s->draw = tesla_draw;
    s->input = tesla_input;
    s->destroy = tesla_destroy;
    return s;
}

/* ---------- home screen: the car's battery at the top when Tesla has focus ---------- */

static TmCar shelf_buf[TM_MAX_CARS];

static bool tesla_draw_shelf(float x, float y) {
    int n = tm_cars(shelf_buf, TM_MAX_CARS);
    if (!n || !tm_has(tm_get(&shelf_buf[0], "battery_level"))) return false;
    const TmCar *c = &shelf_buf[0];
    char b1[48], b2[48], t[200];
    text_draw(F_SEMI, 24, x, y + 40, RGB(240, 110, 110), AL_LEFT, "TESLA");
    text_draw_clip(F_BOLD, 60, x, y + 76, 1100, C_TEXT, AL_LEFT, tm_car_name(c));
    snprintf(t, sizeof t, "%s %%  ·  %s  ·  %s", tm_num(b1, sizeof b1, tm_get(c, "battery_level"), 0),
             tm_num_unit(b2, sizeof b2, tm_range(c), 0, "km"), tm_state_text(c));
    text_draw(F_REG, 32, x, y + 160, C_TEXT2, AL_LEFT, t);
    tm_battery_bar(x, y + 218, 620, 18, atof(tm_get(c, "battery_level")), atof(tm_get(c, "charge_limit_soc")),
                   !strcmp(tm_get(c, "state"), "charging"), 1);
    return true;
}

/* ---------- the app (see NotaApp in tv.h) ---------- */

static void tesla_forget(void) {
    tm_conn_forget();
    cfg_unset("tesla_host");
    cfg_unset("tesla_port");
    cfg_unset("tesla_user");
    cfg_unset("tesla_pass");
    tm_feat_forget();
    cfg_unset("tesla_home");
    memset(shelf_buf, 0, sizeof shelf_buf);
    if (G) {
        memset(G->found, 0, sizeof G->found);
        G->n = 0;
        memset(G->host, 0, sizeof G->host);
        memset(G->user, 0, sizeof G->user);
        memset(G->pass, 0, sizeof G->pass);
        G->mode = M_LIST;
    }
}

static void tesla_init(void) {
    tm_conn_apply();
    if (app_set_overlay) app_set_overlay("tesla", tm_home_overlay); /* NotaTV 1.0.5+: the cars at the top of home */
}

static void act_forget(void) { app_ask_signout("Tesla", tesla_forget); }
static const char *no_server(void) { return *cfg_get("tesla_host", "") ? NULL : ITEM_HIDDEN; }
static const char *server_val(void) {
    static char b[160];
    const char *h = cfg_get("tesla_host", "");
    if (!*h) return T("Not chosen");
    int p = cfg_int("tesla_port", 1883);
    if (p == 1883) snprintf(b, sizeof b, "%s", h);
    else snprintf(b, sizeof b, "%s:%d", h, p);
    return b;
}

static const Item tesla_items[] = {
    {N_("TeslaMate server"), IT_INFO, .value = server_val, .help = N_("Choose another one with Red inside the Tesla app")},
    {N_("Forget the TeslaMate server"), IT_ACTION, .action = act_forget, .locked = no_server,
     .help = N_("Removes the address and login, and everything the app knows about the car")},
};

const NotaApp nota_app = {
    NOTA_API, "tesla", "Tesla", N_("Your Tesla on the TV: battery, charging and status from your TeslaMate"), APP_VERSION, 85, APP_DEFAULT_OFF,
    ICON_POWER, {214, 32, 40, 255}, {120, 12, 18, 255}, {255, 255, 255, 255},
    .open = tesla_screen, .init = tesla_init, .apply = tm_conn_apply, .forget = tesla_forget,
    .settings = tesla_items, .n_settings = sizeof tesla_items / sizeof tesla_items[0],
    .shelf_label = "TESLA", .draw_shelf = tesla_draw_shelf,
};
