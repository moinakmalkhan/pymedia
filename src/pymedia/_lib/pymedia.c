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

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

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

// ============================================================
// 1. get_video_info — returns malloc'd JSON string
// ============================================================

char* get_video_info(uint8_t *video_data, size_t video_size) {
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

// ============================================================
// 2. extract_audio — extract audio track to mp3/wav/aac/ogg
// ============================================================

static int get_audio_format_info(const char *format, const char **encoder_name,
                                 const char **muxer_name,
                                 enum AVSampleFormat *sample_fmt,
                                 int *bitrate) {
    if (strcmp(format, "mp3") == 0) {
        *encoder_name = "libmp3lame"; *muxer_name = "mp3";
        *sample_fmt = AV_SAMPLE_FMT_S16P; *bitrate = 128000;
    } else if (strcmp(format, "aac") == 0) {
        *encoder_name = "aac"; *muxer_name = "adts";
        *sample_fmt = AV_SAMPLE_FMT_FLTP; *bitrate = 128000;
    } else if (strcmp(format, "ogg") == 0) {
        *encoder_name = "libvorbis"; *muxer_name = "ogg";
        *sample_fmt = AV_SAMPLE_FMT_FLTP; *bitrate = 128000;
    } else if (strcmp(format, "wav") == 0) {
        *encoder_name = "pcm_s16le"; *muxer_name = "wav";
        *sample_fmt = AV_SAMPLE_FMT_S16; *bitrate = 0;
    } else {
        return -1;
    }
    return 0;
}

static void encode_fifo_frames(AVAudioFifo *fifo, AVCodecContext *enc_ctx,
                                AVFormatContext *ofmt_ctx, AVStream *out_stream,
                                AVPacket *enc_pkt, AVFrame *enc_frame,
                                int frame_size, int64_t *pts_counter) {
    while (av_audio_fifo_size(fifo) >= frame_size) {
        enc_frame->format = enc_ctx->sample_fmt;
#if FF_NEW_CHANNEL_LAYOUT
        av_channel_layout_copy(&enc_frame->ch_layout, &enc_ctx->ch_layout);
#else
        enc_frame->channel_layout = enc_ctx->channel_layout;
#endif
        enc_frame->sample_rate = enc_ctx->sample_rate;
        enc_frame->nb_samples = frame_size;
        av_frame_get_buffer(enc_frame, 0);
        av_audio_fifo_read(fifo, (void **)enc_frame->data, frame_size);
        enc_frame->pts = *pts_counter;
        *pts_counter += frame_size;

        avcodec_send_frame(enc_ctx, enc_frame);
        av_frame_unref(enc_frame);

        while (avcodec_receive_packet(enc_ctx, enc_pkt) == 0) {
            enc_pkt->stream_index = 0;
            av_packet_rescale_ts(enc_pkt, enc_ctx->time_base,
                                 out_stream->time_base);
            av_interleaved_write_frame(ofmt_ctx, enc_pkt);
            av_packet_unref(enc_pkt);
        }
    }
}

static void encode_fifo_remaining(AVAudioFifo *fifo, AVCodecContext *enc_ctx,
                                   AVFormatContext *ofmt_ctx,
                                   AVStream *out_stream, AVPacket *enc_pkt,
                                   AVFrame *enc_frame, int64_t *pts_counter) {
    int remaining = av_audio_fifo_size(fifo);
    if (remaining <= 0) return;

    enc_frame->format = enc_ctx->sample_fmt;
#if FF_NEW_CHANNEL_LAYOUT
    av_channel_layout_copy(&enc_frame->ch_layout, &enc_ctx->ch_layout);
#else
    enc_frame->channel_layout = enc_ctx->channel_layout;
#endif
    enc_frame->sample_rate = enc_ctx->sample_rate;
    enc_frame->nb_samples = remaining;
    av_frame_get_buffer(enc_frame, 0);
    av_audio_fifo_read(fifo, (void **)enc_frame->data, remaining);
    enc_frame->pts = *pts_counter;
    *pts_counter += remaining;

    avcodec_send_frame(enc_ctx, enc_frame);
    av_frame_unref(enc_frame);

    while (avcodec_receive_packet(enc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = 0;
        av_packet_rescale_ts(enc_pkt, enc_ctx->time_base,
                             out_stream->time_base);
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }
}

uint8_t* extract_audio(uint8_t *video_data, size_t video_size,
                       const char *format, size_t *out_size) {
    *out_size = 0;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    AVCodecContext *enc_ctx = NULL;
    SwrContext *swr = NULL;
    AVAudioFifo *fifo = NULL;
    AVPacket *dec_pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *enc_frame = NULL;
    AVIOContext *input_avio_ctx = NULL;
    uint8_t *output_buffer = NULL;
    uint8_t *result = NULL;
    uint8_t **resamp_buf = NULL;
    int resamp_buf_size = 0;
    BufferData bd;

    const char *encoder_name, *muxer_name;
    enum AVSampleFormat enc_sample_fmt;
    int enc_bitrate;
    if (get_audio_format_info(format, &encoder_name, &muxer_name,
                              &enc_sample_fmt, &enc_bitrate) < 0) {
        fprintf(stderr, "Unsupported audio format: %s\n", format);
        return NULL;
    }

    if (open_input_memory(video_data, video_size, &ifmt_ctx,
                          &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);
    if (audio_idx < 0) goto cleanup;

    // Decoder
    AVCodecParameters *codecpar = ifmt_ctx->streams[audio_idx]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) goto cleanup;
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) goto cleanup;
    avcodec_parameters_to_context(dec_ctx, codecpar);
    if (avcodec_open2(dec_ctx, decoder, NULL) < 0) goto cleanup;

    // Encoder
    const AVCodec *encoder = avcodec_find_encoder_by_name(encoder_name);
    if (!encoder) goto cleanup;
    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) goto cleanup;
    enc_ctx->sample_rate = 44100;
    enc_ctx->sample_fmt = enc_sample_fmt;
#if FF_NEW_CHANNEL_LAYOUT
    av_channel_layout_default(&enc_ctx->ch_layout, 2);
#else
    enc_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
    enc_ctx->channels = 2;
#endif
    if (enc_bitrate > 0) enc_ctx->bit_rate = enc_bitrate;
    enc_ctx->time_base = (AVRational){1, 44100};

    const AVOutputFormat *ofmt = av_guess_format(muxer_name, NULL, NULL);
    if (ofmt && (ofmt->flags & AVFMT_GLOBALHEADER))
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc_ctx, encoder, NULL) < 0) goto cleanup;

    int frame_size = enc_ctx->frame_size > 0 ? enc_ctx->frame_size : 1024;

    // Resampler
#if FF_NEW_CHANNEL_LAYOUT
    {
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout in_layout;
        if (dec_ctx->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&in_layout, &dec_ctx->ch_layout);
        else
            av_channel_layout_default(&in_layout, 2);
        swr_alloc_set_opts2(&swr, &out_layout, enc_sample_fmt, 44100,
            &in_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate, 0, NULL);
        av_channel_layout_uninit(&in_layout);
    }
#else
    swr = swr_alloc_set_opts(NULL,
        AV_CH_LAYOUT_STEREO, enc_sample_fmt, 44100,
        dec_ctx->channel_layout ? dec_ctx->channel_layout
            : av_get_default_channel_layout(dec_ctx->channels),
        dec_ctx->sample_fmt, dec_ctx->sample_rate, 0, NULL);
