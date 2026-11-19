// H.264 (AVC) and AAC decoding for the movie decoder (hle_movie.c) on Linux,
// through libavcodec. The interface is the one hle_movie_avc.h declares for
// VideoToolbox on Apple, so hle_movie.c is the same on both platforms.
//
// Super Mario Odyssey's 44 movies are MP4 / H.264 (hint videos, kingdom travel
// videos): High or Main profile with B-frames, and Baseline. The SDK's demuxer
// hands over one access unit per packet with Annex B start codes; SPS / PPS
// come in Configure's csd-0 / csd-1 (as with Android's MediaCodec) and may also
// appear in the stream. libavcodec's H.264 decoder reorders B-frames itself
// and hands pictures out in presentation order, so this file only holds a
// short FIFO of decoded pictures and converts them to the layout the movie
// decoder asks for (NV12, or RGBA in the native texture mode).
#include "hle.h"
#include <pthread/qos.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include "hle_movie_avc.h"
#include "hle_movie_aac.h"

#define AVC_QMAX 32

struct mavc {
    const AVCodec *codec;
    AVCodecContext *ctx;
    uint8_t *extradata;
    int extradata_n, dirty;
    struct { AVFrame *f; int64_t pts; } q[AVC_QMAX];
    int nq, eof;
    int rgba;                 // pictures as 4-byte RGBA (native texture mode) instead of NV12
    int bt709;                // the last picture's colour matrix
    int red;                  // one red line per decoder for an unsupported picture
};

// The SPS / PPS as libavcodec wants them in extradata: Annex B with start codes.
static void ps_annexb(mavc *a, const uint8_t *sps, int sn, const uint8_t *pps, int pn) {
    size_t n = (size_t)(sn > 0 ? sn + 4 : 0) + (size_t)(pn > 0 ? pn + 4 : 0);
    uint8_t *e = malloc(n ? n : 1);
    if (!e) die("movie: AVC extradata allocation failed");
    size_t o = 0;
    if (sn > 0) { memcpy(e + o, "\0\0\0\1", 4); o += 4; memcpy(e + o, sps, (size_t)sn); o += (size_t)sn; }
    if (pn > 0) { memcpy(e + o, "\0\0\0\1", 4); o += 4; memcpy(e + o, pps, (size_t)pn); o += (size_t)pn; }
    free(a->extradata);
    a->extradata = e; a->extradata_n = (int)o; a->dirty = 1;
}

mavc *mavc_new(void) {
    mavc *a = calloc(1, sizeof *a);
    if (!a) die("movie: AVC decoder allocation failed");
    a->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!a->codec) LOG("[RED] movie: libavcodec has no H.264 decoder, outputting black frames");
    return a;
}

void mavc_free(mavc *a) {
    if (!a) return;
    mavc_drop(a);
    if (a->ctx) avcodec_free_context(&a->ctx);
    free(a->extradata);
    free(a);
}

// Configure's csd-0 / csd-1, already with Annex B start codes.
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) {
    if (!a || !p || !n) return;
    // Split the Annex B runs: the first NAL of type 7 (SPS) and 8 (PPS).
    const uint8_t *sps = NULL, *pps = NULL; size_t sn = 0, pn = 0;
    size_t i = 0;
    while (i + 3 < n) {
        size_t sc = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) sc = 4;
        else if ((p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) || (i + 4 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 0 && p[i + 4] == 1)) sc = i + 4 < n && p[i + 3] == 0 ? 5 : 3;
        if (!sc) { i++; continue; }
        size_t start = i + sc, end = start;
        while (end + 3 < n && !(p[end] == 0 && p[end + 1] == 0 && (p[end + 2] == 1 || (p[end + 2] == 0 && p[end + 3] == 1)))) end++;
        if (end <= start) { i = start; continue; }
        unsigned t = p[start] & 31;
        if (t == 7 && !sps) { sps = p + start; sn = end - start; }
        else if (t == 8 && !pps) { pps = p + start; pn = end - start; }
        i = end;
    }
    if (sn || pn) ps_annexb(a, sps, (int)sn, pps, (int)pn);
}

