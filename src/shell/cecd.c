/* cecd - answers the HDMI-CEC questions the kernel does not answer itself. Usage: cecd /dev/cecN
 *
 * The kernel's CEC core (with cec-ctl --playback, see /usr/libexec/notatv/cec) already answers CEC version,
 * physical address, OSD name and vendor ID, and turns the TV remote's keys into evdev keys. Anything else it
 * answers with "Feature Abort" when no program follows the adapter. LG does not mind, but Samsung (Anynet+) does:
 * it only sends remote keys to a device that answers "Menu Request" with "menu activated", and it asks for the
 * power status and the active source. cecd follows the adapter and answers:
 *   Menu Request            -> Menu Status: activated
 *   Give Device Power Status -> on, or standby while the box sleeps (/run/notatv/sleeping)
 *   Request Active Source   -> Active Source, if the box is the one showing
 *   Set Stream Path (ours)  -> Active Source (the TV switched to the box's input)
 *   Give Deck Status        -> stopped
 *   Standby                 -> "cec on-standby" (sleep when the TV turns off, if that setting is on)
 * Other messages sent directly to the box get "Feature Abort" as before. Unknown and vendor-specific ones are
 * written to /tmp/notatv.log, so a new TV make can be looked into. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/cec-funcs.h>
#include <linux/cec.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SLEEPING "/run/notatv/sleeping"
#define HELPER "/usr/libexec/notatv/cec"
#define LOGFILE "/tmp/notatv.log"

static int fd = -1;
static const char *dev;
static bool active = true; /* the box claims the TV's input when it starts (cec wake) */

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    FILE *f = fopen(LOGFILE, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(f, "[%02d:%02d:%02d] cec: ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static bool sleeping(void) { return access(SLEEPING, F_OK) == 0; }

static __u16 phys_addr(void) {
    __u16 pa = CEC_PHYS_ADDR_INVALID;
    if (ioctl(fd, CEC_ADAP_G_PHYS_ADDR, &pa) < 0) return CEC_PHYS_ADDR_INVALID;
    return pa;
}

static int own_la(void) {
    struct cec_log_addrs la;
    memset(&la, 0, sizeof la);
    if (ioctl(fd, CEC_ADAP_G_LOG_ADDRS, &la) < 0 || la.num_log_addrs == 0) return -1;
    return la.log_addr[0];
}

static void tx(struct cec_msg *m) {
    m->timeout = 0; /* no waiting for a reply */
    /* the fd is non-blocking (see main): the bus can briefly be busy (another transmit in
     * progress, e.g. "cec wake" from another fd) and then the kernel returns EAGAIN.
     * Retry for up to half a second instead of dropping the answer to the TV. */
    for (int i = 0; i < 50; i++) {
        if (ioctl(fd, CEC_TRANSMIT, m) == 0) return;
        if (errno != EAGAIN && errno != EBUSY && errno != EINTR) break; /* EBUSY: the transmit queue is full */
        usleep(10000);
    }
    logmsg("%s: send 0x%02x failed (%s)", dev, m->msg[1], strerror(errno));
}

static void claim_active(void) {
    __u16 pa = phys_addr();
    int la = own_la();
    if (pa == CEC_PHYS_ADDR_INVALID || la < 0) return;
    struct cec_msg m;
    cec_msg_init(&m, la, CEC_LOG_ADDR_BROADCAST);
    cec_msg_active_source(&m, pa);
    tx(&m);
    active = true;
}

static void hexdump(const struct cec_msg *m, char *out, size_t n) {
    size_t o = 0;
    out[0] = 0;
    for (unsigned i = 0; i < m->len && o + 4 < n; i++) o += snprintf(out + o, n - o, "%s%02x", i ? ":" : "", m->msg[i]);
}

/* messages that are answers or reports: never answered with Feature Abort (CEC 1.4 12.3) */
static bool no_abort(__u8 op) {
    switch (op) {
    case CEC_MSG_FEATURE_ABORT: case CEC_MSG_USER_CONTROL_PRESSED: case CEC_MSG_USER_CONTROL_RELEASED:
    case CEC_MSG_SET_OSD_NAME: case CEC_MSG_REPORT_POWER_STATUS: case CEC_MSG_CEC_VERSION:
    case CEC_MSG_DEVICE_VENDOR_ID: case CEC_MSG_MENU_STATUS: case CEC_MSG_REPORT_FEATURES:
    case CEC_MSG_DECK_STATUS: case CEC_MSG_REPORT_AUDIO_STATUS: case CEC_MSG_SYSTEM_AUDIO_MODE_STATUS:
    case CEC_MSG_SET_SYSTEM_AUDIO_MODE: case CEC_MSG_REPORT_PHYSICAL_ADDR: case CEC_MSG_INACTIVE_SOURCE:
    case CEC_MSG_REPORT_SHORT_AUDIO_DESCRIPTOR: case CEC_MSG_REPORT_ARC_INITIATED:
    case CEC_MSG_REPORT_ARC_TERMINATED: case CEC_MSG_SET_MENU_LANGUAGE:
        return true;
    default:
        return false;
    }
}

static void received(struct cec_msg *m) {
    if (m->len < 2) return; /* poll message */
    __u8 op = m->msg[1];
    bool bcast = cec_msg_is_broadcast(m);
    int from = cec_msg_initiator(m);
    struct cec_msg r;
    memset(&r, 0, sizeof r);
    __u16 pa;
    char hex[64];

    switch (op) {
    case CEC_MSG_MENU_REQUEST:
        if (bcast) return;
        cec_msg_set_reply_to(&r, m);
        cec_msg_menu_status(&r, CEC_OP_MENU_STATE_ACTIVATED);
        tx(&r);
        return;
    case CEC_MSG_GIVE_DEVICE_POWER_STATUS:
        if (bcast || from == CEC_LOG_ADDR_UNREGISTERED) return;
        cec_msg_set_reply_to(&r, m);
        cec_msg_report_power_status(&r, sleeping() ? CEC_OP_POWER_STATUS_STANDBY : CEC_OP_POWER_STATUS_ON);
        tx(&r);
        return;
    case CEC_MSG_GIVE_DECK_STATUS:
        if (bcast) return;
        cec_msg_set_reply_to(&r, m);
        cec_msg_deck_status(&r, CEC_OP_DECK_INFO_STOP);
        tx(&r);
        return;
    case CEC_MSG_REQUEST_ACTIVE_SOURCE:
        if (active && !sleeping()) claim_active();
        return;
    case CEC_MSG_SET_STREAM_PATH:
        cec_ops_set_stream_path(m, &pa);
        if (pa == phys_addr() && !sleeping()) claim_active();
        else active = false;
        return;
    case CEC_MSG_ACTIVE_SOURCE:
        cec_ops_active_source(m, &pa);
        if (pa != phys_addr()) active = false;
        return;
    case CEC_MSG_ROUTING_CHANGE: {
        __u16 old_pa;
        cec_ops_routing_change(m, &old_pa, &pa);
        if (pa != phys_addr()) active = false;
        return;
    }
    case CEC_MSG_ROUTING_INFORMATION:
        cec_ops_routing_information(m, &pa);
        if (pa != phys_addr()) active = false;
        return;
    case CEC_MSG_STANDBY:
        if (!sleeping()) {
            logmsg("%s: TV turned off", dev);
            pid_t pid = fork(); /* SIGCHLD is ignored: no zombies */
            if (pid == 0) {
                execl(HELPER, HELPER, "on-standby", (char *)NULL);
                _exit(127);
            }
            if (pid < 0) logmsg("%s: on-standby failed (%s)", dev, strerror(errno));
        }
        return;
    case CEC_MSG_VENDOR_COMMAND: case CEC_MSG_VENDOR_COMMAND_WITH_ID: case CEC_MSG_VENDOR_REMOTE_BUTTON_DOWN:
    case CEC_MSG_VENDOR_REMOTE_BUTTON_UP:
        hexdump(m, hex, sizeof hex);
        logmsg("%s: vendor message from %d: %s", dev, from, hex);
        break;
    default:
        break;
    }
    if (bcast || from == CEC_LOG_ADDR_UNREGISTERED || no_abort(op)) return;
    if (op != CEC_MSG_VENDOR_COMMAND && op != CEC_MSG_VENDOR_COMMAND_WITH_ID && op != CEC_MSG_VENDOR_REMOTE_BUTTON_DOWN &&
        op != CEC_MSG_VENDOR_REMOTE_BUTTON_UP) {
        hexdump(m, hex, sizeof hex);
        logmsg("%s: not supported, from %d: %s", dev, from, hex);
    }
    cec_msg_set_reply_to(&r, m);
    cec_msg_feature_abort(&r, op, CEC_OP_ABORT_UNRECOGNIZED_OP);
    tx(&r);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: cecd /dev/cecN\n");
        return 2;
    }
    dev = argv[1];
    signal(SIGCHLD, SIG_IGN);
    /* O_NONBLOCK: CEC_DQEVENT/CEC_RECEIVE below drain with "until error" loops; on a blocking
     * fd that ioctl waits for the next event/message instead of returning EAGAIN, so the
     * daemon hangs in the first DQEVENT (the kernel queues initial events at open) and the
     * TV's questions are never read. Poll already tells us when something is ready. */
    fd = open(dev, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        logmsg("%s: cannot open (%s)", dev, strerror(errno));
        return 1;
    }
    __u32 mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
    if (ioctl(fd, CEC_S_MODE, &mode) < 0) {
        logmsg("%s: follower mode failed (%s)", dev, strerror(errno));
        return 1;
    }
    bool was_sleeping = sleeping();
    for (;;) {
        struct pollfd p = {fd, POLLIN | POLLPRI, 0};
        int n = poll(&p, 1, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* after waking up, "cec wake" has taken the TV's input again */
        bool s = sleeping();
        if (was_sleeping && !s) active = true;
        was_sleeping = s;
        if (n == 0) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break; /* adapter gone (USB adapter unplugged) */
        if (p.revents & POLLPRI) {
            struct cec_event ev;
            while (ioctl(fd, CEC_DQEVENT, &ev) == 0) {
                if (ev.event == CEC_EVENT_LOST_MSGS) logmsg("%s: %u messages lost", dev, ev.lost_msgs.lost_msgs);
                memset(&ev, 0, sizeof ev);
            }
        }
        if (p.revents & POLLIN) {
            struct cec_msg m;
            memset(&m, 0, sizeof m);
            while (ioctl(fd, CEC_RECEIVE, &m) == 0) {
                received(&m);
                memset(&m, 0, sizeof m);
            }
        }
    }
    logmsg("%s: stopped", dev);
    return 0;
}
