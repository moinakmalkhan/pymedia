// pymedia.c — Video processing library using FFmpeg
//
// Features: audio extraction, format conversion, compression, trimming,
//           muting, frame extraction, GIF conversion, video info,
//           rotate, speed change, volume adjust, merge, reverse,
//           metadata strip/set

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#if defined(_WIN32)
#define PYMEDIA_API __declspec(dllexport)
#else
#define PYMEDIA_API __attribute__((visibility("default")))
#endif

// FFmpeg 5.1+ introduced AVChannelLayout (ch_layout) and deprecated
// the old channel_layout / channels integer fields.
// LIBAVCODEC_VERSION 59.37.100 == FFmpeg 5.1
#define FF_NEW_CHANNEL_LAYOUT \
    (LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100))

// ============================================================
// Common helpers
// ============================================================

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} BufferData;

static int read_packet(void *opaque, uint8_t *buf, int buf_size) {
    BufferData *bd = (BufferData *)opaque;
    int remaining = (int)(bd->size - bd->pos);
    buf_size = FFMIN(buf_size, remaining);
    if (buf_size <= 0)
        return AVERROR_EOF;
    memcpy(buf, bd->data + bd->pos, buf_size);
    bd->pos += buf_size;
    return buf_size;
}

static int64_t seek_packet(void *opaque, int64_t offset, int whence) {
    BufferData *bd = (BufferData *)opaque;
    int64_t new_pos;
    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = (int64_t)bd->pos + offset; break;
        case SEEK_END: new_pos = (int64_t)bd->size + offset; break;
        case AVSEEK_SIZE: return (int64_t)bd->size;
        default: return AVERROR(EINVAL);
    }
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (int64_t)bd->size) new_pos = (int64_t)bd->size;
    bd->pos = (size_t)new_pos;
    return (int64_t)bd->pos;
}

// Open input from memory. Returns 0 on success.
static int open_input_memory(const uint8_t *data, size_t size,
                             AVFormatContext **ifmt_ctx,
                             AVIOContext **input_avio_ctx,
                             BufferData *bd) {
    bd->data = data;
    bd->size = size;
    bd->pos = 0;

    uint8_t *avio_buf = av_malloc(32768);
    if (!avio_buf) return -1;

    *input_avio_ctx = avio_alloc_context(avio_buf, 32768, 0, bd,
                                         read_packet, NULL, seek_packet);
    if (!*input_avio_ctx) { av_free(avio_buf); return -1; }

    *ifmt_ctx = avformat_alloc_context();
    if (!*ifmt_ctx) return -1;
    (*ifmt_ctx)->pb = *input_avio_ctx;

    if (avformat_open_input(ifmt_ctx, NULL, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(*ifmt_ctx, NULL) < 0) return -1;

    return 0;
}

static void close_input(AVFormatContext **ifmt_ctx, AVIOContext **input_avio_ctx) {
    if (*ifmt_ctx) avformat_close_input(ifmt_ctx);
    if (*input_avio_ctx) {
        av_freep(&(*input_avio_ctx)->buffer);
        avio_context_free(input_avio_ctx);
    }
}

static int find_stream(AVFormatContext *fmt_ctx, enum AVMediaType type) {
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == type)
            return (int)i;
    }
    return -1;
}

// Read packets until one from target stream is found. Returns:
//  1 -> packet filled
//  0 -> EOF
// -1 -> error
static int read_next_stream_packet(AVFormatContext *fmt_ctx, int stream_idx, AVPacket *pkt) {
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            return 1;
        }
        av_packet_unref(pkt);
    }
    return 0;
}

