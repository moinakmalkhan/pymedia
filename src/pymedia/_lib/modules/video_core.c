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

PYMEDIA_API uint8_t* convert_format(uint8_t *video_data, size_t video_size,
                        const char *format, size_t *out_size) {
    return remux_video(video_data, video_size, format, -1, -1, 1, 1, out_size);
}

PYMEDIA_API uint8_t* trim_video(uint8_t *video_data, size_t video_size,
                    double start_sec, double end_sec, size_t *out_size) {
    return remux_video(video_data, video_size, NULL,
                       start_sec, end_sec, 1, 1, out_size);
}

PYMEDIA_API uint8_t* mute_video(uint8_t *video_data, size_t video_size,
                    size_t *out_size) {
    return remux_video(video_data, video_size, NULL, -1, -1, 0, 1, out_size);
}

// ============================================================
// 4. extract_frame — extract a single frame as JPEG/PNG
// ============================================================

PYMEDIA_API uint8_t* extract_frame(uint8_t *video_data, size_t video_size,
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

PYMEDIA_API uint8_t* reencode_video(uint8_t *video_data, size_t video_size,
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

PYMEDIA_API uint8_t* transcode_video_bitrate(uint8_t *video_data, size_t video_size,
                                             int video_bitrate, int crf, const char *preset,
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

    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) goto cleanup;
    venc_ctx = avcodec_alloc_context3(vencoder);
    venc_ctx->width = src_w;
    venc_ctx->height = src_h;
    venc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    venc_ctx->time_base = ifmt_ctx->streams[video_idx]->time_base;
    AVRational fps = av_guess_frame_rate(ifmt_ctx,
                                         ifmt_ctx->streams[video_idx], NULL);
    if (fps.num > 0 && fps.den > 0)
        venc_ctx->framerate = fps;

    if (video_bitrate > 0) {
        venc_ctx->bit_rate = (int64_t)video_bitrate;
        av_opt_set(venc_ctx->priv_data, "preset", preset, 0);
    } else {
        char crf_str[8];
        snprintf(crf_str, sizeof(crf_str), "%d", crf);
        av_opt_set(venc_ctx->priv_data, "crf", crf_str, 0);
        av_opt_set(venc_ctx->priv_data, "preset", preset, 0);
    }

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;

    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        venc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(venc_ctx, vencoder, NULL) < 0) goto cleanup;

    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    int out_idx = 0;

    AVStream *v_out = avformat_new_stream(ofmt_ctx, NULL);
    avcodec_parameters_from_context(v_out->codecpar, venc_ctx);
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

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

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        if ((int)i != video_idx && (int)i != audio_idx)
            stream_mapping[i] = -1;
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    sws = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                         src_w, src_h, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    scale_frame = av_frame_alloc();
    if (!pkt || !dec_frame || !scale_frame) goto cleanup;

    scale_frame->format = AV_PIX_FMT_YUV420P;
    scale_frame->width = src_w;
    scale_frame->height = src_h;
    av_frame_get_buffer(scale_frame, 0);

    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) goto cleanup;

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
// 6. crop_video — crop to rectangle (re-encode video, copy audio)
// ============================================================