#endif
    if (!swr || swr_init(swr) < 0) goto cleanup;

    fifo = av_audio_fifo_alloc(enc_sample_fmt, 2, frame_size);
    if (!fifo) goto cleanup;

    // Output
    avformat_alloc_output_context2(&ofmt_ctx, NULL, muxer_name, NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!out_stream) goto cleanup;
    avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
    out_stream->time_base = enc_ctx->time_base;
    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    dec_pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    enc_frame = av_frame_alloc();
    if (!dec_pkt || !enc_pkt || !dec_frame || !enc_frame) goto cleanup;

    int64_t pts_counter = 0;

    // Decode + resample + encode loop
    while (av_read_frame(ifmt_ctx, dec_pkt) >= 0) {
        if (dec_pkt->stream_index == audio_idx) {
            if (avcodec_send_packet(dec_ctx, dec_pkt) < 0) {
                av_packet_unref(dec_pkt);
                continue;
            }
            while (avcodec_receive_frame(dec_ctx, dec_frame) == 0) {
                int out_samples = swr_get_out_samples(swr, dec_frame->nb_samples);
                if (out_samples <= 0) continue;
                if (out_samples > resamp_buf_size) {
                    if (resamp_buf) av_freep(&resamp_buf[0]);
                    av_freep(&resamp_buf);
                    av_samples_alloc_array_and_samples(&resamp_buf, NULL, 2,
                        out_samples, enc_sample_fmt, 0);
                    resamp_buf_size = out_samples;
                }
                int converted = swr_convert(swr, resamp_buf, out_samples,
                    (const uint8_t **)dec_frame->data, dec_frame->nb_samples);
                if (converted > 0) {
                    av_audio_fifo_write(fifo, (void **)resamp_buf, converted);
                    encode_fifo_frames(fifo, enc_ctx, ofmt_ctx, out_stream,
                                       enc_pkt, enc_frame, frame_size,
                                       &pts_counter);
                }
            }
        }
        av_packet_unref(dec_pkt);
    }

    // Flush decoder
    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, dec_frame) == 0) {
        int out_samples = swr_get_out_samples(swr, dec_frame->nb_samples);
        if (out_samples <= 0) continue;
        if (out_samples > resamp_buf_size) {
            if (resamp_buf) av_freep(&resamp_buf[0]);
            av_freep(&resamp_buf);
            av_samples_alloc_array_and_samples(&resamp_buf, NULL, 2,
                out_samples, enc_sample_fmt, 0);
            resamp_buf_size = out_samples;
        }
        int converted = swr_convert(swr, resamp_buf, out_samples,
            (const uint8_t **)dec_frame->data, dec_frame->nb_samples);
        if (converted > 0) {
            av_audio_fifo_write(fifo, (void **)resamp_buf, converted);
            encode_fifo_frames(fifo, enc_ctx, ofmt_ctx, out_stream,
                               enc_pkt, enc_frame, frame_size, &pts_counter);
        }
    }

    // Flush resampler
    for (;;) {
        int out_samples = 1024;
        if (out_samples > resamp_buf_size) {
            if (resamp_buf) av_freep(&resamp_buf[0]);
            av_freep(&resamp_buf);
            av_samples_alloc_array_and_samples(&resamp_buf, NULL, 2,
                out_samples, enc_sample_fmt, 0);
            resamp_buf_size = out_samples;
        }
        int converted = swr_convert(swr, resamp_buf, out_samples, NULL, 0);
        if (converted <= 0) break;
        av_audio_fifo_write(fifo, (void **)resamp_buf, converted);
        encode_fifo_frames(fifo, enc_ctx, ofmt_ctx, out_stream,
                           enc_pkt, enc_frame, frame_size, &pts_counter);
    }

    encode_fifo_remaining(fifo, enc_ctx, ofmt_ctx, out_stream,
                          enc_pkt, enc_frame, &pts_counter);

    // Flush encoder
    avcodec_send_frame(enc_ctx, NULL);
    while (avcodec_receive_packet(enc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = 0;
        av_packet_rescale_ts(enc_pkt, enc_ctx->time_base,
                             out_stream->time_base);
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }

    av_write_trailer(ofmt_ctx);
    int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
    ofmt_ctx->pb = NULL;

    if (output_size > 0) {
        result = malloc(output_size);
        if (result) {
            memcpy(result, output_buffer, output_size);
            *out_size = output_size;
        }
    }
    av_free(output_buffer);
    if (resamp_buf) { av_freep(&resamp_buf[0]); av_freep(&resamp_buf); }

cleanup:
    if (enc_frame) av_frame_free(&enc_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (dec_pkt) av_packet_free(&dec_pkt);
    if (fifo) av_audio_fifo_free(fifo);
    if (swr) swr_free(&swr);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            uint8_t *dummy;
            avio_close_dyn_buf(ofmt_ctx->pb, &dummy);
            av_free(dummy);
        }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 3. remux_video — internal helper for convert/trim/mute
// ============================================================

static uint8_t* remux_video(uint8_t *video_data, size_t video_size,
                            const char *out_format,
                            double start_sec, double end_sec,
                            int copy_audio, int copy_video,
                            size_t *out_size) {
    *out_size = 0;
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVPacket *pkt = NULL;
    uint8_t *output_buffer = NULL;
    uint8_t *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx,
                          &input_avio_ctx, &bd) < 0)
        goto cleanup;

    // Determine output format
    const char *format_name = out_format;
    if (!format_name || format_name[0] == '\0') {
        // Try to use input format — take first name before comma
        const char *iname = ifmt_ctx->iformat->name;
        static char fmt_buf[32];
        const char *comma = strchr(iname, ',');
        if (comma) {
            size_t len = comma - iname;
            if (len >= sizeof(fmt_buf)) len = sizeof(fmt_buf) - 1;
            memcpy(fmt_buf, iname, len);
            fmt_buf[len] = '\0';
            // "mov" -> "mp4" for output
            if (strcmp(fmt_buf, "mov") == 0) strcpy(fmt_buf, "mp4");
            format_name = fmt_buf;
        } else {
            format_name = iname;
        }
    }

    avformat_alloc_output_context2(&ofmt_ctx, NULL, format_name, NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    // Map input streams to output streams
    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVCodecParameters *par = ifmt_ctx->streams[i]->codecpar;
        int is_video = (par->codec_type == AVMEDIA_TYPE_VIDEO);
        int is_audio = (par->codec_type == AVMEDIA_TYPE_AUDIO);

        if ((is_video && !copy_video) || (is_audio && !copy_audio)) {
            stream_mapping[i] = -1;
            continue;
        }
        if (!is_video && !is_audio && par->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }

        AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) goto cleanup;
        if (avcodec_parameters_copy(out_stream->codecpar, par) < 0) goto cleanup;
        out_stream->codecpar->codec_tag = 0;
        stream_mapping[i] = out_idx++;
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    // Seek to start if trimming
    if (start_sec > 0.0) {
        int64_t ts = (int64_t)(start_sec * AV_TIME_BASE);
        av_seek_frame(ifmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
    }

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    int64_t end_ts = (end_sec > 0.0)
        ? (int64_t)(end_sec * AV_TIME_BASE) : INT64_MAX;
    int64_t start_ts = (start_sec > 0.0)
        ? (int64_t)(start_sec * AV_TIME_BASE) : 0;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        int si = pkt->stream_index;
        if (si < 0 || (unsigned)si >= ifmt_ctx->nb_streams ||
            stream_mapping[si] < 0) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream *in_stream = ifmt_ctx->streams[si];
        int out_si = stream_mapping[si];
        AVStream *out_stream = ofmt_ctx->streams[out_si];

        // Check time bounds
        if (start_sec > 0.0 || end_sec > 0.0) {
            int64_t pkt_ts_abs = av_rescale_q(
                pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts,
                in_stream->time_base, AV_TIME_BASE_Q);
            if (pkt_ts_abs < start_ts) { av_packet_unref(pkt); continue; }
            if (pkt_ts_abs > end_ts) { av_packet_unref(pkt); break; }
        }

        // Rescale timestamps
        pkt->stream_index = out_si;
        if (start_sec > 0.0) {
            int64_t offset = av_rescale_q((int64_t)(start_sec * AV_TIME_BASE),
                                          AV_TIME_BASE_Q, in_stream->time_base);
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= offset;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= offset;
            if (pkt->pts < 0) pkt->pts = 0;
            if (pkt->dts < 0) pkt->dts = 0;
        }
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;
        av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(ofmt_ctx);
    int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
    ofmt_ctx->pb = NULL;

    if (output_size > 0) {
        result = malloc(output_size);
        if (result) {
            memcpy(result, output_buffer, output_size);
            *out_size = output_size;
        }
    }
    av_free(output_buffer);

cleanup:
    free(stream_mapping);
    if (pkt) av_packet_free(&pkt);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            uint8_t *dummy;
            avio_close_dyn_buf(ofmt_ctx->pb, &dummy);
            av_free(dummy);
        }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

