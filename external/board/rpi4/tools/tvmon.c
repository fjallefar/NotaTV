/*
 * tvmon - small terminal dashboard for the Raspberry Pi 4 (NotaTV).
 * Reads temperature, clocks, voltage and throttle status directly from
 * the VideoCore firmware via /dev/vcio (the same mailbox vcgencmd uses),
 * plus CPU load and memory from /proc.
 *
 *   tvmon          live dashboard (q or Ctrl-C to stop)
 *   tvmon -l       one line per second (for logging)
 *   tvmon -1       one measurement, then exit
 *   tvmon -i 0.5   update interval in seconds
 */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char *)

#define TAG_GET_CLOCK_RATE          0x00030002
#define TAG_GET_VOLTAGE             0x00030003
#define TAG_GET_MAX_CLOCK_RATE      0x00030004
#define TAG_GET_TEMPERATURE         0x00030006
#define TAG_GET_THROTTLED           0x00030046
#define TAG_GET_CLOCK_RATE_MEASURED 0x00030047

static int mbox = -1;

/* One property request with up to two 32-bit values in/out. */
static int mbox_prop(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out_b)
{
    uint32_t buf[8] __attribute__((aligned(16))) = {
        sizeof(buf), 0, tag, 8, 8, a, b, 0
    };
    if (ioctl(mbox, IOCTL_MBOX_PROPERTY, buf) < 0 || buf[1] != 0x80000000)
        return -1;
    *out_b = buf[6];
    return 0;
}

static long get_u(uint32_t tag, uint32_t id)
{
    uint32_t v;
    return mbox_prop(tag, id, 0, &v) ? -1 : (long)v;
}

static long get_throttled(void)
{
    uint32_t buf[7] __attribute__((aligned(16))) = {
        sizeof(buf), 0, TAG_GET_THROTTLED, 4, 4, 0, 0
    };
    if (ioctl(mbox, IOCTL_MBOX_PROPERTY, buf) < 0 || buf[1] != 0x80000000)
        return -1;
    return buf[5];
}

struct clk { const char *name; uint32_t id; };
static const struct clk clks[] = {
    { "arm",  3 }, { "core", 4 }, { "v3d", 5 }, { "hevc", 11 },
    { "h264", 6 }, { "isp",  7 }, { "sdram", 8 },
};
#define NCLK (sizeof(clks) / sizeof(clks[0]))

/* CPU load per core from /proc/stat */
#define MAXCPU 8
static unsigned long long prev_tot[MAXCPU + 1], prev_idle[MAXCPU + 1];

static int cpu_load(double *load)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[256];
    int n = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f) && n <= MAXCPU) {
        unsigned long long u, ni, s, id, io, irq, sirq, st;
        if (strncmp(line, "cpu", 3))
            break;
        char *p = strchr(line, ' ');
        if (sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &ni, &s, &id, &io, &irq, &sirq, &st) != 8)
            continue;
        unsigned long long idle = id + io, tot = u + ni + s + id + io + irq + sirq + st;
        unsigned long long dt = tot - prev_tot[n], di = idle - prev_idle[n];
        load[n] = dt ? 100.0 * (double)(dt - di) / (double)dt : 0;
        prev_tot[n] = tot;
        prev_idle[n] = idle;
        n++;
    }
    fclose(f);
    return n; /* load[0] = total, load[1..] = cores */
}

static long mem_avail_mb(long *total)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char k[64];
    long v, avail = -1;
    *total = -1;
    if (!f)
        return -1;
    while (fscanf(f, "%63s %ld kB\n", k, &v) == 2) {
        if (!strcmp(k, "MemTotal:"))
            *total = v / 1024;
        else if (!strcmp(k, "MemAvailable:"))
            avail = v / 1024;
    }
    fclose(f);
    return avail;
}

#define RST "\033[0m"
#define DIM "\033[2m"
#define BLD "\033[1m"
#define RED "\033[31m"
#define GRN "\033[32m"
#define YEL "\033[33m"
#define CYN "\033[36m"

static const char *temp_col(double t)
{
    return t < 60 ? GRN : t < 75 ? YEL : RED;
}

static void bar(double pct, int w)
{
    int f = (int)(pct / 100.0 * w + 0.5);
    if (f > w) f = w;
    if (f < 0) f = 0;
    fputs(pct < 50 ? GRN : pct < 85 ? YEL : RED, stdout);
    for (int i = 0; i < f; i++)
        fputs("█", stdout);
    fputs(RST DIM, stdout);
    for (int i = f; i < w; i++)
        fputs("·", stdout);
    fputs(RST, stdout);
}

static const char *thr_bits[] = {
    "under-voltage", "arm-freq capped", "throttled", "soft temp-limit",
};

static double tmax = 0;