PYMEDIA_API uint8_t* crop_video(uint8_t *video_data, size_t video_size,
                                int crop_x, int crop_y, int crop_w, int crop_h,
                                int crf, const char *preset,
                                size_t *out_size) {
    *out_size = 0;
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL;
    AVCodecContext *venc_ctx = NULL;
    struct SwsContext *sws_full = NULL;
    struct SwsContext *sws_crop = NULL;
    AVPacket *pkt = NULL;
    AVFrame *dec_frame = NULL;
    AVFrame *full_frame = NULL;
    AVFrame *crop_frame = NULL;
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

    if (crop_x < 0 || crop_y < 0 || crop_w <= 0 || crop_h <= 0)
        goto cleanup;

    // YUV420 requires even-aligned crop geometry.
    crop_x &= ~1;
    crop_y &= ~1;
    crop_w &= ~1;
    crop_h &= ~1;

    if (crop_w <= 0 || crop_h <= 0) goto cleanup;
    if (crop_x + crop_w > src_w || crop_y + crop_h > src_h) goto cleanup;

    // Video decoder
    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    if (!vdec_ctx) goto cleanup;
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    // Video encoder (H.264)
    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) goto cleanup;
    venc_ctx = avcodec_alloc_context3(vencoder);
    if (!venc_ctx) goto cleanup;
    venc_ctx->width = crop_w;
    venc_ctx->height = crop_h;
    venc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    venc_ctx->time_base = ifmt_ctx->streams[video_idx]->time_base;
    {
        AVRational fps = av_guess_frame_rate(ifmt_ctx, ifmt_ctx->streams[video_idx], NULL);
        if (fps.num > 0 && fps.den > 0) venc_ctx->framerate = fps;
    }

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
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    AVStream *v_out = avformat_new_stream(ofmt_ctx, NULL);
    if (!v_out) goto cleanup;
    if (avcodec_parameters_from_context(v_out->codecpar, venc_ctx) < 0) goto cleanup;
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

    int audio_out_idx = -1;
    if (audio_idx >= 0) {
        AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
        if (!a_out) goto cleanup;
        if (avcodec_parameters_copy(a_out->codecpar,
                                    ifmt_ctx->streams[audio_idx]->codecpar) < 0)
            goto cleanup;
        a_out->codecpar->codec_tag = 0;
        a_out->time_base = ifmt_ctx->streams[audio_idx]->time_base;
        audio_out_idx = out_idx++;
        stream_mapping[audio_idx] = audio_out_idx;
    }

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        if ((int)i != video_idx && (int)i != audio_idx)
            stream_mapping[i] = -1;
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    // Convert decoded frames to YUV420P at source size, then crop region.
    sws_full = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                              src_w, src_h, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_full) goto cleanup;

    sws_crop = sws_getContext(crop_w, crop_h, AV_PIX_FMT_YUV420P,
                              crop_w, crop_h, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_crop) goto cleanup;

    pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    full_frame = av_frame_alloc();
    crop_frame = av_frame_alloc();
    if (!pkt || !dec_frame || !full_frame || !crop_frame) goto cleanup;

    full_frame->format = AV_PIX_FMT_YUV420P;
    full_frame->width = src_w;
    full_frame->height = src_h;
    if (av_frame_get_buffer(full_frame, 0) < 0) goto cleanup;

    crop_frame->format = AV_PIX_FMT_YUV420P;
    crop_frame->width = crop_w;
    crop_frame->height = crop_h;
    if (av_frame_get_buffer(crop_frame, 0) < 0) goto cleanup;

    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(full_frame);
                    sws_scale(sws_full,
                              (const uint8_t *const *)dec_frame->data,
                              dec_frame->linesize, 0, src_h,
                              full_frame->data, full_frame->linesize);

                    const uint8_t *crop_data[4] = {0};
                    int crop_linesize[4] = {0};
                    crop_data[0] = full_frame->data[0] + crop_y * full_frame->linesize[0] + crop_x;
                    crop_data[1] = full_frame->data[1] + (crop_y / 2) * full_frame->linesize[1] + (crop_x / 2);
                    crop_data[2] = full_frame->data[2] + (crop_y / 2) * full_frame->linesize[2] + (crop_x / 2);
                    crop_linesize[0] = full_frame->linesize[0];
                    crop_linesize[1] = full_frame->linesize[1];
                    crop_linesize[2] = full_frame->linesize[2];

                    av_frame_make_writable(crop_frame);
                    sws_scale(sws_crop,
                              crop_data, crop_linesize, 0, crop_h,
                              crop_frame->data, crop_frame->linesize);
                    crop_frame->pts = dec_frame->pts;

                    avcodec_send_frame(venc_ctx, crop_frame);
                    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
                        enc_pkt->stream_index = stream_mapping[video_idx];
                        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
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

    avcodec_send_packet(vdec_ctx, NULL);
    while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
        av_frame_make_writable(full_frame);
        sws_scale(sws_full,
                  (const uint8_t *const *)dec_frame->data,
                  dec_frame->linesize, 0, src_h,
                  full_frame->data, full_frame->linesize);

        const uint8_t *crop_data[4] = {0};
        int crop_linesize[4] = {0};
        crop_data[0] = full_frame->data[0] + crop_y * full_frame->linesize[0] + crop_x;
        crop_data[1] = full_frame->data[1] + (crop_y / 2) * full_frame->linesize[1] + (crop_x / 2);
        crop_data[2] = full_frame->data[2] + (crop_y / 2) * full_frame->linesize[2] + (crop_x / 2);
        crop_linesize[0] = full_frame->linesize[0];
        crop_linesize[1] = full_frame->linesize[1];
        crop_linesize[2] = full_frame->linesize[2];

        av_frame_make_writable(crop_frame);
        sws_scale(sws_crop,
                  crop_data, crop_linesize, 0, crop_h,
                  crop_frame->data, crop_frame->linesize);
        crop_frame->pts = dec_frame->pts;

        avcodec_send_frame(venc_ctx, crop_frame);
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
    av_packet_free(&enc_pkt);

    av_write_trailer(ofmt_ctx);
    {
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
    }