uint8_t* convert_format(uint8_t *video_data, size_t video_size,
                        const char *format, size_t *out_size) {
    return remux_video(video_data, video_size, format, -1, -1, 1, 1, out_size);
}

uint8_t* trim_video(uint8_t *video_data, size_t video_size,
                    double start_sec, double end_sec, size_t *out_size) {
    return remux_video(video_data, video_size, NULL,
                       start_sec, end_sec, 1, 1, out_size);
}

uint8_t* mute_video(uint8_t *video_data, size_t video_size,
                    size_t *out_size) {
    return remux_video(video_data, video_size, NULL, -1, -1, 0, 1, out_size);
}

// ============================================================
// 4. extract_frame — extract a single frame as JPEG/PNG
// ============================================================

uint8_t* extract_frame(uint8_t *video_data, size_t video_size,
                       double timestamp_sec, const char *img_format,
                       size_t *out_size) {
    *out_size = 0;
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    AVCodecContext *enc_ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    AVFrame *rgb_frame = NULL;
    struct SwsContext *sws = NULL;
    uint8_t *output_buffer = NULL;
    uint8_t *result = NULL;
    AVFormatContext *ofmt_ctx = NULL;

    // Determine encoder
    const char *enc_name, *mux_name;
    enum AVPixelFormat target_pix_fmt;
    if (!img_format || strcmp(img_format, "jpeg") == 0 ||
        strcmp(img_format, "jpg") == 0) {
        enc_name = "mjpeg";
        mux_name = "mjpeg";
        target_pix_fmt = AV_PIX_FMT_YUVJ420P;
    } else if (strcmp(img_format, "png") == 0) {
        enc_name = "png";
        mux_name = "image2";
        target_pix_fmt = AV_PIX_FMT_RGB24;
    } else {
        fprintf(stderr, "Unsupported image format: %s\n", img_format);
        return NULL;
    }

    if (open_input_memory(video_data, video_size, &ifmt_ctx,
                          &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;

    // Decoder
    AVCodecParameters *codecpar = ifmt_ctx->streams[video_idx]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) goto cleanup;
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) goto cleanup;
    avcodec_parameters_to_context(dec_ctx, codecpar);
    if (avcodec_open2(dec_ctx, decoder, NULL) < 0) goto cleanup;

    // Seek to target
    if (timestamp_sec > 0.0) {
        int64_t ts = (int64_t)(timestamp_sec * AV_TIME_BASE);
        av_seek_frame(ifmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) goto cleanup;

    // Decode until we get a frame at or after the target timestamp
    int got_frame = 0;
    int64_t target_pts = (int64_t)(timestamp_sec *
        av_q2d(av_inv_q(ifmt_ctx->streams[video_idx]->time_base)));

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(dec_ctx, pkt) >= 0) {
                if (avcodec_receive_frame(dec_ctx, frame) == 0) {
                    got_frame = 1;
                    if (frame->pts >= target_pts || timestamp_sec <= 0.0) {
                        av_packet_unref(pkt);
                        break;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    // If no frame yet, flush decoder
    if (!got_frame) {
        avcodec_send_packet(dec_ctx, NULL);
        if (avcodec_receive_frame(dec_ctx, frame) == 0)
            got_frame = 1;
    }
    if (!got_frame) goto cleanup;

    // Convert pixel format
    int w = frame->width, h = frame->height;
    sws = sws_getContext(w, h, frame->format,
                         w, h, target_pix_fmt,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    rgb_frame = av_frame_alloc();
    if (!rgb_frame) goto cleanup;
    rgb_frame->format = target_pix_fmt;
    rgb_frame->width = w;
    rgb_frame->height = h;
    if (av_frame_get_buffer(rgb_frame, 0) < 0) goto cleanup;

    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, h, rgb_frame->data, rgb_frame->linesize);

    // Encode frame to image
    const AVCodec *img_encoder = avcodec_find_encoder_by_name(enc_name);
    if (!img_encoder) goto cleanup;
    enc_ctx = avcodec_alloc_context3(img_encoder);
    if (!enc_ctx) goto cleanup;
    enc_ctx->width = w;
    enc_ctx->height = h;
    enc_ctx->pix_fmt = target_pix_fmt;
    enc_ctx->time_base = (AVRational){1, 1};
    if (strcmp(enc_name, "mjpeg") == 0)
        enc_ctx->qmin = enc_ctx->qmax = 2; // high quality JPEG
    if (avcodec_open2(enc_ctx, img_encoder, NULL) < 0) goto cleanup;

    // Encode directly to packet (no muxer needed for single image)
    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) goto cleanup;

    rgb_frame->pts = 0;
    avcodec_send_frame(enc_ctx, rgb_frame);
    avcodec_send_frame(enc_ctx, NULL); // flush

    if (avcodec_receive_packet(enc_ctx, enc_pkt) == 0) {
        result = malloc(enc_pkt->size);
        if (result) {
            memcpy(result, enc_pkt->data, enc_pkt->size);
            *out_size = enc_pkt->size;
        }
    }
    av_packet_free(&enc_pkt);

cleanup:
    if (sws) sws_freeContext(sws);
    if (rgb_frame) av_frame_free(&rgb_frame);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            uint8_t *dummy;
            avio_close_dyn_buf(ofmt_ctx->pb, &dummy);
            av_free(dummy);
        }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 5. reencode_video — compress / resize video (H.264 output)
// ============================================================

uint8_t* reencode_video(uint8_t *video_data, size_t video_size,
                        int crf, const char *preset,
                        int out_width, int out_height,
                        size_t *out_size) {
    *out_size = 0;
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL;
    AVCodecContext *venc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL;
    AVFrame *dec_frame = NULL;
    AVFrame *scale_frame = NULL;
    uint8_t *output_buffer = NULL;
    uint8_t *result = NULL;
    int *stream_mapping = NULL;

    if (!preset || preset[0] == '\0') preset = "medium";
    if (crf < 0) crf = 23;
    if (crf > 51) crf = 51;

    if (open_input_memory(video_data, video_size, &ifmt_ctx,
                          &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;
    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width, src_h = in_vpar->height;

    // Determine output dimensions
    if (out_width <= 0 && out_height <= 0) {
        out_width = src_w;
        out_height = src_h;
    } else if (out_width <= 0) {
        out_width = (int)((double)src_w / src_h * out_height + 0.5);
        out_width &= ~1; // ensure even
    } else if (out_height <= 0) {
        out_height = (int)((double)src_h / src_w * out_width + 0.5);
        out_height &= ~1;
    }
    out_width &= ~1;
    out_height &= ~1;

    // Video decoder
    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    // Video encoder (H.264)
    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) {
        fprintf(stderr, "libx264 encoder not found\n");
        goto cleanup;
    }
    venc_ctx = avcodec_alloc_context3(vencoder);
    venc_ctx->width = out_width;
    venc_ctx->height = out_height;
    venc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    venc_ctx->time_base = ifmt_ctx->streams[video_idx]->time_base;
    AVRational fps = av_guess_frame_rate(ifmt_ctx,
                                         ifmt_ctx->streams[video_idx], NULL);
    if (fps.num > 0 && fps.den > 0)
        venc_ctx->framerate = fps;

    char crf_str[8];
    snprintf(crf_str, sizeof(crf_str), "%d", crf);
    av_opt_set(venc_ctx->priv_data, "crf", crf_str, 0);
    av_opt_set(venc_ctx->priv_data, "preset", preset, 0);

    // Output
    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;

    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        venc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(venc_ctx, vencoder, NULL) < 0) goto cleanup;

    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    // Create output streams + mapping
    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    int out_idx = 0;

    // Video output stream
    AVStream *v_out = avformat_new_stream(ofmt_ctx, NULL);
    avcodec_parameters_from_context(v_out->codecpar, venc_ctx);
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

    // Audio output stream (copy)
    int audio_out_idx = -1;
    if (audio_idx >= 0) {
        AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(a_out->codecpar,
                               ifmt_ctx->streams[audio_idx]->codecpar);
        a_out->codecpar->codec_tag = 0;
        a_out->time_base = ifmt_ctx->streams[audio_idx]->time_base;
        audio_out_idx = out_idx++;
        stream_mapping[audio_idx] = audio_out_idx;
    }

    // Mark other streams as unmapped
    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        if ((int)i != video_idx && (int)i != audio_idx)
            stream_mapping[i] = -1;
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    // Scaler
    sws = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                         out_width, out_height, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    scale_frame = av_frame_alloc();
    if (!pkt || !dec_frame || !scale_frame) goto cleanup;

    scale_frame->format = AV_PIX_FMT_YUV420P;
    scale_frame->width = out_width;
    scale_frame->height = out_height;
    av_frame_get_buffer(scale_frame, 0);

    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) goto cleanup;

    // Read, decode video / copy audio
    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(scale_frame);
                    sws_scale(sws,
                        (const uint8_t *const *)dec_frame->data,
                        dec_frame->linesize, 0, src_h,
                        scale_frame->data, scale_frame->linesize);
                    scale_frame->pts = dec_frame->pts;

                    avcodec_send_frame(venc_ctx, scale_frame);
                    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
                        enc_pkt->stream_index = stream_mapping[video_idx];
                        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base,
                                             v_out->time_base);
                        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
                        av_packet_unref(enc_pkt);
                    }
                }
            }
        } else if (pkt->stream_index == audio_idx && audio_out_idx >= 0) {
            AVStream *in_s = ifmt_ctx->streams[audio_idx];
            AVStream *out_s = ofmt_ctx->streams[audio_out_idx];
            pkt->stream_index = audio_out_idx;
            av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
            pkt->pos = -1;
            av_interleaved_write_frame(ofmt_ctx, pkt);
        }
        av_packet_unref(pkt);
    }

    // Flush video decoder + encoder
    avcodec_send_packet(vdec_ctx, NULL);
    while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
        av_frame_make_writable(scale_frame);
        sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                  dec_frame->linesize, 0, src_h,
                  scale_frame->data, scale_frame->linesize);
        scale_frame->pts = dec_frame->pts;
        avcodec_send_frame(venc_ctx, scale_frame);
        while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
            enc_pkt->stream_index = stream_mapping[video_idx];
            av_packet_rescale_ts(enc_pkt, venc_ctx->time_base,
                                 v_out->time_base);
            av_interleaved_write_frame(ofmt_ctx, enc_pkt);
            av_packet_unref(enc_pkt);
        }
    }
    avcodec_send_frame(venc_ctx, NULL);
    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = stream_mapping[video_idx];
        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }
    av_packet_free(&enc_pkt);

    av_write_trailer(ofmt_ctx);
    int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
    ofmt_ctx->pb = NULL;

    if (output_size > 0) {
        result = malloc(output_size);
        if (result) {
            memcpy(result, output_buffer, output_size);
            *out_size = output_size;
        }
    }
    av_free(output_buffer);