static void blend_rgba_overlay(AVFrame *dst_rgba, const uint8_t *wm_rgba,
                               int wm_w, int wm_h, int wm_linesize,
                               int pos_x, int pos_y, double opacity) {
    if (!dst_rgba || !wm_rgba) return;
    if (opacity <= 0.0) return;
    if (opacity > 1.0) opacity = 1.0;

    for (int wy = 0; wy < wm_h; wy++) {
        int dy = pos_y + wy;
        if (dy < 0 || dy >= dst_rgba->height) continue;

        const uint8_t *src_row = wm_rgba + wy * wm_linesize;
        uint8_t *dst_row = dst_rgba->data[0] + dy * dst_rgba->linesize[0];

        for (int wx = 0; wx < wm_w; wx++) {
            int dx = pos_x + wx;
            if (dx < 0 || dx >= dst_rgba->width) continue;

            const uint8_t *sp = src_row + wx * 4;
            uint8_t *dp = dst_row + dx * 4;

            double a = (sp[3] / 255.0) * opacity;
            if (a <= 0.0) continue;
            double ia = 1.0 - a;

            dp[0] = (uint8_t)(dp[0] * ia + sp[0] * a + 0.5);
            dp[1] = (uint8_t)(dp[1] * ia + sp[1] * a + 0.5);
            dp[2] = (uint8_t)(dp[2] * ia + sp[2] * a + 0.5);
            dp[3] = 255;
        }
    }
}

typedef struct {
    double start_sec;
    double end_sec;
    char *text;
} SubtitleCue;

static void draw_rgba_rect(AVFrame *rgba, int x, int y, int w, int h,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!rgba || rgba->format != AV_PIX_FMT_RGBA || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > rgba->width ? rgba->width : x + w;
    int y1 = y + h > rgba->height ? rgba->height : y + h;
    for (int py = y0; py < y1; py++) {
        uint8_t *row = rgba->data[0] + py * rgba->linesize[0];
        for (int px = x0; px < x1; px++) {
            uint8_t *p = row + px * 4;
            double alpha = a / 255.0;
            double inv = 1.0 - alpha;
            p[0] = (uint8_t)(p[0] * inv + r * alpha + 0.5);
            p[1] = (uint8_t)(p[1] * inv + g * alpha + 0.5);
            p[2] = (uint8_t)(p[2] * inv + b * alpha + 0.5);
            p[3] = 255;
        }
    }
}

static void draw_block_subtitle(AVFrame *rgba, const char *text, int margin_bottom, int font_size) {
    if (!rgba || !text || !text[0]) return;
    if (font_size < 10) font_size = 10;
    if (font_size > 48) font_size = 48;
    int cw = font_size / 2;
    int ch = font_size;
    int spacing = cw / 4 + 1;

    int len = (int)strlen(text);
    if (len <= 0) return;
    int max_chars = (rgba->width - 20) / (cw + spacing);
    if (max_chars <= 0) return;
    if (len > max_chars) len = max_chars;

    int text_w = len * (cw + spacing) - spacing;
    int x0 = (rgba->width - text_w) / 2;
    int y0 = rgba->height - margin_bottom - ch - 10;
    if (y0 < 10) y0 = 10;

    draw_rgba_rect(rgba, x0 - 8, y0 - 6, text_w + 16, ch + 12, 0, 0, 0, 160);

    for (int i = 0; i < len; i++) {
        unsigned char uc = (unsigned char)text[i];
        if (uc == ' ') continue;
        int gx = x0 + i * (cw + spacing);
        int gy = y0;
        for (int py = 0; py < ch; py++) {
            for (int px = 0; px < cw; px++) {
                int edge = (px == 0 || py == 0 || px == cw - 1 || py == ch - 1);
                int pattern = (((uc * 17 + px * 11 + py * 7) & 31) < 8);
                if (edge || pattern) {
                    draw_rgba_rect(rgba, gx + px, gy + py, 1, 1, 255, 255, 255, 230);
                }
            }
        }
    }
}

static double parse_srt_timestamp(const char *s) {
    if (!s) return -1.0;
    int hh = 0, mm = 0, ss = 0, ms = 0;
    if (sscanf(s, "%d:%d:%d,%d", &hh, &mm, &ss, &ms) != 4) return -1.0;
    return hh * 3600.0 + mm * 60.0 + ss + ms / 1000.0;
}