cleanup:
    free(stream_mapping);
    if (sws_crop) sws_freeContext(sws_crop);
    if (sws_full) sws_freeContext(sws_full);
    if (crop_frame) av_frame_free(&crop_frame);
    if (full_frame) av_frame_free(&full_frame);
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
// 6b. change_fps — convert to a target constant frame rate
// ============================================================

PYMEDIA_API uint8_t* change_fps(uint8_t *video_data, size_t video_size,
                                double target_fps, int crf, const char *preset,
                                size_t *out_size) {
    *out_size = 0;
    if (target_fps <= 0.0) return NULL;
    if (!preset || preset[0] == '\0') preset = "medium";
    if (crf < 0) crf = 23;
    if (crf > 51) crf = 51;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL, *venc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *yuv_frame = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;
    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width, src_h = in_vpar->height;

    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    if (!vdec_ctx) goto cleanup;
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) goto cleanup;
    venc_ctx = avcodec_alloc_context3(vencoder);
    if (!venc_ctx) goto cleanup;
    venc_ctx->width = src_w;
    venc_ctx->height = src_h;
    venc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    {
        AVRational out_fps_q = av_d2q(target_fps, 100000);
        if (out_fps_q.num <= 0 || out_fps_q.den <= 0) goto cleanup;
        venc_ctx->framerate = out_fps_q;
        venc_ctx->time_base = av_inv_q(out_fps_q);
    }

    char crf_str[8];
    snprintf(crf_str, sizeof(crf_str), "%d", crf);
    av_opt_set(venc_ctx->priv_data, "crf", crf_str, 0);
    av_opt_set(venc_ctx->priv_data, "preset", preset, 0);

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
    if (!v_out) goto cleanup;
    if (avcodec_parameters_from_context(v_out->codecpar, venc_ctx) < 0) goto cleanup;
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

    int audio_out_idx = -1;
    if (audio_idx >= 0) {
        AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
        if (!a_out) goto cleanup;
        if (avcodec_parameters_copy(a_out->codecpar, ifmt_ctx->streams[audio_idx]->codecpar) < 0)
            goto cleanup;
        a_out->codecpar->codec_tag = 0;
        a_out->time_base = ifmt_ctx->streams[audio_idx]->time_base;
        audio_out_idx = out_idx++;
        stream_mapping[audio_idx] = audio_out_idx;
    }

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        if ((int)i != video_idx && (int)i != audio_idx) stream_mapping[i] = -1;
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    sws = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                         src_w, src_h, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    yuv_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame || !yuv_frame) goto cleanup;

    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = src_w;
    yuv_frame->height = src_h;
    if (av_frame_get_buffer(yuv_frame, 0) < 0) goto cleanup;

    AVRational in_fps = av_guess_frame_rate(ifmt_ctx, ifmt_ctx->streams[video_idx], NULL);
    double src_fps = (in_fps.num > 0 && in_fps.den > 0)
        ? ((double)in_fps.num / (double)in_fps.den)
        : target_fps;
    if (src_fps <= 0.0) src_fps = target_fps;
    double ratio = target_fps / src_fps;
    int64_t in_frames = 0;
    int64_t out_frames = 0;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(yuv_frame);
                    sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                              dec_frame->linesize, 0, src_h,
                              yuv_frame->data, yuv_frame->linesize);
                    in_frames++;
                    int64_t should_have = (int64_t)floor((double)in_frames * ratio + 1e-9);
                    while (out_frames < should_have) {
                        yuv_frame->pts = out_frames;
                        avcodec_send_frame(venc_ctx, yuv_frame);
                        while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
                            enc_pkt->stream_index = stream_mapping[video_idx];
                            av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
                            av_interleaved_write_frame(ofmt_ctx, enc_pkt);
                            av_packet_unref(enc_pkt);
                        }
                        out_frames++;
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

    avcodec_send_packet(vdec_ctx, NULL);
    while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
        av_frame_make_writable(yuv_frame);
        sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                  dec_frame->linesize, 0, src_h,
                  yuv_frame->data, yuv_frame->linesize);
        in_frames++;
        int64_t should_have = (int64_t)floor((double)in_frames * ratio + 1e-9);
        while (out_frames < should_have) {
            yuv_frame->pts = out_frames;
            avcodec_send_frame(venc_ctx, yuv_frame);
            while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
                enc_pkt->stream_index = stream_mapping[video_idx];
                av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
                av_interleaved_write_frame(ofmt_ctx, enc_pkt);
                av_packet_unref(enc_pkt);
            }
            out_frames++;
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
            if (result) {
                memcpy(result, output_buffer, output_size);
                *out_size = output_size;
            }
        }
        av_free(output_buffer);
    }