cleanup:
    free(stream_mapping);
    if (sws) sws_freeContext(sws);
    if (scale_frame) av_frame_free(&scale_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (pkt) av_packet_free(&pkt);
    if (venc_ctx) avcodec_free_context(&venc_ctx);
    if (vdec_ctx) avcodec_free_context(&vdec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            uint8_t *dummy;
            avio_close_dyn_buf(ofmt_ctx->pb, &dummy);
            av_free(dummy);
        }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 6. video_to_gif
// ============================================================

uint8_t* video_to_gif(uint8_t *video_data, size_t video_size,
                      int fps, int width,
                      double start_sec, double duration_sec,
                      size_t *out_size) {
    *out_size = 0;
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL;
    AVCodecContext *gif_enc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL;
    AVFrame *dec_frame = NULL;
    AVFrame *gif_frame = NULL;
    uint8_t *output_buffer = NULL;
    uint8_t *result = NULL;

    if (fps <= 0) fps = 10;
    if (width <= 0) width = 320;

    if (open_input_memory(video_data, video_size, &ifmt_ctx,
                          &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width, src_h = in_vpar->height;

    // Calculate output height maintaining aspect ratio
    int out_h = (int)((double)src_h / src_w * width + 0.5);
    out_h &= ~1;
    width &= ~1;

    // Video decoder
    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    // GIF encoder
    const AVCodec *gif_enc = avcodec_find_encoder(AV_CODEC_ID_GIF);
    if (!gif_enc) goto cleanup;
    gif_enc_ctx = avcodec_alloc_context3(gif_enc);
    gif_enc_ctx->width = width;
    gif_enc_ctx->height = out_h;
    gif_enc_ctx->pix_fmt = AV_PIX_FMT_RGB8;
    gif_enc_ctx->time_base = (AVRational){1, fps};
    gif_enc_ctx->framerate = (AVRational){fps, 1};
    if (avcodec_open2(gif_enc_ctx, gif_enc, NULL) < 0) goto cleanup;

    // Output GIF muxer
    avformat_alloc_output_context2(&ofmt_ctx, NULL, "gif", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    AVStream *gif_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!gif_stream) goto cleanup;
    avcodec_parameters_from_context(gif_stream->codecpar, gif_enc_ctx);
    gif_stream->time_base = gif_enc_ctx->time_base;
    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    // Scaler: source -> RGB8 at target size
    sws = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                         width, out_h, AV_PIX_FMT_RGB8,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    gif_frame = av_frame_alloc();
    if (!pkt || !dec_frame || !gif_frame) goto cleanup;

    gif_frame->format = AV_PIX_FMT_RGB8;
    gif_frame->width = width;
    gif_frame->height = out_h;
    av_frame_get_buffer(gif_frame, 0);

    // Seek to start
    if (start_sec > 0.0) {
        int64_t ts = (int64_t)(start_sec * AV_TIME_BASE);
        av_seek_frame(ifmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
    }

    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) goto cleanup;

    int64_t frame_count = 0;
    double end_sec = (duration_sec > 0.0) ? start_sec + duration_sec : 1e18;

    // Frame interval: only encode every Nth decoded frame to match target fps
    AVRational src_fps = av_guess_frame_rate(ifmt_ctx,
                                             ifmt_ctx->streams[video_idx], NULL);
    double src_fps_val = (src_fps.den > 0) ? (double)src_fps.num / src_fps.den : 30.0;
    double frame_interval = src_fps_val / fps;
    if (frame_interval < 1.0) frame_interval = 1.0;
    int64_t decoded_count = 0;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // Check time
        AVStream *vs = ifmt_ctx->streams[video_idx];
        double pkt_time = pkt->pts != AV_NOPTS_VALUE
            ? pkt->pts * av_q2d(vs->time_base) : 0;
        if (pkt_time > end_sec) { av_packet_unref(pkt); break; }

        if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
            while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                double frame_time = dec_frame->pts * av_q2d(vs->time_base);
                if (frame_time < start_sec) continue;
                if (frame_time > end_sec) break;

                // Sample at target fps
                if (decoded_count % (int64_t)(frame_interval + 0.5) != 0) {
                    decoded_count++;
                    continue;
                }
                decoded_count++;

                av_frame_make_writable(gif_frame);
                sws_scale(sws,
                    (const uint8_t *const *)dec_frame->data,
                    dec_frame->linesize, 0, src_h,
                    gif_frame->data, gif_frame->linesize);
                gif_frame->pts = frame_count++;

                avcodec_send_frame(gif_enc_ctx, gif_frame);
                while (avcodec_receive_packet(gif_enc_ctx, enc_pkt) == 0) {
                    enc_pkt->stream_index = 0;
                    av_packet_rescale_ts(enc_pkt, gif_enc_ctx->time_base,
                                         gif_stream->time_base);
                    av_interleaved_write_frame(ofmt_ctx, enc_pkt);
                    av_packet_unref(enc_pkt);
                }
            }
        }
        av_packet_unref(pkt);
    }

    // Flush encoder
    avcodec_send_frame(gif_enc_ctx, NULL);
    while (avcodec_receive_packet(gif_enc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = 0;
        av_packet_rescale_ts(enc_pkt, gif_enc_ctx->time_base,
                             gif_stream->time_base);
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }
    av_packet_free(&enc_pkt);

    av_write_trailer(ofmt_ctx);
    int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
    ofmt_ctx->pb = NULL;

    if (output_size > 0) {
        result = malloc(output_size);
        if (result) {
            memcpy(result, output_buffer, output_size);
            *out_size = output_size;
        }
    }
    av_free(output_buffer);

cleanup:
    if (sws) sws_freeContext(sws);
    if (gif_frame) av_frame_free(&gif_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (pkt) av_packet_free(&pkt);
    if (gif_enc_ctx) avcodec_free_context(&gif_enc_ctx);
    if (vdec_ctx) avcodec_free_context(&vdec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            uint8_t *dummy;
            avio_close_dyn_buf(ofmt_ctx->pb, &dummy);
            av_free(dummy);
        }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 7. rotate_video — rotate 90 / 180 / 270 degrees
// ============================================================

// Rotate a YUV420P frame. Returns new AVFrame (caller must av_frame_free).
static AVFrame* rotate_yuv420p_frame(AVFrame *src, int angle) {
    AVFrame *dst = av_frame_alloc();
    if (!dst) return NULL;
    dst->format = AV_PIX_FMT_YUV420P;

    int W = src->width, H = src->height;

    if (angle == 180) {
        dst->width  = W;
        dst->height = H;
        if (av_frame_get_buffer(dst, 0) < 0) { av_frame_free(&dst); return NULL; }
        for (int row = 0; row < H; row++)
            for (int col = 0; col < W; col++)
                dst->data[0][(H-1-row)*dst->linesize[0] + (W-1-col)] =
                    src->data[0][row*src->linesize[0] + col];
        int uW = W/2, uH = H/2;
        for (int row = 0; row < uH; row++)
            for (int col = 0; col < uW; col++) {
                dst->data[1][(uH-1-row)*dst->linesize[1] + (uW-1-col)] =
                    src->data[1][row*src->linesize[1] + col];
                dst->data[2][(uH-1-row)*dst->linesize[2] + (uW-1-col)] =
                    src->data[2][row*src->linesize[2] + col];
            }
    } else if (angle == 90) {
        dst->width  = H;
        dst->height = W;
        if (av_frame_get_buffer(dst, 0) < 0) { av_frame_free(&dst); return NULL; }
        int nW = H, nH = W, sUH = H/2;
        int nUW = nW/2, nUH = nH/2;
        // dst[nr][nc] = src[H-1-nc][nr]
        for (int nr = 0; nr < nH; nr++)
            for (int nc = 0; nc < nW; nc++)
                dst->data[0][nr*dst->linesize[0] + nc] =
                    src->data[0][(H-1-nc)*src->linesize[0] + nr];
        for (int nr = 0; nr < nUH; nr++)
            for (int nc = 0; nc < nUW; nc++) {
                dst->data[1][nr*dst->linesize[1] + nc] =
                    src->data[1][(sUH-1-nc)*src->linesize[1] + nr];
                dst->data[2][nr*dst->linesize[2] + nc] =
                    src->data[2][(sUH-1-nc)*src->linesize[2] + nr];
            }
    } else { // 270
        dst->width  = H;
        dst->height = W;
        if (av_frame_get_buffer(dst, 0) < 0) { av_frame_free(&dst); return NULL; }
        int nW = H, nH = W, sUW = W/2;
        int nUW = nW/2, nUH = nH/2;
        // dst[nr][nc] = src[nc][W-1-nr]
        for (int nr = 0; nr < nH; nr++)
            for (int nc = 0; nc < nW; nc++)
                dst->data[0][nr*dst->linesize[0] + nc] =
                    src->data[0][nc*src->linesize[0] + (W-1-nr)];
        for (int nr = 0; nr < nUH; nr++)
            for (int nc = 0; nc < nUW; nc++) {
                dst->data[1][nr*dst->linesize[1] + nc] =
                    src->data[1][nc*src->linesize[1] + (sUW-1-nr)];
                dst->data[2][nr*dst->linesize[2] + nc] =
                    src->data[2][nc*src->linesize[2] + (sUW-1-nr)];
            }
    }
    return dst;
}

uint8_t* rotate_video(uint8_t *video_data, size_t video_size,
                      int angle, size_t *out_size) {
    *out_size = 0;
    angle = ((angle % 360) + 360) % 360;
    if (angle != 90 && angle != 180 && angle != 270) return NULL;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL, *venc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *yuv_frame = NULL, *rot_frame = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;
    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width, src_h = in_vpar->height;
    int out_w = (angle == 90 || angle == 270) ? src_h : src_w;
    int out_h = (angle == 90 || angle == 270) ? src_w : src_h;
    out_w &= ~1; out_h &= ~1;

    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) goto cleanup;
    venc_ctx = avcodec_alloc_context3(vencoder);
    venc_ctx->width    = out_w;
    venc_ctx->height   = out_h;
    venc_ctx->pix_fmt  = AV_PIX_FMT_YUV420P;
    venc_ctx->time_base = ifmt_ctx->streams[video_idx]->time_base;
    {
        AVRational fps = av_guess_frame_rate(ifmt_ctx, ifmt_ctx->streams[video_idx], NULL);
        if (fps.num > 0 && fps.den > 0) venc_ctx->framerate = fps;
    }
    av_opt_set(venc_ctx->priv_data, "crf",    "18",     0);
    av_opt_set(venc_ctx->priv_data, "preset", "medium", 0);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        venc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(venc_ctx, vencoder, NULL) < 0) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    AVStream *v_out = avformat_new_stream(ofmt_ctx, NULL);
    avcodec_parameters_from_context(v_out->codecpar, venc_ctx);
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

    int audio_out_idx = -1;
    if (audio_idx >= 0) {
        AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(a_out->codecpar, ifmt_ctx->streams[audio_idx]->codecpar);
        a_out->codecpar->codec_tag = 0;
        a_out->time_base = ifmt_ctx->streams[audio_idx]->time_base;
        audio_out_idx = out_idx++;
        stream_mapping[audio_idx] = audio_out_idx;
    }
    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++)
        if ((int)i != video_idx && (int)i != audio_idx)
            stream_mapping[i] = -1;

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    sws = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                         src_w, src_h, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc(); enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc(); yuv_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame || !yuv_frame) goto cleanup;

    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = src_w; yuv_frame->height = src_h;
    if (av_frame_get_buffer(yuv_frame, 0) < 0) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(yuv_frame);
                    sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                              dec_frame->linesize, 0, src_h,
                              yuv_frame->data, yuv_frame->linesize);
                    yuv_frame->pts = dec_frame->pts;
                    rot_frame = rotate_yuv420p_frame(yuv_frame, angle);
                    if (!rot_frame) continue;
                    rot_frame->pts = yuv_frame->pts;
                    avcodec_send_frame(venc_ctx, rot_frame);
                    av_frame_free(&rot_frame); rot_frame = NULL;
                    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
                        enc_pkt->stream_index = stream_mapping[video_idx];
                        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
                        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
                        av_packet_unref(enc_pkt);
                    }
                }
            }
        } else if (pkt->stream_index == audio_idx && audio_out_idx >= 0) {
            pkt->stream_index = audio_out_idx;
            av_packet_rescale_ts(pkt, ifmt_ctx->streams[audio_idx]->time_base,
                                 ofmt_ctx->streams[audio_out_idx]->time_base);
            pkt->pos = -1;
            av_interleaved_write_frame(ofmt_ctx, pkt);
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(vdec_ctx, NULL);
    while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
        av_frame_make_writable(yuv_frame);
        sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                  dec_frame->linesize, 0, src_h, yuv_frame->data, yuv_frame->linesize);
        yuv_frame->pts = dec_frame->pts;
        rot_frame = rotate_yuv420p_frame(yuv_frame, angle);
        if (rot_frame) {
            rot_frame->pts = yuv_frame->pts;
            avcodec_send_frame(venc_ctx, rot_frame);
            av_frame_free(&rot_frame); rot_frame = NULL;
        }
        while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
            enc_pkt->stream_index = stream_mapping[video_idx];
            av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
            av_interleaved_write_frame(ofmt_ctx, enc_pkt);
            av_packet_unref(enc_pkt);
        }
    }
    avcodec_send_frame(venc_ctx, NULL);
    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = stream_mapping[video_idx];
        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }

    av_write_trailer(ofmt_ctx);
    {
        int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
        ofmt_ctx->pb = NULL;
        if (output_size > 0) {
            result = malloc(output_size);
            if (result) { memcpy(result, output_buffer, output_size); *out_size = output_size; }
        }
        av_free(output_buffer);
    }

