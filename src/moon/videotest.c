/* tvmoon videotest: runs a video file through exactly the same decoder and display path as the stream
 * (dr_setup/dr_submit in video.c), at the file's frame rate. For testing without a Sunshine PC. */
#include "tvmoon.h"
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool tvmoon_signalled(void);

int videotest_run(const char *path, int fps, const char *mode) {
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, path, NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) {
        tlog("videotest: kan ikke åbne %s", path);
        return 1;
    }
    int si = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si < 0) { tlog("videotest: ingen video"); return 1; }
    AVCodecParameters *par = fc->streams[si]->codecpar;
    int fmt = par->codec_id == AV_CODEC_ID_HEVC ? (par->format == AV_PIX_FMT_YUV420P10 ? VIDEO_FORMAT_H265_MAIN10 : VIDEO_FORMAT_H265)
              : par->codec_id == AV_CODEC_ID_H264 ? VIDEO_FORMAT_H264 : 0;
    if (!fmt) { tlog("videotest: kun H.264/HEVC"); return 1; }
    AVRational fr = fc->streams[si]->avg_frame_rate;
    if (fr.num > 0 && fr.den > 0) fps = (int)(av_q2d(fr) + 0.5);
    if (fps <= 0) fps = 60;
    /* MP4 stores NAL units with length fields; the stream from Sunshine is Annex B */
    const AVBitStreamFilter *bf = av_bsf_get_by_name(par->codec_id == AV_CODEC_ID_HEVC ? "hevc_mp4toannexb" : "h264_mp4toannexb");
    AVBSFContext *bsf = NULL;
    av_bsf_alloc(bf, &bsf);
    avcodec_parameters_copy(bsf->par_in, par);
    bsf->time_base_in = fc->streams[si]->time_base;
    av_bsf_init(bsf);

    VideoOpts vo = {.mode = mode};
    video_configure(&vo);
    if (fmt == VIDEO_FORMAT_H265_MAIN10) video_set_hdr(true);
    if (video_callbacks.setup(fmt, par->width, par->height, fps, NULL, 0) != 0) return 1;
    tout("status\tstreaming");
    tlog("videotest: %s %dx%d@%d", avcodec_get_name(par->codec_id), par->width, par->height, fps);
    AVPacket *pkt = av_packet_alloc();
    int frame = 0;
    double t0 = now_s(), last = t0;
    while (!tvmoon_signalled() && av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index != si) { av_packet_unref(pkt); continue; }
        av_bsf_send_packet(bsf, pkt);
        while (av_bsf_receive_packet(bsf, pkt) == 0) {
            /* like from the network: one frame per 1/fps second */
            double due = t0 + (double)frame / fps;
            double wait = due - now_s();
            if (wait > 0) usleep((useconds_t)(wait * 1e6));
            LENTRY e = {.next = NULL, .data = (char *)pkt->data, .length = pkt->size, .bufferType = BUFFER_TYPE_PICDATA};
            DECODE_UNIT du = {0};
            du.frameNumber = ++frame;
            du.frameType = (pkt->flags & AV_PKT_FLAG_KEY) ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME;
            du.receiveTimeUs = LiGetMicroseconds();
            du.enqueueTimeUs = du.receiveTimeUs;
            du.fullLength = pkt->size;
            du.bufferList = &e;
            video_callbacks.submitDecodeUnit(&du);
            av_packet_unref(pkt);
        }
        if (now_s() - last >= 1) {
            last = now_s();
            char vs[256];
            VStat st;
            video_stats(vs, sizeof vs, &st);
            tlog("stat: %s", vs);
            if (getenv("TVMOON_STATS")) { /* overlay test */
                overlay_show(true);
                char l[2][96];
                snprintf(l[0], sizeof l[0], "videotest · %dp %d Hz", st.disp_h, st.hz);
                snprintf(l[1], sizeof l[1], T("Received → display: %.1f ms"), st.total_ms);
                const char *lines[2] = {l[0], l[1]};
                overlay_set_text(lines, 2, -1, st.hdr);
            }
        }
    }
    av_packet_free(&pkt);
    av_bsf_free(&bsf);
    avformat_close_input(&fc);
    video_callbacks.cleanup();
    tout("status\tstopped");
    return 0;
}