cleanup:
    free(stream_mapping);
    if (yuv_frame) av_frame_free(&yuv_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (pkt) av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
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
// 6c. pad_video — pad video to a larger canvas, copy audio
// ============================================================

PYMEDIA_API uint8_t* pad_video(uint8_t *video_data, size_t video_size,
                               int out_width, int out_height, int pad_x, int pad_y,
                               const char *color, int crf, const char *preset,
                               size_t *out_size) {
    *out_size = 0;
    if (!preset || preset[0] == '\0') preset = "medium";
    if (crf < 0) crf = 23;
    if (crf > 51) crf = 51;
    if (out_width <= 0 || out_height <= 0 || pad_x < 0 || pad_y < 0) return NULL;

    out_width &= ~1;
    out_height &= ~1;
    pad_x &= ~1;
    pad_y &= ~1;

    uint8_t yv = 16, uv = 128, vv = 128; // black
    if (color && str_eq_nocase(color, "white")) {
        yv = 235; uv = 128; vv = 128;
    }

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL, *venc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *src_frame = NULL, *pad_frame = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;
    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width, src_h = in_vpar->height;
    if (out_width < src_w + pad_x || out_height < src_h + pad_y) goto cleanup;

    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    if (!vdec_ctx) goto cleanup;
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) goto cleanup;
    venc_ctx = avcodec_alloc_context3(vencoder);
    if (!venc_ctx) goto cleanup;
    venc_ctx->width = out_width;
    venc_ctx->height = out_height;
    venc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    venc_ctx->time_base = ifmt_ctx->streams[video_idx]->time_base;
    {
        AVRational fps = av_guess_frame_rate(ifmt_ctx, ifmt_ctx->streams[video_idx], NULL);
        if (fps.num > 0 && fps.den > 0) venc_ctx->framerate = fps;
    }

    char crf_str[8];
    snprintf(crf_str, sizeof(crf_str), "%d", crf);
    av_opt_set(venc_ctx->priv_data, "crf", crf_str, 0);
    av_opt_set(venc_ctx->priv_data, "preset", preset, 0);

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
    if (!v_out) goto cleanup;
    if (avcodec_parameters_from_context(v_out->codecpar, venc_ctx) < 0) goto cleanup;
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

    int audio_out_idx = -1;
    if (audio_idx >= 0) {
        AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
        if (!a_out) goto cleanup;
        if (avcodec_parameters_copy(a_out->codecpar, ifmt_ctx->streams[audio_idx]->codecpar) < 0)
            goto cleanup;
        a_out->codecpar->codec_tag = 0;
        a_out->time_base = ifmt_ctx->streams[audio_idx]->time_base;
        audio_out_idx = out_idx++;
        stream_mapping[audio_idx] = audio_out_idx;
    }
    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        if ((int)i != video_idx && (int)i != audio_idx) stream_mapping[i] = -1;
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    sws = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                         src_w, src_h, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    src_frame = av_frame_alloc();
    pad_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame || !src_frame || !pad_frame) goto cleanup;

    src_frame->format = AV_PIX_FMT_YUV420P;
    src_frame->width = src_w;
    src_frame->height = src_h;
    if (av_frame_get_buffer(src_frame, 0) < 0) goto cleanup;

    pad_frame->format = AV_PIX_FMT_YUV420P;
    pad_frame->width = out_width;
    pad_frame->height = out_height;
    if (av_frame_get_buffer(pad_frame, 0) < 0) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(src_frame);
                    sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                              dec_frame->linesize, 0, src_h,
                              src_frame->data, src_frame->linesize);

                    av_frame_make_writable(pad_frame);
                    fill_yuv420_frame(pad_frame, out_width, out_height, yv, uv, vv);
                    for (int y = 0; y < src_h; y++) {
                        memcpy(pad_frame->data[0] + (pad_y + y) * pad_frame->linesize[0] + pad_x,
                               src_frame->data[0] + y * src_frame->linesize[0],
                               src_w);
                    }
                    for (int y = 0; y < src_h / 2; y++) {
                        memcpy(pad_frame->data[1] + (pad_y / 2 + y) * pad_frame->linesize[1] + pad_x / 2,
                               src_frame->data[1] + y * src_frame->linesize[1],
                               src_w / 2);
                        memcpy(pad_frame->data[2] + (pad_y / 2 + y) * pad_frame->linesize[2] + pad_x / 2,
                               src_frame->data[2] + y * src_frame->linesize[2],
                               src_w / 2);
                    }
                    pad_frame->pts = dec_frame->pts;

                    avcodec_send_frame(venc_ctx, pad_frame);
                    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
                        enc_pkt->stream_index = stream_mapping[video_idx];
                        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
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

    avcodec_send_packet(vdec_ctx, NULL);
    while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
        av_frame_make_writable(src_frame);
        sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                  dec_frame->linesize, 0, src_h,
                  src_frame->data, src_frame->linesize);

        av_frame_make_writable(pad_frame);
        fill_yuv420_frame(pad_frame, out_width, out_height, yv, uv, vv);
        for (int y = 0; y < src_h; y++) {
            memcpy(pad_frame->data[0] + (pad_y + y) * pad_frame->linesize[0] + pad_x,
                   src_frame->data[0] + y * src_frame->linesize[0],
                   src_w);
        }
        for (int y = 0; y < src_h / 2; y++) {
            memcpy(pad_frame->data[1] + (pad_y / 2 + y) * pad_frame->linesize[1] + pad_x / 2,
                   src_frame->data[1] + y * src_frame->linesize[1],
                   src_w / 2);
            memcpy(pad_frame->data[2] + (pad_y / 2 + y) * pad_frame->linesize[2] + pad_x / 2,
                   src_frame->data[2] + y * src_frame->linesize[2],
                   src_w / 2);
        }
        pad_frame->pts = dec_frame->pts;

        avcodec_send_frame(venc_ctx, pad_frame);
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
            if (result) {
                memcpy(result, output_buffer, output_size);
                *out_size = output_size;
            }
        }
        av_free(output_buffer);
    }

