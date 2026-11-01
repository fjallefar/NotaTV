/* tvmoon - audio: Opus (multistream) from the host -> PCM -> ALSA "default" (HDMI, via NotaTV' audio setup).
 *
 * The buffer is kept short (~40 ms). If more than that builds up in the queue (e.g. after a network hiccup),
 * packets are dropped, so the audio does not lag behind the picture for the rest of the session.
 */
#include "tvmoon.h"
#include <alsa/asoundlib.h>
#include <opus/opus_multistream.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_QUEUED_MS 60

static struct {
    OpusMSDecoder *dec;
    snd_pcm_t *pcm;
    int ch, rate, frame;
    int16_t *pcmbuf;
    int n_pkt, n_drop, n_xrun;
} A;

static int ar_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION oc, void *ctx, int flags) {
    int err = 0;
    A.ch = oc->channelCount;
    A.rate = oc->sampleRate;
    A.frame = oc->samplesPerFrame;
    A.dec = opus_multistream_decoder_create(oc->sampleRate, oc->channelCount, oc->streams, oc->coupledStreams, oc->mapping, &err);
    if (!A.dec) { tlog("lyd: opus-dekoder fejlede (%d)", err); return -1; }
    const char *dev = getenv("TVMOON_AUDIO");
    if (!dev || !*dev) dev = "default";
    if ((err = snd_pcm_open(&A.pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        tlog("lyd: kan ikke åbne %s: %s", dev, snd_strerror(err));
        return -1;
    }
    /* 40 ms buffer: short enough for games, long enough that a single late network packet does not stutter */
    if ((err = snd_pcm_set_params(A.pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned)A.ch,
                                  (unsigned)A.rate, 1, 40000)) < 0) {
        tlog("lyd: opsætning fejlede: %s", snd_strerror(err));
        return -1;
    }
    A.pcmbuf = malloc(sizeof(int16_t) * (size_t)A.ch * (size_t)A.frame * 2);
    tlog("lyd: %d kanaler, %d Hz, %d samples/pakke -> %s", A.ch, A.rate, A.frame, dev);
    return 0;
}

static void ar_start(void) {}
static void ar_stop(void) {}

static void ar_cleanup(void) {
    if (A.pcm) { snd_pcm_drop(A.pcm); snd_pcm_close(A.pcm); }
    if (A.dec) opus_multistream_decoder_destroy(A.dec);
    free(A.pcmbuf);
    A.pcm = NULL;
    A.dec = NULL;
    A.pcmbuf = NULL;
}

static void ar_play(char *data, int len) {
    if (!A.pcm || !A.dec) return;
    A.n_pkt++;
    /* too much in the queue: drop this packet, so the latency goes down again */
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(A.pcm, &delay) == 0 && delay > (snd_pcm_sframes_t)A.rate * MAX_QUEUED_MS / 1000) {
        A.n_drop++;
        return;
    }
    int n = opus_multistream_decode(A.dec, (const unsigned char *)data, len, A.pcmbuf, A.frame * 2, 0);
    if (n <= 0) return;
    snd_pcm_sframes_t w = snd_pcm_writei(A.pcm, A.pcmbuf, (snd_pcm_uframes_t)n);
    if (w < 0) {
        A.n_xrun++;
        if (snd_pcm_recover(A.pcm, (int)w, 1) == 0) snd_pcm_writei(A.pcm, A.pcmbuf, (snd_pcm_uframes_t)n);
    }
}

void audio_stats(char *out, size_t n) {
    snd_pcm_sframes_t delay = 0;
    if (A.pcm) snd_pcm_delay(A.pcm, &delay);
    snprintf(out, n, "lyd %d pk, smidt %d, xrun %d, buffer %.0f ms", A.n_pkt, A.n_drop, A.n_xrun,
             A.rate ? delay * 1000.0 / A.rate : 0);
    A.n_pkt = A.n_drop = A.n_xrun = 0;
}

AUDIO_RENDERER_CALLBACKS audio_callbacks = {
    .init = ar_init,
    .start = ar_start,
    .stop = ar_stop,
    .cleanup = ar_cleanup,
    .decodeAndPlaySample = ar_play,
    .capabilities = 0,
};