static SubtitleCue *parse_srt_cues(const char *srt_text, int *out_count) {
    *out_count = 0;
    if (!srt_text || !srt_text[0]) return NULL;

    char *buf = strdup(srt_text);
    if (!buf) return NULL;

    SubtitleCue *cues = NULL;
    int cap = 0;
    int count = 0;
    char *line = buf;

    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        while (*line == '\r' || *line == '\n' || *line == ' ' || *line == '\t') line++;
        if (!*line) { line = next; continue; }

        char *arrow = strstr(line, "-->");
        if (!arrow) {
            line = next;
            if (line && strstr(line, "-->")) continue;
            continue;
        }

        *arrow = '\0';
        char *start_s = line;
        char *end_s = arrow + 3;
        while (*end_s == ' ' || *end_s == '\t') end_s++;

        double start = parse_srt_timestamp(start_s);
        double end = parse_srt_timestamp(end_s);
        if (start < 0.0 || end <= start) {
            line = next;
            continue;
        }

        char text_accum[1024];
        text_accum[0] = '\0';
        size_t text_len = 0;
        char *scan = next;
        while (scan && *scan) {
            char *scan_next = strchr(scan, '\n');
            if (scan_next) *scan_next++ = '\0';
            while (*scan == '\r') scan++;
            if (!*scan) {
                next = scan_next;
                break;
            }
            size_t l = strlen(scan);
            if (text_len + l + 2 < sizeof(text_accum)) {
                if (text_len > 0) text_accum[text_len++] = ' ';
                memcpy(text_accum + text_len, scan, l);
                text_len += l;
                text_accum[text_len] = '\0';
            }
            scan = scan_next;
            next = scan_next;
        }

        if (text_len > 0) {
            if (count >= cap) {
                int new_cap = cap ? cap * 2 : 16;
                SubtitleCue *tmp = realloc(cues, new_cap * sizeof(SubtitleCue));
                if (!tmp) break;
                cues = tmp;
                cap = new_cap;
            }
            cues[count].start_sec = start;
            cues[count].end_sec = end;
            cues[count].text = strdup(text_accum);
            if (!cues[count].text) break;
            count++;
        }
        line = next;
    }

    free(buf);
    *out_count = count;
    return cues;
}

static void free_srt_cues(SubtitleCue *cues, int count) {
    if (!cues) return;
    for (int i = 0; i < count; i++) free(cues[i].text);
    free(cues);
}

static const char *active_subtitle_text(SubtitleCue *cues, int cue_count, double t_sec, int *hint_idx) {
    if (!cues || cue_count <= 0) return NULL;
    int i = (hint_idx && *hint_idx >= 0) ? *hint_idx : 0;
    while (i < cue_count && t_sec > cues[i].end_sec) i++;
    if (hint_idx) *hint_idx = i;
    if (i >= cue_count) return NULL;
    if (t_sec >= cues[i].start_sec && t_sec <= cues[i].end_sec) return cues[i].text;
    return NULL;
}

static void copy_yuv420_frame(AVFrame *dst, const AVFrame *src, int w, int h) {
    for (int p = 0; p < 3; p++) {
        int plane_h = (p == 0) ? h : h / 2;
        int plane_w = (p == 0) ? w : w / 2;
        for (int y = 0; y < plane_h; y++) {
            memcpy(dst->data[p] + y * dst->linesize[p],
                   src->data[p] + y * src->linesize[p],
                   plane_w);
        }
    }
}

static void fill_yuv420_frame(AVFrame *frame, int w, int h, uint8_t yv, uint8_t uv, uint8_t vv) {
    for (int y = 0; y < h; y++) {
        memset(frame->data[0] + y * frame->linesize[0], yv, w);
    }
    int cw = w / 2, ch = h / 2;
    for (int y = 0; y < ch; y++) {
        memset(frame->data[1] + y * frame->linesize[1], uv, cw);
        memset(frame->data[2] + y * frame->linesize[2], vv, cw);
    }
}

static void flip_yuv420_frame(AVFrame *dst, const AVFrame *src, int w, int h,
                              int horizontal, int vertical) {
    for (int plane = 0; plane < 3; plane++) {
        int pw = (plane == 0) ? w : w / 2;
        int ph = (plane == 0) ? h : h / 2;
        for (int y = 0; y < ph; y++) {
            int sy = vertical ? (ph - 1 - y) : y;
            const uint8_t *src_row = src->data[plane] + sy * src->linesize[plane];
            uint8_t *dst_row = dst->data[plane] + y * dst->linesize[plane];
            if (!horizontal) {
                memcpy(dst_row, src_row, pw);
                continue;
            }
            for (int x = 0; x < pw; x++) {
                dst_row[x] = src_row[pw - 1 - x];
            }
        }
    }
}