static void draw(int live)
{
    double load[MAXCPU + 1] = {0};
    int ncpu = cpu_load(load) - 1;
    double temp = get_u(TAG_GET_TEMPERATURE, 0) / 1000.0;
    long uv = get_u(TAG_GET_VOLTAGE, 1);
    long thr = get_throttled();
    long mtot, mav = mem_avail_mb(&mtot);
    if (temp > tmax)
        tmax = temp;

    if (!live) {
        time_t now = time(NULL);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
        printf("%s temp=%.1f vcore=%.4f thr=0x%lx", ts, temp, uv / 1e6, thr);
        for (size_t i = 0; i < NCLK; i++)
            printf(" %s=%ld", clks[i].name,
                   get_u(TAG_GET_CLOCK_RATE_MEASURED, clks[i].id) / 1000000);
        printf(" cpu=%.0f%%\n", load[0]);
        fflush(stdout);
        return;
    }

    printf("\033[H");
    printf(BLD CYN " NotaTV · Pi 4 monitor" RST DIM "   (q = afslut)\033[K\n\n" RST);

    printf("  Temp    %s" BLD "%5.1f °C" RST DIM "   max %.1f °C" RST "\033[K\n",
           temp_col(temp), temp, tmax);
    printf("  Vcore   " BLD "%.4f V" RST "\033[K\n", uv / 1e6);
    printf("  RAM     %ld / %ld MB fri\033[K\n\n", mav, mtot);

    printf(DIM "  Clock      målt      sat      max" RST "\033[K\n");
    for (size_t i = 0; i < NCLK; i++) {
        long m = get_u(TAG_GET_CLOCK_RATE_MEASURED, clks[i].id);
        long s = get_u(TAG_GET_CLOCK_RATE, clks[i].id);
        long x = get_u(TAG_GET_MAX_CLOCK_RATE, clks[i].id);
        printf("  %-6s " BLD "%5ld" RST " MHz %5ld MHz %5ld MHz\033[K\n",
               clks[i].name, m / 1000000, s / 1000000, x / 1000000);
    }

    printf("\n  CPU     ");
    bar(load[0], 24);
    printf(" %5.1f%%\033[K\n", load[0]);
    for (int c = 1; c <= ncpu; c++) {
        printf(DIM "   cpu%d  " RST, c - 1);
        bar(load[c], 24);
        printf(" %5.1f%%\033[K\n", load[c]);
    }

    printf("\n  Status  ");
    if (thr < 0) {
        printf(DIM "ukendt" RST);
    } else if (!thr) {
        printf(GRN "OK - ingen throttling" RST);
    } else {
        for (int b = 0; b < 4; b++) {
            if (thr & (1 << b))
                printf(RED "%s NU  " RST, thr_bits[b]);
            else if (thr & (1 << (b + 16)))
                printf(YEL "%s (har været)  " RST, thr_bits[b]);
        }
    }
    printf(DIM "  0x%lx" RST "\033[K\n\033[J", thr);
    fflush(stdout);
}

static struct termios old_tio;
static int tio_saved;

static void restore(void)
{
    if (tio_saved)
        tcsetattr(0, TCSANOW, &old_tio);
    printf("\033[?25h" RST "\n");
}

static void on_sig(int s)
{
    (void)s;
    exit(0);
}

int main(int argc, char **argv)
{
    int live = 1, once = 0;
    double interval = 1.0;
    int opt;
    while ((opt = getopt(argc, argv, "l1i:")) != -1) {
        if (opt == 'l')
            live = 0;
        else if (opt == '1')
            once = 1, live = 0;
        else if (opt == 'i')
            interval = atof(optarg);
        else {
            fprintf(stderr, "brug: tvmon [-l] [-1] [-i sek]\n");
            return 1;
        }
    }

    mbox = open("/dev/vcio", O_RDONLY);
    if (mbox < 0) {
        perror("/dev/vcio");
        return 1;
    }

    double dummy[MAXCPU + 1];
    cpu_load(dummy);

    if (live) {
        if (tcgetattr(0, &old_tio) == 0) {
            struct termios t = old_tio;
            t.c_lflag &= ~(ICANON | ECHO);
            tcsetattr(0, TCSANOW, &t);
            tio_saved = 1;
        }
        atexit(restore);
        signal(SIGINT, on_sig);
        signal(SIGTERM, on_sig);
        printf("\033[2J\033[?25l");
    }

    if (once)
        usleep(200000);

    for (;;) {
        draw(live);
        if (once)
            break;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        struct timeval tv = { (long)interval, (long)((interval - (long)interval) * 1e6) };
        if (live && select(1, &rf, NULL, NULL, &tv) > 0) {
            char c;
            if (read(0, &c, 1) == 1 && (c == 'q' || c == 'Q'))
                break;
        } else if (!live) {
            usleep((useconds_t)(interval * 1e6));
        }
    }
    return 0;
}
