/* NotaTV app package "tesla" (Tesla) - shared between the package's own files */
#ifndef TESLA_H
#define TESLA_H
#include "tv.h"

#define TM_MAX_CARS 4
#define TM_MAX_KV 96

/* one car's values, exactly as TeslaMate publishes them: teslamate/[namespace/]cars/<id>/<key> = <value> */
typedef struct {
    int id;
    int n;
    struct { char k[40], v[400]; } kv[TM_MAX_KV]; /* v: active_route is a small JSON object */
} TmCar;

typedef enum { TM_OFF, TM_CONNECTING, TM_LIVE, TM_AUTH, TM_UNREACHABLE, TM_NODATA } TmState;

/* mqtt.c: the connection to the MQTT server TeslaMate publishes to (runs in its own thread) */
void tm_conn_apply(void);           /* start/stop/restart after the server or the install state changed */
TmState tm_state(void);
int tm_cars(TmCar *out, int max);   /* a copy of what we know (sorted by car id) */
void tm_conn_forget(void);          /* stop and drop every value from memory */
const char *tm_get(const TmCar *c, const char *key); /* "" when unknown */

/* discovery: MQTT servers (port 1883) on our own subnet that carry TeslaMate topics */
typedef enum { TD_TESLAMATE, TD_LOGIN, TD_NOTM, TD_WEBONLY } TdKind;
typedef struct { char addr[64]; int port; TdKind kind; char cars[96]; } TmFound;
typedef void (*TmDiscDone)(void *ud, const TmFound *f, int n);
void tm_discover(TmDiscDone cb, void *ud);

/* try one server: 0 = TeslaMate data, 1 = login needed/refused, 2 = no TeslaMate data, 3 = unreachable */
typedef void (*TmProbeDone)(void *ud, int result, const char *cars);
void tm_probe(const char *host, int port, const char *user, const char *pass, TmProbeDone cb, void *ud);

/* view.c: formatting shared by the screens */
const char *tm_ctx(const char *t); /* "door|Open" -> "Open": a context against the system's own translation */
bool tm_true(const char *v);
bool tm_has(const char *v);
const char *tm_num(char *buf, size_t n, const char *v, int d);
const char *tm_num_unit(char *buf, size_t n, const char *v, int d, const char *unit);
const char *tm_since(char *buf, size_t n, const char *iso);
const char *tm_state_text(const TmCar *c);
Color tm_state_color(const TmCar *c);
const char *tm_car_name(const TmCar *c);
const char *tm_range(const TmCar *c);
Color tm_paint(const TmCar *c);
void tm_battery_bar(float x, float y, float w, float h, float pct, float limit, bool charging, float a);

/* parts of the car view that can be turned off in the side menu (cfg tesla_f_<key>) */
enum { TF_CHARGING, TF_CURRENT, TF_CLIMATE, TF_TYRES, TF_SECURITY, TF_TOPVIEW, TF_WHERE, TF_DRIVE, TF_ROUTE, TF_MAP, TF_CAR,
       TF_DISPLAY, TF_USABLE, TF_COUNT };
bool tm_feat(int f);
void tm_feat_forget(void);

/* the cars at the top of the home screen (cfg tesla_home) */
#define TM_HOME_MAX 3
int tm_home_cars(const TmCar *cars, int n, const TmCar **out);
void tm_home_overlay(void); /* app_set_overlay: draws only on the home screen */

/* the car view */
typedef struct {
    SideMenu side;
    int page, car;
    int it_feats, it_home, it_server; /* side menu items on the main page */
    float scroll, scroll_to, content_h;
    double opened;
} TView;
enum { VIEW_SERVER = 1, VIEW_CLOSE = 2 };
void view_init(TView *v);
void view_update(TView *v, float dt);
void view_draw(TView *v);
int view_input(TView *v, const InputEvent *ev); /* VIEW_SERVER / VIEW_CLOSE / 0; -1 = not used */

Screen *tesla_screen(void);

#endif