// An AVCDecoderConfigurationRecord (the MP4 avcC box): 2-byte lengths.
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (!a || !p || n < 7) return;
    size_t i = 6, sps_n = p[5] & 31;
    const uint8_t *sps = NULL; size_t sn = 0;
    for (size_t k = 0; k < sps_n && i + 2 <= n; k++) {
        size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2;
        if (i + len > n) break;
        if (!sps) { sps = p + i; sn = len; }
        i += len;
    }
    const uint8_t *pps = NULL; size_t pn = 0;
    if (i < n) {
        size_t pps_n = p[i++];
        for (size_t k = 0; k < pps_n && i + 2 <= n; k++) {
            size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2;
            if (i + len > n) break;
            if (!pps) { pps = p + i; pn = len; }
            i += len;
        }
    }
    if (sn || pn) ps_annexb(a, sps, (int)sn, pps, (int)pn);
}

static int avc_open(mavc *a) {
    if (!a->codec) return 0;
    if (a->ctx && !a->dirty) return 1;
    if (a->ctx) avcodec_free_context(&a->ctx);
    a->ctx = avcodec_alloc_context3(a->codec);
    if (!a->ctx) return 0;
    if (a->extradata_n > 0) {
        a->ctx->extradata = av_mallocz((size_t)a->extradata_n + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!a->ctx->extradata) { avcodec_free_context(&a->ctx); return 0; }
        memcpy(a->ctx->extradata, a->extradata, (size_t)a->extradata_n);
        a->ctx->extradata_size = a->extradata_n;
    }
    a->ctx->thread_count = 1;   // the movie decoder has its own worker thread per decoder; keep the frame order simple
    // The packets' times are the movie's presentation times in microseconds;
    // saying so keeps the decoder from rescaling the timestamps it hands back
    // (they are what the player schedules the next picture by, and a rescaled
    // time made each picture look later than the last: the hint video crawled).
    a->ctx->pkt_timebase = (AVRational){1, 1000000};
    if (avcodec_open2(a->ctx, a->codec, NULL) < 0) {
        if (!a->red++) LOG("[RED] movie: libavcodec cannot open the H.264 decoder, outputting black frames");
        avcodec_free_context(&a->ctx);
        return 0;
    }
    a->dirty = 0; a->eof = 0;
    return 1;
}

// Take every picture libavcodec has ready into the queue (presentation order).
static void avc_receive(mavc *a) {
    for (;;) {
        if (a->nq == AVC_QMAX) break;
        AVFrame *f = av_frame_alloc();
        if (!f) return;
        int r = avcodec_receive_frame(a->ctx, f);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) { av_frame_free(&f); if (r == AVERROR_EOF) a->eof = 1; return; }
        if (r < 0) { static int red_recv; if (!red_recv++) LOG("[RED] movie: H.264 decode error %d, outputting black frames", r); av_frame_free(&f); return; }
        int64_t pts = f->pts != AV_NOPTS_VALUE ? f->pts
                    : f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : AV_NOPTS_VALUE;
        a->q[a->nq].f = f; a->q[a->nq].pts = pts; a->nq++;
        if (f->colorspace == AVCOL_SPC_BT709) a->bt709 = 1;
        else if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M) a->bt709 = 0;
    }
}

// One access unit with Annex B start codes; its time is in microseconds.
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    if (!a || !avc_open(a)) return 0;
    if (n <= 0) { avc_receive(a); return 0; }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return -1;
    if (av_new_packet(pkt, n) < 0) { av_packet_free(&pkt); return -1; }
    memcpy(pkt->data, p, (size_t)n);
    pkt->pts = pts_us;
    pkt->dts = pts_us;
    int r = avcodec_send_packet(a->ctx, pkt);
    av_packet_free(&pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        static int red_send; if (!red_send++) LOG("[RED] movie: H.264 packet rejected (%d)", r);
        return -1;
    }
    avc_receive(a);
    return 0;
}

int mavc_queued(const mavc *a) { return a ? a->nq : 0; }

void mavc_drop(mavc *a) {
    if (!a) return;
    for (int i = 0; i < a->nq; i++) av_frame_free(&a->q[i].f);
    a->nq = 0;
    if (a->ctx) avcodec_flush_buffers(a->ctx);
    a->eof = 0;
}

void mavc_set_rgba(mavc *a) { if (a) a->rgba = 1; }

int mavc_bt709(const mavc *a, int h) { return a && a->bt709 ? 1 : h >= 720; }

// ---------- pictures ----------
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Where the sample for output (x, y) is in a plane of the given subsampling.
static inline const uint8_t *px(const uint8_t *plane, int stride, int x, int y, int sx, int sy) {
    return plane + (size_t)(y / sy) * (size_t)stride + (size_t)(x / sx);
}