cleanup:
    free(stream_mapping);
    if (rot_frame)  av_frame_free(&rot_frame);
    if (yuv_frame)  av_frame_free(&yuv_frame);
    if (dec_frame)  av_frame_free(&dec_frame);
    if (enc_pkt)    av_packet_free(&enc_pkt);
    if (pkt)        av_packet_free(&pkt);
    if (sws)        sws_freeContext(sws);
    if (venc_ctx)   avcodec_free_context(&venc_ctx);
    if (vdec_ctx)   avcodec_free_context(&vdec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) { uint8_t *d; avio_close_dyn_buf(ofmt_ctx->pb, &d); av_free(d); }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 8. change_speed — speed up or slow down (PTS rescaling)
// speed > 1.0 = faster, speed < 1.0 = slower
// ============================================================

uint8_t* change_speed(uint8_t *video_data, size_t video_size,
                      double speed, size_t *out_size) {
    *out_size = 0;
    if (speed <= 0.0) return NULL;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVPacket *pkt = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVCodecParameters *par = ifmt_ctx->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
            par->codec_type != AVMEDIA_TYPE_AUDIO) {
            stream_mapping[i] = -1; continue;
        }
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) goto cleanup;
        avcodec_parameters_copy(out_stream->codecpar, par);
        out_stream->codecpar->codec_tag = 0;
        stream_mapping[i] = out_idx++;
    }
    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        int si = pkt->stream_index;
        if (si < 0 || (unsigned)si >= ifmt_ctx->nb_streams || stream_mapping[si] < 0) {
            av_packet_unref(pkt); continue;
        }
        AVStream *in_s  = ifmt_ctx->streams[si];
        int out_si      = stream_mapping[si];
        AVStream *out_s = ofmt_ctx->streams[out_si];

        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts      = (int64_t)(pkt->pts / speed);
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts      = (int64_t)(pkt->dts / speed);
        if (pkt->duration > 0)          pkt->duration  = (int64_t)(pkt->duration / speed);

        pkt->stream_index = out_si;
        av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
        pkt->pos = -1;
        av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(ofmt_ctx);
    {
        int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
        ofmt_ctx->pb = NULL;
        if (output_size > 0) {
            result = malloc(output_size);
            if (result) { memcpy(result, output_buffer, output_size); *out_size = output_size; }
        }
        av_free(output_buffer);
    }

