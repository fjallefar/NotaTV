/* NotaTV shell - game streaming: starts tvmoon (its own process, like mpv) and gives up the display meanwhile.
 * A crash in tvmoon does not take the user interface with it; tvshell takes the display back when it ends. */
#define _GNU_SOURCE /* pipe2 */
#include "tv.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* run tvmoon with the display; argv[0] is ignored. Each line from tvmoon's stdout is given to on_line.
   Returns tvmoon's exit code (-1 on error). */
int moon_run(char *const *argv, void (*on_line)(const char *line, void *ud), void *ud) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return -1;
    audio_bg_take();
    run_cmd("%s/music pause >/dev/null 2>&1", helper_path());
    bool video_off = app_video_suspend();
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM); /* tvshell died: stop the stream, so the display becomes free */
        int dn = open("/dev/null", O_RDONLY);
        dup2(dn, 0);
        dup2(pfd[1], 1);
        int lf = open("/tmp/tvmoon.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (lf >= 0) dup2(lf, 2);
        setsid();
        execv("/usr/bin/tvmoon", argv);
        _exit(127);
    }
    close(pfd[1]);
    int status = -1;
    if (pid > 0) {
        SDL_Log("moon: tvmoon startet (pid %d)", pid);
        char buf[1024];
        size_t len = 0;
        for (;;) {
            /* tvmoon reads controllers/keyboard/remote itself; we just empty our own queue */
            struct pollfd p[40];
            int fds[38];
            int nf = input_fds(fds, 38);
            for (int i = 0; i < nf; i++) p[i] = (struct pollfd){fds[i], POLLIN, 0};
            p[nf] = (struct pollfd){pfd[0], POLLIN, 0};
            poll(p, (nfds_t)nf + 1, 200);
            input_poll();
            InputEvent ev;
            while (input_next(&ev)) {}
            if (p[nf].revents & (POLLIN | POLLHUP)) {
                ssize_t r = read(pfd[0], buf + len, sizeof buf - 1 - len);
                if (r > 0) {
                    len += (size_t)r;
                    buf[len] = 0;
                    char *nl;
                    while ((nl = strchr(buf, '\n'))) {
                        *nl = 0;
                        SDL_Log("moon: %s", buf);
                        if (on_line) on_line(buf, ud);
                        len -= (size_t)(nl + 1 - buf);
                        memmove(buf, nl + 1, len + 1);
                    }
                    if (len >= sizeof buf - 1) len = 0;
                }
            }
            if (waitpid(pid, &status, WNOHANG) == pid) break;
        }
        SDL_Log("moon: tvmoon sluttede (%d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    close(pfd[0]);
    if (video_off) app_video_resume();
    InputEvent ev;
    while (input_next(&ev)) {}
    run_cmd("%s/music resume >/dev/null 2>&1", helper_path());
    return pid > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* debugging via the inbox (_moon): "stream 192.168.1.20 1234 -r 1920x1080" or "videotest /data/test/x.mp4" */
void moon_inbox(const char *args) {
    char copy[512];
    snprintf(copy, sizeof copy, "%s", args);
    char *argv[24] = {"tvmoon"};
    int n = 1;
    for (char *s = strtok(copy, " "); s && n < 23; s = strtok(NULL, " ")) argv[n++] = s;
    argv[n] = NULL;
    if (n < 2) return;
    moon_run(argv, NULL, NULL);
}