// YUV (4:2:0 / 4:2:2 / 4:4:4, 8-bit) to NV12 at dst: w x h luma then w x h/2
// interleaved chroma. A smaller picture is padded with video-range black, a
// larger one cropped.
static int to_nv12(const AVFrame *f, uint8_t *dst, int w, int h) {
    int sw = f->width, sh = f->height;
    int sw2 = (sw + 1) & ~1, sh2 = (sh + 1) & ~1;
    int sx, sy;
    const uint8_t *yp = f->data[0], *up = NULL, *vp = NULL;
    int ys = f->linesize[0], us = 0, vs = 0;
    switch (f->format) {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: sx = 2; sy = 2; up = f->data[1]; vp = f->data[2]; us = vs = f->linesize[1]; break;
    case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: sx = 2; sy = 1; up = f->data[1]; vp = f->data[2]; us = vs = f->linesize[1]; break;
    case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P: sx = 1; sy = 1; up = f->data[1]; vp = f->data[2]; us = vs = f->linesize[1]; break;
    case AV_PIX_FMT_NV12: sx = 2; sy = 2; up = f->data[1]; vp = f->data[1] + 1; us = vs = f->linesize[1]; break;
    default: return 0;
    }
    if (!yp) return 0;
    uint8_t *uv = dst + (size_t)w * h;
    for (int y = 0; y < h; y++) {
        if (y >= sh) { memset(dst + (size_t)y * w, 16, (size_t)w); continue; }
        const uint8_t *row = yp + (size_t)y * ys;
        int n = w < sw ? w : sw;
        memcpy(dst + (size_t)y * w, row, (size_t)n);
        if (n < w) memset(dst + (size_t)y * w + n, 16, (size_t)(w - n));
    }
    if (!up) {
        for (int y = 0; y < h / 2; y++) memset(uv + (size_t)y * w, 128, (size_t)w);
        return 1;
    }
    (void)sw2; (void)sh2;
    for (int y = 0; y < h; y += 2) {
        uint8_t *orow = uv + (size_t)(y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            int cy = y / sy, cx = x / sx;
            const uint8_t *u = px(up, us, x, y, sx, sy), *v = vp ? px(vp, vs, x, y, sx, sy) : u; (void)cx; (void)cy;
            orow[x] = *u; orow[x + 1] = *v;
        }
    }
    return 1;
}

static int to_rgba(const AVFrame *f, uint8_t *dst, int w, int h, int bt709) {
    int sw = f->width, sh = f->height, sx, sy;
    const uint8_t *yp = f->data[0], *up = NULL, *vp = NULL;
    int ys = f->linesize[0], us = 0, vs = 0;
    switch (f->format) {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: sx = 2; sy = 2; up = f->data[1]; vp = f->data[2]; us = vs = f->linesize[1]; break;
    case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: sx = 2; sy = 1; up = f->data[1]; vp = f->data[2]; us = vs = f->linesize[1]; break;
    case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P: sx = 1; sy = 1; up = f->data[1]; vp = f->data[2]; us = vs = f->linesize[1]; break;
    case AV_PIX_FMT_NV12: sx = 2; sy = 2; up = f->data[1]; vp = f->data[1] + 1; us = vs = f->linesize[1]; break;
    default: return 0;
    }
    if (!yp) return 0;
    int full = f->color_range == AVCOL_RANGE_JPEG;
    for (int y = 0; y < h; y++) {
        uint8_t *orow = dst + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            int Y = y < sh && x < sw ? yp[(size_t)y * ys + x] : 16;
            int U = 128, V = 128;
            if (up && y < sh && x < sw) { U = *px(up, us, x, y, sx, sy); V = vp ? *px(vp, vs, x, y, sx, sy) : U; }
            int r, g, b;
            if (full) { int c = Y, d = U - 128, e = V - 128; r = c + ((91881 * e) >> 16); g = c - ((22554 * d + 46802 * e) >> 16); b = c + ((116130 * d) >> 16); }
            else if (bt709) { int c = Y - 16, d = U - 128, e = V - 128; r = (298 * c + 459 * e + 128) >> 8; g = (298 * c - 55 * d - 136 * e + 128) >> 8; b = (298 * c + 541 * d + 128) >> 8; }
            else { int c = Y - 16, d = U - 128, e = V - 128; r = (298 * c + 409 * e + 128) >> 8; g = (298 * c - 100 * d - 208 * e + 128) >> 8; b = (298 * c + 516 * d + 128) >> 8; }
            orow[4 * x] = (uint8_t)clamp8(r); orow[4 * x + 1] = (uint8_t)clamp8(g); orow[4 * x + 2] = (uint8_t)clamp8(b); orow[4 * x + 3] = 255;
        }
    }
    return 1;
}

