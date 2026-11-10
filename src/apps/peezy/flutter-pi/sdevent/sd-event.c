/* Minimal sd-event for flutter-pi on NotaTV (see systemd/sd-event.h). One epoll fd holds the io sources, a timerfd
 * for the earliest time source and an eventfd that is poked when a defer source is added from another thread.
 * All calls take one recursive mutex, so sources can be added from any thread and from inside callbacks. */
#define _GNU_SOURCE
#include "systemd/sd-event.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

enum kind { K_IO, K_TIME, K_DEFER };

struct sd_event_source {
	sd_event *e;
	enum kind kind;
	int refs, enabled, floating, pending, dead;
	int64_t prio;
	uint64_t seq;          /* insertion order: ties in priority go first come, first served */
	int fd;
	uint32_t events, revents;
	uint64_t usec;
	void *cb, *userdata;
	sd_event_source *next;
};

struct sd_event {
	int refs, state, exit_req;
	int epfd, tfd, efd;
	uint64_t seq;
	pthread_mutex_t mu;
	sd_event_source *list;
};

static void lock(sd_event *e) { pthread_mutex_lock(&e->mu); }
static void unlock(sd_event *e) { pthread_mutex_unlock(&e->mu); }

static uint64_t now_usec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int sd_event_new(sd_event **ret) {
	sd_event *e = calloc(1, sizeof *e);
	if (!e) return -ENOMEM;
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&e->mu, &a);
	pthread_mutexattr_destroy(&a);
	e->refs = 1;
	e->epfd = epoll_create1(EPOLL_CLOEXEC);
	e->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	e->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (e->epfd < 0 || e->tfd < 0 || e->efd < 0) {
		int err = -errno;
		if (e->epfd >= 0) close(e->epfd);
		if (e->tfd >= 0) close(e->tfd);
		if (e->efd >= 0) close(e->efd);
		free(e);
		return err;
	}
	struct epoll_event ev = { .events = EPOLLIN, .data.ptr = &e->tfd };
	epoll_ctl(e->epfd, EPOLL_CTL_ADD, e->tfd, &ev);
	ev.data.ptr = &e->efd;
	epoll_ctl(e->epfd, EPOLL_CTL_ADD, e->efd, &ev);
	*ret = e;
	return 0;
}

static void source_free(sd_event_source *s) {
	sd_event *e = s->e;
	for (sd_event_source **p = &e->list; *p; p = &(*p)->next)
		if (*p == s) { *p = s->next; break; }
	if (s->kind == K_IO) epoll_ctl(e->epfd, EPOLL_CTL_DEL, s->fd, NULL);
	free(s);
}

sd_event *sd_event_ref(sd_event *e) { if (e) { lock(e); e->refs++; unlock(e); } return e; }

sd_event *sd_event_unref(sd_event *e) {
	if (!e) return NULL;
	lock(e);
	if (--e->refs > 0) { unlock(e); return NULL; }
	while (e->list) source_free(e->list);
	close(e->epfd); close(e->tfd); close(e->efd);
	unlock(e);
	pthread_mutex_destroy(&e->mu);
	free(e);
	return NULL;
}

void sd_event_unrefp(sd_event **e) { if (e && *e) sd_event_unref(*e); }

int sd_event_get_fd(sd_event *e) { return e->epfd; }
int sd_event_get_state(sd_event *e) { lock(e); int s = e->state; unlock(e); return s; }

static sd_event_source *source_new(sd_event *e, sd_event_source **out, enum kind k, void *cb, void *ud) {
	sd_event_source *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	s->e = e; s->kind = k; s->cb = cb; s->userdata = ud; s->refs = 1; s->fd = -1;
	s->enabled = k == K_TIME ? SD_EVENT_ONESHOT : SD_EVENT_ON;
	s->floating = out == NULL;
	s->seq = e->seq++;
	s->next = e->list; e->list = s;
	if (out) *out = s;
	return s;
}

static void poke(sd_event *e) { uint64_t one = 1; (void)!write(e->efd, &one, sizeof one); }