cleanup:
    free(stream_mapping);
    if (pkt) av_packet_free(&pkt);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) { uint8_t *d; avio_close_dyn_buf(ofmt_ctx->pb, &d); av_free(d); }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 9. adjust_volume — change audio volume level
// factor > 1.0 amplifies, < 1.0 reduces, 0.0 = silence
// ============================================================

uint8_t* adjust_volume(uint8_t *video_data, size_t video_size,
                       double factor, size_t *out_size) {
    *out_size = 0;
    if (factor < 0.0) factor = 0.0;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *adec_ctx = NULL, *aenc_ctx = NULL;
    SwrContext *swr = NULL;
    AVAudioFifo *fifo = NULL;
    AVPacket *pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *enc_frame = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;
    uint8_t **resamp_buf = NULL;
    int resamp_buf_size = 0;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);
    if (audio_idx < 0) goto cleanup;
    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);

    AVCodecParameters *apar = ifmt_ctx->streams[audio_idx]->codecpar;
    const AVCodec *adecoder = avcodec_find_decoder(apar->codec_id);
    if (!adecoder) goto cleanup;
    adec_ctx = avcodec_alloc_context3(adecoder);
    avcodec_parameters_to_context(adec_ctx, apar);
    if (avcodec_open2(adec_ctx, adecoder, NULL) < 0) goto cleanup;

    const AVCodec *aencoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!aencoder) goto cleanup;
    aenc_ctx = avcodec_alloc_context3(aencoder);
    aenc_ctx->sample_rate = adec_ctx->sample_rate > 0 ? adec_ctx->sample_rate : 44100;
    aenc_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    aenc_ctx->bit_rate    = 128000;
    aenc_ctx->time_base   = (AVRational){1, aenc_ctx->sample_rate};
#if FF_NEW_CHANNEL_LAYOUT
    if (adec_ctx->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&aenc_ctx->ch_layout, &adec_ctx->ch_layout);
    else
        av_channel_layout_default(&aenc_ctx->ch_layout, 2);
#else
    aenc_ctx->channel_layout = adec_ctx->channel_layout
        ? adec_ctx->channel_layout : AV_CH_LAYOUT_STEREO;
    aenc_ctx->channels = av_get_channel_layout_nb_channels(aenc_ctx->channel_layout);
#endif

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        aenc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(aenc_ctx, aencoder, NULL) < 0) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    int frame_size = aenc_ctx->frame_size > 0 ? aenc_ctx->frame_size : 1024;

#if FF_NEW_CHANNEL_LAYOUT
    {
        AVChannelLayout out_layout, in_layout;
        av_channel_layout_copy(&out_layout, &aenc_ctx->ch_layout);
        if (adec_ctx->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&in_layout, &adec_ctx->ch_layout);
        else
            av_channel_layout_default(&in_layout, 2);
        swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLTP, aenc_ctx->sample_rate,
                            &in_layout, adec_ctx->sample_fmt, adec_ctx->sample_rate, 0, NULL);
        av_channel_layout_uninit(&out_layout);
        av_channel_layout_uninit(&in_layout);
    }
#else
    swr = swr_alloc_set_opts(NULL,
        aenc_ctx->channel_layout, AV_SAMPLE_FMT_FLTP, aenc_ctx->sample_rate,
        adec_ctx->channel_layout ? adec_ctx->channel_layout
            : av_get_default_channel_layout(adec_ctx->channels),
        adec_ctx->sample_fmt, adec_ctx->sample_rate, 0, NULL);
#endif
    if (!swr || swr_init(swr) < 0) goto cleanup;

#if FF_NEW_CHANNEL_LAYOUT
    fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, aenc_ctx->ch_layout.nb_channels, frame_size);
#else
    fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, aenc_ctx->channels, frame_size);