// The next picture in presentation order; with flush the decoder is drained
// first so the pictures still in it come out too.
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (!a || !a->ctx || w <= 0 || h <= 0) return 0;
    if (flush && a->nq == 0 && !a->eof) {
        avcodec_send_packet(a->ctx, NULL);
        a->eof = 1;
        avc_receive(a);
    }
    if (a->nq == 0) return 0;
    AVFrame *f = a->q[0].f;
    int64_t pts = a->q[0].pts;
    int ok = a->rgba ? to_rgba(f, dst, w, h, mavc_bt709(a, h)) : to_nv12(f, dst, w, h);
    av_frame_free(&a->q[0].f);
    memmove(&a->q[0], &a->q[1], sizeof a->q[0] * (size_t)(a->nq - 1));
    a->nq--;
    if (!ok) {
        memset(dst, 16, (size_t)w * h);
        memset(dst + (size_t)w * h, 128, (size_t)w * h / 2);
        if (!a->red++) LOG("[RED] movie: H.264 picture format %d is not converted, outputting a black frame", f->format);
    }
    if (pts_us) *pts_us = pts;
    return 1;
}

// ---------- AAC ----------
struct maac {
    AVCodecContext *ctx;
    AVFrame *f;
    int ch, rate, red;
};

maac *maac_new(int rate, int ch, const uint8_t *asc, int asc_n) {
    maac *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) { free(a); return NULL; }
    a->ctx = avcodec_alloc_context3(codec);
    a->f = av_frame_alloc();
    if (!a->ctx || !a->f) { avcodec_free_context(&a->ctx); av_frame_free(&a->f); free(a); return NULL; }
    a->rate = rate; a->ch = ch;
    if (asc_n > 0) {
        a->ctx->extradata = av_mallocz((size_t)asc_n + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!a->ctx->extradata) { avcodec_free_context(&a->ctx); av_frame_free(&a->f); free(a); return NULL; }
        memcpy(a->ctx->extradata, asc, (size_t)asc_n);
        a->ctx->extradata_size = asc_n;
    }
    a->ctx->thread_count = 1;
    if (avcodec_open2(a->ctx, codec, NULL) < 0) { avcodec_free_context(&a->ctx); av_frame_free(&a->f); free(a); return NULL; }
    return a;
}

void maac_free(maac *a) {
    if (!a) return;
    avcodec_free_context(&a->ctx);
    av_frame_free(&a->f);
    free(a);
}

int maac_decode(maac *a, const uint8_t *p, int n, int16_t *out, int max_frames) {
    if (!a || !a->ctx || n <= 0 || max_frames <= 0) return 0;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return 0;
    if (av_new_packet(pkt, n) < 0) { av_packet_free(&pkt); return 0; }
    memcpy(pkt->data, p, (size_t)n);
    int r = avcodec_send_packet(a->ctx, pkt);
    av_packet_free(&pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) return 0;
    r = avcodec_receive_frame(a->ctx, a->f);
    if (r < 0) return 0;
    int ch = a->f->ch_layout.nb_channels > 0 ? a->f->ch_layout.nb_channels : a->ch;
    int frames = a->f->nb_samples;
    if (frames > max_frames) frames = max_frames;
    enum AVSampleFormat fmt = (enum AVSampleFormat)a->f->format;
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) {
            int v;
            if (fmt == AV_SAMPLE_FMT_FLTP) v = (int)(((const float *)a->f->data[c])[i] * 32767.0f);
            else if (fmt == AV_SAMPLE_FMT_FLT) v = (int)(((const float *)a->f->data[0])[i * ch + c] * 32767.0f);
            else if (fmt == AV_SAMPLE_FMT_S16P) v = ((const int16_t *)a->f->data[c])[i];
            else if (fmt == AV_SAMPLE_FMT_S16) v = ((const int16_t *)a->f->data[0])[i * ch + c];
            else { if (!a->red++) LOG("[RED] movie: AAC sample format %d is not converted, outputting silence", (int)fmt); v = 0; }
            out[i * ch + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    av_frame_unref(a->f);
    return frames;
}