/* arm the timerfd for the earliest enabled time source */
static void arm_timer(sd_event *e) {
	uint64_t t = 0;
	for (sd_event_source *s = e->list; s; s = s->next)
		if (s->kind == K_TIME && s->enabled != SD_EVENT_OFF && !s->dead && (!t || s->usec < t)) t = s->usec;
	struct itimerspec its = { 0 };
	if (t) {
		if (t == 0) t = 1;
		its.it_value.tv_sec = t / 1000000u;
		its.it_value.tv_nsec = (t % 1000000u) * 1000u;
		if (!its.it_value.tv_sec && !its.it_value.tv_nsec) its.it_value.tv_nsec = 1;
	}
	timerfd_settime(e->tfd, TFD_TIMER_ABSTIME, &its, NULL);
}

int sd_event_add_io(sd_event *e, sd_event_source **out, int fd, uint32_t events, sd_event_io_handler_t cb, void *ud) {
	lock(e);
	sd_event_source *s = source_new(e, out, K_IO, (void *)cb, ud);
	if (!s) { unlock(e); return -ENOMEM; }
	s->fd = fd; s->events = events;
	struct epoll_event ev = { .events = events, .data.ptr = s };
	if (epoll_ctl(e->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		int err = -errno;
		s->kind = K_DEFER; /* not in epoll, so source_free must not try to remove it */
		source_free(s);
		if (out) *out = NULL;
		unlock(e);
		return err;
	}
	unlock(e);
	return 0;
}

int sd_event_add_time(sd_event *e, sd_event_source **out, clockid_t clock, uint64_t usec, uint64_t acc,
                      sd_event_time_handler_t cb, void *ud) {
	(void)acc;
	if (clock != CLOCK_MONOTONIC) return -EOPNOTSUPP;
	lock(e);
	sd_event_source *s = source_new(e, out, K_TIME, (void *)cb, ud);
	if (!s) { unlock(e); return -ENOMEM; }
	s->usec = usec;
	arm_timer(e);
	unlock(e);
	return 0;
}

int sd_event_add_defer(sd_event *e, sd_event_source **out, sd_event_handler_t cb, void *ud) {
	lock(e);
	sd_event_source *s = source_new(e, out, K_DEFER, (void *)cb, ud);
	unlock(e);
	if (!s) return -ENOMEM;
	poke(e);
	return 0;
}

sd_event_source *sd_event_source_ref(sd_event_source *s) {
	if (s) { lock(s->e); s->refs++; unlock(s->e); }
	return s;
}

sd_event_source *sd_event_source_unref(sd_event_source *s) {
	if (!s) return NULL;
	sd_event *e = s->e;
	lock(e);
	if (--s->refs <= 0) source_free(s);
	unlock(e);
	return NULL;
}

void sd_event_source_unrefp(sd_event_source **s) { if (s && *s) sd_event_source_unref(*s); }

int sd_event_source_set_enabled(sd_event_source *s, int enabled) {
	if (!s) return -EINVAL;
	sd_event *e = s->e;
	lock(e);
	s->enabled = enabled;
	if (enabled == SD_EVENT_OFF) s->pending = 0;
	if (s->kind == K_IO) {
		struct epoll_event ev = { .events = enabled == SD_EVENT_OFF ? 0 : s->events, .data.ptr = s };
		epoll_ctl(e->epfd, EPOLL_CTL_MOD, s->fd, &ev);
	} else if (s->kind == K_TIME) {
		arm_timer(e);
	} else if (enabled != SD_EVENT_OFF) {
		poke(e);
	}
	unlock(e);
	return 0;
}

sd_event_source *sd_event_source_disable_unref(sd_event_source *s) {
	if (!s) return NULL;
	sd_event_source_set_enabled(s, SD_EVENT_OFF);
	return sd_event_source_unref(s);
}

int sd_event_source_set_priority(sd_event_source *s, int64_t prio) {
	if (!s) return -EINVAL;
	lock(s->e); s->prio = prio; unlock(s->e);
	return 0;
}

/* floating = owned by the loop: it takes its own reference */
int sd_event_source_set_floating(sd_event_source *s, int b) {
	if (!s) return -EINVAL;
	lock(s->e);
	if (b && !s->floating) { s->floating = 1; s->refs++; }
	else if (!b && s->floating) { s->floating = 0; s->refs--; }
	unlock(s->e);
	return 0;
}

static void drain(int fd) { uint64_t v; while (read(fd, &v, sizeof v) > 0) {} }

/* mark due time sources and enabled defer sources pending; returns whether anything is pending */
static int collect(sd_event *e) {
	uint64_t t = now_usec();
	int any = e->exit_req;
	for (sd_event_source *s = e->list; s; s = s->next) {
		if (s->dead || s->enabled == SD_EVENT_OFF) continue;
		if (s->kind == K_DEFER) s->pending = 1;
		else if (s->kind == K_TIME && s->usec <= t) s->pending = 1;
		if (s->pending) any = 1;
	}
	return any;
}

int sd_event_prepare(sd_event *e) {
	lock(e);
	if (e->state != SD_EVENT_INITIAL) { unlock(e); return -EBUSY; }
	int any = collect(e);
	if (!any) arm_timer(e);
	e->state = any ? SD_EVENT_PENDING : SD_EVENT_ARMED;
	unlock(e);
	return any;
}

int sd_event_wait(sd_event *e, uint64_t usec) {
	lock(e);
	if (e->state != SD_EVENT_ARMED) { unlock(e); return -EBUSY; }
	struct epoll_event evs[32];
	int timeout = usec == (uint64_t)-1 ? -1 : (int)(usec / 1000u);
	unlock(e);
	int n = epoll_wait(e->epfd, evs, 32, timeout);
	lock(e);
	if (n < 0 && errno != EINTR) { int err = -errno; e->state = SD_EVENT_INITIAL; unlock(e); return err; }
	for (int i = 0; i < n; i++) {
		void *p = evs[i].data.ptr;
		if (p == &e->tfd) { drain(e->tfd); continue; }
		if (p == &e->efd) { drain(e->efd); continue; }
		sd_event_source *s = p;
		for (sd_event_source *q = e->list; q; q = q->next) /* only if it still exists */
			if (q == s && !s->dead && s->enabled != SD_EVENT_OFF) { s->pending = 1; s->revents = evs[i].events; }
	}
	int any = collect(e);
	e->state = any ? SD_EVENT_PENDING : SD_EVENT_INITIAL;
	unlock(e);
	return any;
}

int sd_event_dispatch(sd_event *e) {
	lock(e);
	if (e->state != SD_EVENT_PENDING) { unlock(e); return -EBUSY; }
	if (e->exit_req) { e->state = SD_EVENT_FINISHED; unlock(e); return 0; }
	sd_event_source *best = NULL;
	for (sd_event_source *s = e->list; s; s = s->next)
		if (s->pending && !s->dead && (!best || s->prio < best->prio || (s->prio == best->prio && s->seq < best->seq)))
			best = s;
	e->state = SD_EVENT_RUNNING;
	if (best) {
		best->pending = 0;
		best->refs++; /* the callback may unref it */
		if (best->kind == K_TIME && best->enabled == SD_EVENT_ONESHOT) best->enabled = SD_EVENT_OFF;
		int r;
		if (best->kind == K_IO) r = ((sd_event_io_handler_t)best->cb)(best, best->fd, best->revents, best->userdata);
		else if (best->kind == K_TIME) r = ((sd_event_time_handler_t)best->cb)(best, best->usec, best->userdata);
		else r = ((sd_event_handler_t)best->cb)(best, best->userdata);
		if (r < 0) best->enabled = SD_EVENT_OFF; /* like sd-event: a failing source is disabled */
		if (--best->refs <= 0) source_free(best);
		else if (best->kind == K_TIME) arm_timer(e);
	}
	e->state = e->exit_req ? SD_EVENT_FINISHED : SD_EVENT_INITIAL;
	/* leftovers stay pending; the next prepare sees them through collect() or the pending flag */
	unlock(e);
	return 1;
}

int sd_event_exit(sd_event *e, int code) {
	(void)code;
	lock(e); e->exit_req = 1; unlock(e);
	poke(e);
	return 0;
}