#endif
    if (!fifo) goto cleanup;

    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    int video_out_idx = -1;
    if (video_idx >= 0) {
        AVStream *v_out = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(v_out->codecpar, ifmt_ctx->streams[video_idx]->codecpar);
        v_out->codecpar->codec_tag = 0;
        v_out->time_base = ifmt_ctx->streams[video_idx]->time_base;
        video_out_idx = out_idx++;
        stream_mapping[video_idx] = video_out_idx;
    }
    AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
    avcodec_parameters_from_context(a_out->codecpar, aenc_ctx);
    a_out->time_base = aenc_ctx->time_base;
    int audio_out_idx = out_idx++;
    stream_mapping[audio_idx] = audio_out_idx;

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++)
        if ((int)i != video_idx && (int)i != audio_idx)
            stream_mapping[i] = -1;

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    pkt = av_packet_alloc(); enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc(); enc_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame || !enc_frame) goto cleanup;

    int64_t pts_counter = 0;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx && video_out_idx >= 0) {
            pkt->stream_index = video_out_idx;
            av_packet_rescale_ts(pkt, ifmt_ctx->streams[video_idx]->time_base,
                                 ofmt_ctx->streams[video_out_idx]->time_base);
            pkt->pos = -1;
            av_interleaved_write_frame(ofmt_ctx, pkt);
        } else if (pkt->stream_index == audio_idx) {
            if (avcodec_send_packet(adec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(adec_ctx, dec_frame) == 0) {
                    // Apply volume to float planar samples
                    if (dec_frame->format == AV_SAMPLE_FMT_FLTP) {
#if FF_NEW_CHANNEL_LAYOUT
                        int nch = dec_frame->ch_layout.nb_channels;
#else
                        int nch = dec_frame->channels;
#endif
                        for (int ch = 0; ch < nch; ch++) {
                            float *s = (float *)dec_frame->data[ch];
                            for (int n = 0; n < dec_frame->nb_samples; n++) {
                                float v = s[n] * (float)factor;
                                s[n] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                            }
                        }
                    }
                    int out_samples = swr_get_out_samples(swr, dec_frame->nb_samples);
                    if (out_samples <= 0) continue;
                    if (out_samples > resamp_buf_size) {
                        if (resamp_buf) av_freep(&resamp_buf[0]);
                        av_freep(&resamp_buf);
#if FF_NEW_CHANNEL_LAYOUT
                        av_samples_alloc_array_and_samples(&resamp_buf, NULL,
                            aenc_ctx->ch_layout.nb_channels, out_samples, AV_SAMPLE_FMT_FLTP, 0);
#else
                        av_samples_alloc_array_and_samples(&resamp_buf, NULL,
                            aenc_ctx->channels, out_samples, AV_SAMPLE_FMT_FLTP, 0);
#endif
                        resamp_buf_size = out_samples;
                    }
                    int converted = swr_convert(swr, resamp_buf, out_samples,
                                                (const uint8_t **)dec_frame->data,
                                                dec_frame->nb_samples);
                    if (converted > 0) {
                        av_audio_fifo_write(fifo, (void **)resamp_buf, converted);
                        encode_fifo_frames(fifo, aenc_ctx, ofmt_ctx, a_out,
                                           enc_pkt, enc_frame, frame_size, &pts_counter);
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    avcodec_send_packet(adec_ctx, NULL);
    while (avcodec_receive_frame(adec_ctx, dec_frame) == 0) {
        int out_samples = swr_get_out_samples(swr, dec_frame->nb_samples);
        if (out_samples > 0) {
            if (out_samples > resamp_buf_size) {
                if (resamp_buf) av_freep(&resamp_buf[0]);
                av_freep(&resamp_buf);
#if FF_NEW_CHANNEL_LAYOUT
                av_samples_alloc_array_and_samples(&resamp_buf, NULL,
                    aenc_ctx->ch_layout.nb_channels, out_samples, AV_SAMPLE_FMT_FLTP, 0);
#else
                av_samples_alloc_array_and_samples(&resamp_buf, NULL,
                    aenc_ctx->channels, out_samples, AV_SAMPLE_FMT_FLTP, 0);
#endif
                resamp_buf_size = out_samples;
            }
            int converted = swr_convert(swr, resamp_buf, out_samples,
                                        (const uint8_t **)dec_frame->data, dec_frame->nb_samples);
            if (converted > 0) {
                av_audio_fifo_write(fifo, (void **)resamp_buf, converted);
                encode_fifo_frames(fifo, aenc_ctx, ofmt_ctx, a_out,
                                   enc_pkt, enc_frame, frame_size, &pts_counter);
            }
        }
    }
    encode_fifo_remaining(fifo, aenc_ctx, ofmt_ctx, a_out, enc_pkt, enc_frame, &pts_counter);
    avcodec_send_frame(aenc_ctx, NULL);
    while (avcodec_receive_packet(aenc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = audio_out_idx;
        av_packet_rescale_ts(enc_pkt, aenc_ctx->time_base, a_out->time_base);
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }

    av_write_trailer(ofmt_ctx);
    {
        int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
        ofmt_ctx->pb = NULL;
        if (output_size > 0) {
            result = malloc(output_size);
            if (result) { memcpy(result, output_buffer, output_size); *out_size = output_size; }
        }
        av_free(output_buffer);
    }
    if (resamp_buf) { av_freep(&resamp_buf[0]); av_freep(&resamp_buf); }

cleanup:
    free(stream_mapping);
    if (enc_frame) av_frame_free(&enc_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt)   av_packet_free(&enc_pkt);
    if (pkt)       av_packet_free(&pkt);
    if (fifo)      av_audio_fifo_free(fifo);
    if (swr)       swr_free(&swr);
    if (aenc_ctx)  avcodec_free_context(&aenc_ctx);
    if (adec_ctx)  avcodec_free_context(&adec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) { uint8_t *d; avio_close_dyn_buf(ofmt_ctx->pb, &d); av_free(d); }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 10. merge_videos — concatenate two videos sequentially
// ============================================================

uint8_t* merge_videos(uint8_t *data1, size_t size1,
                      uint8_t *data2, size_t size2,
                      size_t *out_size) {
    *out_size = 0;

    BufferData bd1, bd2;
    AVFormatContext *ifmt1 = NULL, *ifmt2 = NULL, *ofmt_ctx = NULL;
    AVIOContext *avio1 = NULL, *avio2 = NULL;
    AVPacket *pkt = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *map1 = NULL, *map2 = NULL;
    int64_t *last_dts = NULL, *last_dur = NULL, *dts_offset = NULL;

    if (open_input_memory(data1, size1, &ifmt1, &avio1, &bd1) < 0) goto cleanup;
    if (open_input_memory(data2, size2, &ifmt2, &avio2, &bd2) < 0) goto cleanup;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    map1 = calloc(ifmt1->nb_streams, sizeof(int));
    map2 = calloc(ifmt2->nb_streams, sizeof(int));
    if (!map1 || !map2) goto cleanup;

    int out_idx = 0;
    for (unsigned i = 0; i < ifmt1->nb_streams; i++) {
        AVCodecParameters *par = ifmt1->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
            par->codec_type != AVMEDIA_TYPE_AUDIO) { map1[i] = -1; continue; }
        AVStream *out_s = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(out_s->codecpar, par);
        out_s->codecpar->codec_tag = 0;
        map1[i] = out_idx++;
    }

    // Map input2 streams to output by media type
    for (unsigned i = 0; i < ifmt2->nb_streams; i++) {
        map2[i] = -1;
        for (unsigned j = 0; j < ifmt1->nb_streams; j++) {
            if (map1[j] >= 0 &&
                ifmt1->streams[j]->codecpar->codec_type ==
                ifmt2->streams[i]->codecpar->codec_type) {
                map2[i] = map1[j]; break;
            }
        }
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    last_dts   = calloc(out_idx, sizeof(int64_t));
    last_dur   = calloc(out_idx, sizeof(int64_t));
    dts_offset = calloc(out_idx, sizeof(int64_t));
    if (!last_dts || !last_dur || !dts_offset) goto cleanup;

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    // Write input1
    while (av_read_frame(ifmt1, pkt) >= 0) {
        int si = pkt->stream_index;
        if (si < 0 || (unsigned)si >= ifmt1->nb_streams || map1[si] < 0) {
            av_packet_unref(pkt); continue;
        }
        AVStream *in_s  = ifmt1->streams[si];
        int out_si      = map1[si];
        AVStream *out_s = ofmt_ctx->streams[out_si];
        pkt->stream_index = out_si;
        av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
        pkt->pos = -1;
        if (pkt->dts != AV_NOPTS_VALUE) {
            last_dts[out_si] = pkt->dts;
            last_dur[out_si] = pkt->duration > 0 ? pkt->duration : 1;
        }
        av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    for (int i = 0; i < out_idx; i++)
        dts_offset[i] = last_dts[i] + last_dur[i];

    // Write input2 with offset
    while (av_read_frame(ifmt2, pkt) >= 0) {
        int si = pkt->stream_index;
        if (si < 0 || (unsigned)si >= ifmt2->nb_streams || map2[si] < 0) {
            av_packet_unref(pkt); continue;
        }
        AVStream *in_s  = ifmt2->streams[si];
        int out_si      = map2[si];
        AVStream *out_s = ofmt_ctx->streams[out_si];
        av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += dts_offset[out_si];
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += dts_offset[out_si];
        pkt->stream_index = out_si;
        pkt->pos = -1;
        av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(ofmt_ctx);
    {
        int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
        ofmt_ctx->pb = NULL;
        if (output_size > 0) {
            result = malloc(output_size);
            if (result) { memcpy(result, output_buffer, output_size); *out_size = output_size; }
        }
        av_free(output_buffer);
    }

cleanup:
    free(last_dts); free(last_dur); free(dts_offset);
    free(map1); free(map2);
    if (pkt) av_packet_free(&pkt);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) { uint8_t *d; avio_close_dyn_buf(ofmt_ctx->pb, &d); av_free(d); }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt1, &avio1);
    close_input(&ifmt2, &avio2);
    return result;
}

// ============================================================
// 11. reverse_video — reverse video playback (audio dropped)
// ============================================================

uint8_t* reverse_video(uint8_t *video_data, size_t video_size,
                       size_t *out_size) {
    *out_size = 0;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL, *venc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *yuv_frame = NULL;
    AVFrame **frames = NULL;
    int frame_count = 0, frame_cap = 0;
    uint8_t *output_buffer = NULL, *result = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width, src_h = in_vpar->height;

    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) goto cleanup;
    venc_ctx = avcodec_alloc_context3(vencoder);
    venc_ctx->width    = src_w & ~1;
    venc_ctx->height   = src_h & ~1;
    venc_ctx->pix_fmt  = AV_PIX_FMT_YUV420P;
    venc_ctx->time_base = ifmt_ctx->streams[video_idx]->time_base;
    {
        AVRational fps = av_guess_frame_rate(ifmt_ctx, ifmt_ctx->streams[video_idx], NULL);
        if (fps.num > 0 && fps.den > 0) venc_ctx->framerate = fps;
    }
    av_opt_set(venc_ctx->priv_data, "crf",    "18",     0);
    av_opt_set(venc_ctx->priv_data, "preset", "medium", 0);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        venc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(venc_ctx, vencoder, NULL) < 0) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    AVStream *v_out = avformat_new_stream(ofmt_ctx, NULL);
    avcodec_parameters_from_context(v_out->codecpar, venc_ctx);
    v_out->time_base = venc_ctx->time_base;
    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    sws = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                         src_w & ~1, src_h & ~1, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc(); enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame) goto cleanup;

    // Decode all video frames
    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx &&
            avcodec_send_packet(vdec_ctx, pkt) >= 0) {
            while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                if (frame_count >= frame_cap) {
                    int new_cap = frame_cap ? frame_cap * 2 : 64;
                    AVFrame **tmp = realloc(frames, new_cap * sizeof(AVFrame *));
                    if (!tmp) goto encode;
                    frames = tmp; frame_cap = new_cap;
                }
                frames[frame_count] = av_frame_clone(dec_frame);
                if (frames[frame_count]) frame_count++;
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(vdec_ctx, NULL);
    while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
        if (frame_count < frame_cap) {
            frames[frame_count] = av_frame_clone(dec_frame);
            if (frames[frame_count]) frame_count++;
        }
    }

encode:
    yuv_frame = av_frame_alloc();
    if (!yuv_frame) goto cleanup;
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width  = src_w & ~1;
    yuv_frame->height = src_h & ~1;
    if (av_frame_get_buffer(yuv_frame, 0) < 0) goto cleanup;

    for (int i = frame_count - 1; i >= 0; i--) {
        AVFrame *f = frames[i];
        av_frame_make_writable(yuv_frame);
        sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, src_h,
                  yuv_frame->data, yuv_frame->linesize);
        yuv_frame->pts = (int64_t)(frame_count - 1 - i);
        avcodec_send_frame(venc_ctx, yuv_frame);
        while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
            enc_pkt->stream_index = 0;
            av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
            av_interleaved_write_frame(ofmt_ctx, enc_pkt);
            av_packet_unref(enc_pkt);
        }
    }
    avcodec_send_frame(venc_ctx, NULL);
    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = 0;
        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }

    av_write_trailer(ofmt_ctx);
    {
        int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
        ofmt_ctx->pb = NULL;
        if (output_size > 0) {
            result = malloc(output_size);
            if (result) { memcpy(result, output_buffer, output_size); *out_size = output_size; }
        }
        av_free(output_buffer);
    }