cleanup:
    free(stream_mapping);
    if (pad_frame) av_frame_free(&pad_frame);
    if (src_frame) av_frame_free(&src_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (pkt) av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
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
// 6d. flip_video — horizontal/vertical mirror, copy audio
// ============================================================

PYMEDIA_API uint8_t* flip_video(uint8_t *video_data, size_t video_size,
                                int horizontal, int vertical, int crf,
                                const char *preset, size_t *out_size) {
    *out_size = 0;
    if (!horizontal && !vertical) return NULL;
    if (!preset || preset[0] == '\0') preset = "medium";
    if (crf < 0) crf = 23;
    if (crf > 51) crf = 51;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL, *venc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *src_frame = NULL, *flip_frame = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;
    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width, src_h = in_vpar->height;

    const AVCodec *vdecoder = avcodec_find_decoder(in_vpar->codec_id);
    if (!vdecoder) goto cleanup;
    vdec_ctx = avcodec_alloc_context3(vdecoder);
    if (!vdec_ctx) goto cleanup;
    avcodec_parameters_to_context(vdec_ctx, in_vpar);
    if (avcodec_open2(vdec_ctx, vdecoder, NULL) < 0) goto cleanup;

    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) goto cleanup;
    venc_ctx = avcodec_alloc_context3(vencoder);
    if (!venc_ctx) goto cleanup;
    venc_ctx->width = src_w;
    venc_ctx->height = src_h;
    venc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    venc_ctx->time_base = ifmt_ctx->streams[video_idx]->time_base;
    {
        AVRational fps = av_guess_frame_rate(ifmt_ctx, ifmt_ctx->streams[video_idx], NULL);
        if (fps.num > 0 && fps.den > 0) venc_ctx->framerate = fps;
    }

    char crf_str[8];
    snprintf(crf_str, sizeof(crf_str), "%d", crf);
    av_opt_set(venc_ctx->priv_data, "crf", crf_str, 0);
    av_opt_set(venc_ctx->priv_data, "preset", preset, 0);

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
    if (!v_out) goto cleanup;
    if (avcodec_parameters_from_context(v_out->codecpar, venc_ctx) < 0) goto cleanup;
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

    int audio_out_idx = -1;
    if (audio_idx >= 0) {
        AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
        if (!a_out) goto cleanup;
        if (avcodec_parameters_copy(a_out->codecpar, ifmt_ctx->streams[audio_idx]->codecpar) < 0)
            goto cleanup;
        a_out->codecpar->codec_tag = 0;
        a_out->time_base = ifmt_ctx->streams[audio_idx]->time_base;
        audio_out_idx = out_idx++;
        stream_mapping[audio_idx] = audio_out_idx;
    }
    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        if ((int)i != video_idx && (int)i != audio_idx) stream_mapping[i] = -1;
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    sws = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                         src_w, src_h, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) goto cleanup;

    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    src_frame = av_frame_alloc();
    flip_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame || !src_frame || !flip_frame) goto cleanup;

    src_frame->format = AV_PIX_FMT_YUV420P;
    src_frame->width = src_w;
    src_frame->height = src_h;
    if (av_frame_get_buffer(src_frame, 0) < 0) goto cleanup;

    flip_frame->format = AV_PIX_FMT_YUV420P;
    flip_frame->width = src_w;
    flip_frame->height = src_h;
    if (av_frame_get_buffer(flip_frame, 0) < 0) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(src_frame);
                    sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                              dec_frame->linesize, 0, src_h,
                              src_frame->data, src_frame->linesize);

                    av_frame_make_writable(flip_frame);
                    flip_yuv420_frame(flip_frame, src_frame, src_w, src_h, horizontal, vertical);
                    flip_frame->pts = dec_frame->pts;

                    avcodec_send_frame(venc_ctx, flip_frame);
                    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
                        enc_pkt->stream_index = stream_mapping[video_idx];
                        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
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

    avcodec_send_packet(vdec_ctx, NULL);
    while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
        av_frame_make_writable(src_frame);
        sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                  dec_frame->linesize, 0, src_h,
                  src_frame->data, src_frame->linesize);

        av_frame_make_writable(flip_frame);
        flip_yuv420_frame(flip_frame, src_frame, src_w, src_h, horizontal, vertical);
        flip_frame->pts = dec_frame->pts;

        avcodec_send_frame(venc_ctx, flip_frame);
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
            if (result) {
                memcpy(result, output_buffer, output_size);
                *out_size = output_size;
            }
        }
        av_free(output_buffer);
    }

cleanup:
    free(stream_mapping);
    if (flip_frame) av_frame_free(&flip_frame);
    if (src_frame) av_frame_free(&src_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (pkt) av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
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

