/* Minimal stand-in for libsystemd's sd-event, only what flutter-pi uses (NotaTV has no systemd).
 * Semantics follow sd-event: sources are dispatched one per iteration, lowest priority value first;
 * time sources are one-shot; defer sources fire every iteration while enabled. */
#ifndef NOTA_SD_EVENT_H
#define NOTA_SD_EVENT_H
#include <inttypes.h> /* sd-event.h brings it in, and flutter-pi relies on that */
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/epoll.h>

typedef struct sd_event sd_event;
typedef struct sd_event_source sd_event_source;
typedef int (*sd_event_handler_t)(sd_event_source *s, void *userdata);
typedef int (*sd_event_io_handler_t)(sd_event_source *s, int fd, uint32_t revents, void *userdata);
typedef int (*sd_event_time_handler_t)(sd_event_source *s, uint64_t usec, void *userdata);

enum { SD_EVENT_OFF = 0, SD_EVENT_ON = 1, SD_EVENT_ONESHOT = -1 };
enum { SD_EVENT_INITIAL, SD_EVENT_ARMED, SD_EVENT_PENDING, SD_EVENT_RUNNING, SD_EVENT_EXITING, SD_EVENT_FINISHED, SD_EVENT_PREPARING };
enum { SD_EVENT_PRIORITY_IMPORTANT = -100, SD_EVENT_PRIORITY_NORMAL = 0, SD_EVENT_PRIORITY_IDLE = 100 };

int sd_event_new(sd_event **ret);
sd_event *sd_event_ref(sd_event *e);
sd_event *sd_event_unref(sd_event *e);
void sd_event_unrefp(sd_event **e);
int sd_event_get_fd(sd_event *e);
int sd_event_get_state(sd_event *e);
int sd_event_prepare(sd_event *e);
int sd_event_wait(sd_event *e, uint64_t usec);
int sd_event_dispatch(sd_event *e);
int sd_event_exit(sd_event *e, int code);

int sd_event_add_io(sd_event *e, sd_event_source **s, int fd, uint32_t events, sd_event_io_handler_t cb, void *userdata);
int sd_event_add_time(sd_event *e, sd_event_source **s, clockid_t clock, uint64_t usec, uint64_t accuracy,
                      sd_event_time_handler_t cb, void *userdata);
int sd_event_add_defer(sd_event *e, sd_event_source **s, sd_event_handler_t cb, void *userdata);

sd_event_source *sd_event_source_ref(sd_event_source *s);
sd_event_source *sd_event_source_unref(sd_event_source *s);
void sd_event_source_unrefp(sd_event_source **s);
sd_event_source *sd_event_source_disable_unref(sd_event_source *s);
int sd_event_source_set_enabled(sd_event_source *s, int enabled);
int sd_event_source_set_priority(sd_event_source *s, int64_t priority);
int sd_event_source_set_floating(sd_event_source *s, int b);
#endif