cleanup:
    if (frames) {
        for (int i = 0; i < frame_count; i++)
            if (frames[i]) av_frame_free(&frames[i]);
        free(frames);
    }
    if (yuv_frame) av_frame_free(&yuv_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt)   av_packet_free(&enc_pkt);
    if (pkt)       av_packet_free(&pkt);
    if (sws)       sws_freeContext(sws);
    if (venc_ctx)  avcodec_free_context(&venc_ctx);
    if (vdec_ctx)  avcodec_free_context(&vdec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) { uint8_t *d; avio_close_dyn_buf(ofmt_ctx->pb, &d); av_free(d); }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 12. strip_metadata — remove all metadata tags
// ============================================================

uint8_t* strip_metadata(uint8_t *video_data, size_t video_size,
                        size_t *out_size) {
    *out_size = 0;
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVPacket *pkt = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVCodecParameters *par = ifmt_ctx->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
            par->codec_type != AVMEDIA_TYPE_AUDIO) { stream_mapping[i] = -1; continue; }
        AVStream *out_s = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(out_s->codecpar, par);
        out_s->codecpar->codec_tag = 0;
        stream_mapping[i] = out_idx++;
    }
    av_dict_free(&ofmt_ctx->metadata);  // clear container metadata
    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        int si = pkt->stream_index;
        if (si < 0 || (unsigned)si >= ifmt_ctx->nb_streams || stream_mapping[si] < 0) {
            av_packet_unref(pkt); continue;
        }
        AVStream *in_s  = ifmt_ctx->streams[si];
        int out_si      = stream_mapping[si];
        AVStream *out_s = ofmt_ctx->streams[out_si];
        pkt->stream_index = out_si;
        av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
        pkt->pos = -1;
        av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(ofmt_ctx);
    {
        int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
        ofmt_ctx->pb = NULL;
        if (output_size > 0) {
            result = malloc(output_size);
            if (result) { memcpy(result, output_buffer, output_size); *out_size = output_size; }
        }
        av_free(output_buffer);
    }

cleanup:
    free(stream_mapping);
    if (pkt) av_packet_free(&pkt);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) { uint8_t *d; avio_close_dyn_buf(ofmt_ctx->pb, &d); av_free(d); }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 13. set_metadata — set a metadata key/value on the container
// ============================================================

uint8_t* set_metadata(uint8_t *video_data, size_t video_size,
                      const char *key, const char *value,
                      size_t *out_size) {
    *out_size = 0;
    if (!key || !value) return NULL;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVPacket *pkt = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVCodecParameters *par = ifmt_ctx->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
            par->codec_type != AVMEDIA_TYPE_AUDIO) { stream_mapping[i] = -1; continue; }
        AVStream *out_s = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(out_s->codecpar, par);
        out_s->codecpar->codec_tag = 0;
        stream_mapping[i] = out_idx++;
    }
    av_dict_copy(&ofmt_ctx->metadata, ifmt_ctx->metadata, 0);
    av_dict_set(&ofmt_ctx->metadata, key, value, 0);
    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        int si = pkt->stream_index;
        if (si < 0 || (unsigned)si >= ifmt_ctx->nb_streams || stream_mapping[si] < 0) {
            av_packet_unref(pkt); continue;
        }
        AVStream *in_s  = ifmt_ctx->streams[si];
        int out_si      = stream_mapping[si];
        AVStream *out_s = ofmt_ctx->streams[out_si];
        pkt->stream_index = out_si;
        av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
        pkt->pos = -1;
        av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(ofmt_ctx);
    {
        int output_size = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
        ofmt_ctx->pb = NULL;
        if (output_size > 0) {
            result = malloc(output_size);
            if (result) { memcpy(result, output_buffer, output_size); *out_size = output_size; }
        }
        av_free(output_buffer);
    }

cleanup:
    free(stream_mapping);
    if (pkt) av_packet_free(&pkt);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) { uint8_t *d; avio_close_dyn_buf(ofmt_ctx->pb, &d); av_free(d); }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}