static void blend_yuv420_frames(AVFrame *dst, const AVFrame *a, const AVFrame *b,
                                int w, int h, double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    int wb = (int)(t * 1024.0 + 0.5);
    int wa = 1024 - wb;
    for (int p = 0; p < 3; p++) {
        int plane_h = (p == 0) ? h : h / 2;
        int plane_w = (p == 0) ? w : w / 2;
        for (int y = 0; y < plane_h; y++) {
            uint8_t *d = dst->data[p] + y * dst->linesize[p];
            const uint8_t *pa = a->data[p] + y * a->linesize[p];
            const uint8_t *pb = b->data[p] + y * b->linesize[p];
            for (int x = 0; x < plane_w; x++) {
                d[x] = (uint8_t)((pa[x] * wa + pb[x] * wb) / 1024);
            }
        }
    }
}

static void slide_left_yuv420_frames(AVFrame *dst, const AVFrame *a, const AVFrame *b,
                                     int w, int h, double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    int shift = (int)(t * w + 0.5);
    for (int p = 0; p < 3; p++) {
        int plane_h = (p == 0) ? h : h / 2;
        int plane_w = (p == 0) ? w : w / 2;
        int plane_shift = (p == 0) ? shift : shift / 2;
        if (plane_shift < 0) plane_shift = 0;
        if (plane_shift > plane_w) plane_shift = plane_w;
        for (int y = 0; y < plane_h; y++) {
            uint8_t *d = dst->data[p] + y * dst->linesize[p];
            const uint8_t *pa = a->data[p] + y * a->linesize[p];
            const uint8_t *pb = b->data[p] + y * b->linesize[p];
            for (int x = 0; x < plane_w; x++) {
                int src_a = x + plane_shift;
                int src_b = x - (plane_w - plane_shift);
                if (src_a < plane_w) d[x] = pa[src_a];
                else if (src_b >= 0) d[x] = pb[src_b];
                else d[x] = pa[plane_w - 1];
            }
        }
    }
}

static int decode_first_frame_to_yuv420(uint8_t *data, size_t size, int out_w, int out_h, AVFrame **out_frame) {
    *out_frame = NULL;
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL;
    AVFrame *dec_frame = NULL;
    AVFrame *yuv_frame = NULL;
    int ret = -1;

    if (open_input_memory(data, size, &ifmt_ctx, &input_avio_ctx, &bd) < 0) goto cleanup;
    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;
    AVCodecParameters *par = ifmt_ctx->streams[video_idx]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(par->codec_id);
    if (!decoder) goto cleanup;
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) goto cleanup;
    avcodec_parameters_to_context(dec_ctx, par);
    if (avcodec_open2(dec_ctx, decoder, NULL) < 0) goto cleanup;

    sws = sws_getContext(par->width, par->height, dec_ctx->pix_fmt,
                         out_w, out_h, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    yuv_frame = av_frame_alloc();
    if (!pkt || !dec_frame || !yuv_frame) goto cleanup;
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = out_w;
    yuv_frame->height = out_h;
    if (av_frame_get_buffer(yuv_frame, 0) < 0) goto cleanup;

    int found = 0;
    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(dec_ctx, pkt) >= 0 &&
                avcodec_receive_frame(dec_ctx, dec_frame) == 0) {
                found = 1;
                av_packet_unref(pkt);
                break;
            }
        }
        av_packet_unref(pkt);
    }
    if (!found) goto cleanup;

    sws_scale(sws, (const uint8_t *const *)dec_frame->data, dec_frame->linesize,
              0, par->height, yuv_frame->data, yuv_frame->linesize);
    *out_frame = yuv_frame;
    yuv_frame = NULL;
    ret = 0;

cleanup:
    if (yuv_frame) av_frame_free(&yuv_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (pkt) av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    close_input(&ifmt_ctx, &input_avio_ctx);
    return ret;
}

static int str_eq_nocase(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

// ============================================================
// 1. get_video_info — returns malloc'd JSON string
// ============================================================

PYMEDIA_API char* get_video_info(uint8_t *video_data, size_t video_size) {
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    char *result = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);

    char json[4096];
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "{");

    double duration = ifmt_ctx->duration > 0
        ? ifmt_ctx->duration / (double)AV_TIME_BASE : 0.0;
    pos += snprintf(json + pos, sizeof(json) - pos, "\"duration\":%.3f", duration);
    pos += snprintf(json + pos, sizeof(json) - pos, ",\"bitrate\":%ld",
                    (long)ifmt_ctx->bit_rate);
    pos += snprintf(json + pos, sizeof(json) - pos, ",\"num_streams\":%u",
                    ifmt_ctx->nb_streams);
    pos += snprintf(json + pos, sizeof(json) - pos, ",\"has_video\":%s",
                    video_idx >= 0 ? "true" : "false");
    pos += snprintf(json + pos, sizeof(json) - pos, ",\"has_audio\":%s",
                    audio_idx >= 0 ? "true" : "false");

    if (video_idx >= 0) {
        AVCodecParameters *vpar = ifmt_ctx->streams[video_idx]->codecpar;
        const AVCodecDescriptor *vdesc = avcodec_descriptor_get(vpar->codec_id);
        AVRational fps = av_guess_frame_rate(ifmt_ctx,
                                             ifmt_ctx->streams[video_idx], NULL);
        pos += snprintf(json + pos, sizeof(json) - pos,
                        ",\"width\":%d,\"height\":%d", vpar->width, vpar->height);
        pos += snprintf(json + pos, sizeof(json) - pos,
                        ",\"video_codec\":\"%s\"", vdesc ? vdesc->name : "unknown");
        pos += snprintf(json + pos, sizeof(json) - pos,
                        ",\"fps\":%.3f",
                        fps.den > 0 ? (double)fps.num / fps.den : 0.0);
    }

    if (audio_idx >= 0) {
        AVCodecParameters *apar = ifmt_ctx->streams[audio_idx]->codecpar;
        const AVCodecDescriptor *adesc = avcodec_descriptor_get(apar->codec_id);
        pos += snprintf(json + pos, sizeof(json) - pos,
                        ",\"audio_codec\":\"%s\"", adesc ? adesc->name : "unknown");
        pos += snprintf(json + pos, sizeof(json) - pos,
                        ",\"sample_rate\":%d", apar->sample_rate);
        pos += snprintf(json + pos, sizeof(json) - pos,
#if FF_NEW_CHANNEL_LAYOUT
                        ",\"channels\":%d", apar->ch_layout.nb_channels);
#else
                        ",\"channels\":%d", apar->channels);
#endif
    }

    pos += snprintf(json + pos, sizeof(json) - pos, "}");
    result = strdup(json);

cleanup:
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

PYMEDIA_API char* list_keyframes_json(uint8_t *video_data, size_t video_size) {
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVPacket *pkt = NULL;
    char *json = NULL;
    size_t cap = 256;
    size_t len = 0;
    int first = 1;

    json = malloc(cap);
    if (!json) return NULL;
    json[len++] = '[';
    json[len] = '\0';

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;
    AVStream *vs = ifmt_ctx->streams[video_idx];

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx && (pkt->flags & AV_PKT_FLAG_KEY)) {
            int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            if (ts != AV_NOPTS_VALUE) {
                double t = ts * av_q2d(vs->time_base);
                char item[64];
                int n = snprintf(item, sizeof(item), first ? "%.6f" : ",%.6f", t);
                if ((size_t)(len + n + 2) >= cap) {
                    while ((size_t)(len + n + 2) >= cap) cap *= 2;
                    char *tmp = realloc(json, cap);
                    if (!tmp) goto cleanup;
                    json = tmp;
                }
                memcpy(json + len, item, n);
                len += (size_t)n;
                json[len] = '\0';
                first = 0;
            }
        }
        av_packet_unref(pkt);
    }

    if (len + 2 >= cap) {
        cap += 2;
        char *tmp = realloc(json, cap);
        if (!tmp) goto cleanup;
        json = tmp;
    }
    json[len++] = ']';
    json[len] = '\0';

cleanup:
    if (pkt) av_packet_free(&pkt);
    close_input(&ifmt_ctx, &input_avio_ctx);
    return json;
}


// ============================================================
// Split module includes
// ============================================================

#include "modules/audio.c"
#include "modules/video_core.c"
#include "modules/video_effects.c"
#include "modules/transforms.c"
#include "modules/metadata.c"
#include "modules/subtitles_tracks.c"
#include "modules/filters.c"
#include "modules/streaming.c"
