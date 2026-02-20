// ============================================================
// 7. add_watermark — overlay an image watermark and re-encode
// ============================================================

PYMEDIA_API uint8_t* add_watermark(uint8_t *video_data, size_t video_size,
                                   uint8_t *watermark_data, size_t watermark_size,
                                   int pos_x, int pos_y, double opacity,
                                   int crf, const char *preset,
                                   size_t *out_size) {
    *out_size = 0;
    if (!preset || preset[0] == '\0') preset = "medium";
    if (crf < 0) crf = 23;
    if (crf > 51) crf = 51;
    if (opacity <= 0.0) opacity = 0.5;
    if (opacity > 1.0) opacity = 1.0;

    BufferData v_bd, w_bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL, *wfmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL, *w_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL, *venc_ctx = NULL, *wdec_ctx = NULL;
    struct SwsContext *sws_dec_to_rgba = NULL, *sws_rgba_to_yuv = NULL, *sws_wm_to_rgba = NULL;
    AVPacket *pkt = NULL, *w_pkt = NULL;
    AVFrame *dec_frame = NULL, *rgba_frame = NULL, *yuv_frame = NULL, *wm_dec = NULL;
    uint8_t *wm_rgba = NULL, *output_buffer = NULL, *result = NULL;
    int wm_w = 0, wm_h = 0, wm_linesize = 0;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &v_bd) < 0)
        goto cleanup;
    if (open_input_memory(watermark_data, watermark_size, &wfmt_ctx, &w_avio_ctx, &w_bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);
    int wm_video_idx = find_stream(wfmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0 || wm_video_idx < 0) goto cleanup;

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width, src_h = in_vpar->height;

    // Decode watermark first frame -> RGBA buffer.
    {
        AVCodecParameters *wpar = wfmt_ctx->streams[wm_video_idx]->codecpar;
        const AVCodec *wdecoder = avcodec_find_decoder(wpar->codec_id);
        if (!wdecoder) goto cleanup;
        wdec_ctx = avcodec_alloc_context3(wdecoder);
        if (!wdec_ctx) goto cleanup;
        avcodec_parameters_to_context(wdec_ctx, wpar);
        if (avcodec_open2(wdec_ctx, wdecoder, NULL) < 0) goto cleanup;

        w_pkt = av_packet_alloc();
        wm_dec = av_frame_alloc();
        if (!w_pkt || !wm_dec) goto cleanup;

        int found_wm = 0;
        while (av_read_frame(wfmt_ctx, w_pkt) >= 0) {
            if (w_pkt->stream_index != wm_video_idx) {
                av_packet_unref(w_pkt);
                continue;
            }
            if (avcodec_send_packet(wdec_ctx, w_pkt) >= 0 &&
                avcodec_receive_frame(wdec_ctx, wm_dec) == 0) {
                found_wm = 1;
                av_packet_unref(w_pkt);
                break;
            }
            av_packet_unref(w_pkt);
        }
        if (!found_wm) goto cleanup;

        wm_w = wm_dec->width;
        wm_h = wm_dec->height;
        if (wm_w <= 0 || wm_h <= 0) goto cleanup;
        wm_linesize = wm_w * 4;
        wm_rgba = malloc((size_t)wm_linesize * wm_h);
        if (!wm_rgba) goto cleanup;

        sws_wm_to_rgba = sws_getContext(wm_w, wm_h, wdec_ctx->pix_fmt,
                                        wm_w, wm_h, AV_PIX_FMT_RGBA,
                                        SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_wm_to_rgba) goto cleanup;

        uint8_t *dst_data[4] = {wm_rgba, NULL, NULL, NULL};
        int dst_linesize[4] = {wm_linesize, 0, 0, 0};
        sws_scale(sws_wm_to_rgba,
                  (const uint8_t *const *)wm_dec->data, wm_dec->linesize,
                  0, wm_h, dst_data, dst_linesize);
    }

    // Main video decode / encode setup.
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
        if (avcodec_parameters_copy(a_out->codecpar,
                                    ifmt_ctx->streams[audio_idx]->codecpar) < 0)
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

    sws_dec_to_rgba = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                                     src_w, src_h, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, NULL, NULL, NULL);
    sws_rgba_to_yuv = sws_getContext(src_w, src_h, AV_PIX_FMT_RGBA,
                                     src_w, src_h, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_dec_to_rgba || !sws_rgba_to_yuv) goto cleanup;

    pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    rgba_frame = av_frame_alloc();
    yuv_frame = av_frame_alloc();
    if (!pkt || !dec_frame || !rgba_frame || !yuv_frame) goto cleanup;

    rgba_frame->format = AV_PIX_FMT_RGBA;
    rgba_frame->width = src_w;
    rgba_frame->height = src_h;
    if (av_frame_get_buffer(rgba_frame, 0) < 0) goto cleanup;

    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = src_w;
    yuv_frame->height = src_h;
    if (av_frame_get_buffer(yuv_frame, 0) < 0) goto cleanup;

    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(rgba_frame);
                    sws_scale(sws_dec_to_rgba,
                              (const uint8_t *const *)dec_frame->data, dec_frame->linesize,
                              0, src_h, rgba_frame->data, rgba_frame->linesize);

                    blend_rgba_overlay(rgba_frame, wm_rgba, wm_w, wm_h, wm_linesize,
                                       pos_x, pos_y, opacity);

                    av_frame_make_writable(yuv_frame);
                    sws_scale(sws_rgba_to_yuv,
                              (const uint8_t *const *)rgba_frame->data, rgba_frame->linesize,
                              0, src_h, yuv_frame->data, yuv_frame->linesize);
                    yuv_frame->pts = dec_frame->pts;

                    avcodec_send_frame(venc_ctx, yuv_frame);
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
        av_frame_make_writable(rgba_frame);
        sws_scale(sws_dec_to_rgba,
                  (const uint8_t *const *)dec_frame->data, dec_frame->linesize,
                  0, src_h, rgba_frame->data, rgba_frame->linesize);
        blend_rgba_overlay(rgba_frame, wm_rgba, wm_w, wm_h, wm_linesize,
                           pos_x, pos_y, opacity);
        av_frame_make_writable(yuv_frame);
        sws_scale(sws_rgba_to_yuv,
                  (const uint8_t *const *)rgba_frame->data, rgba_frame->linesize,
                  0, src_h, yuv_frame->data, yuv_frame->linesize);
        yuv_frame->pts = dec_frame->pts;

        avcodec_send_frame(venc_ctx, yuv_frame);
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
    if (wm_rgba) free(wm_rgba);
    if (wm_dec) av_frame_free(&wm_dec);
    if (w_pkt) av_packet_free(&w_pkt);
    if (yuv_frame) av_frame_free(&yuv_frame);
    if (rgba_frame) av_frame_free(&rgba_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (pkt) av_packet_free(&pkt);
    if (sws_wm_to_rgba) sws_freeContext(sws_wm_to_rgba);
    if (sws_rgba_to_yuv) sws_freeContext(sws_rgba_to_yuv);
    if (sws_dec_to_rgba) sws_freeContext(sws_dec_to_rgba);
    if (wdec_ctx) avcodec_free_context(&wdec_ctx);
    if (venc_ctx) avcodec_free_context(&venc_ctx);
    if (vdec_ctx) avcodec_free_context(&vdec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            uint8_t *d;
            avio_close_dyn_buf(ofmt_ctx->pb, &d);
            av_free(d);
        }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&wfmt_ctx, &w_avio_ctx);
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 8. video_to_gif
// ============================================================

PYMEDIA_API uint8_t* video_to_gif(uint8_t *video_data, size_t video_size,
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

PYMEDIA_API uint8_t* rotate_video(uint8_t *video_data, size_t video_size,
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

PYMEDIA_API uint8_t* change_speed(uint8_t *video_data, size_t video_size,
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
// 14. replace_audio — use video stream from one input and audio stream from another
// ============================================================

PYMEDIA_API uint8_t* replace_audio(uint8_t *video_data, size_t video_size,
                                   uint8_t *audio_data, size_t audio_size,
                                   int trim_to_video, size_t *out_size) {
    *out_size = 0;

    BufferData video_bd, audio_bd;
    AVFormatContext *v_ifmt = NULL, *a_ifmt = NULL, *ofmt_ctx = NULL;
    AVIOContext *v_avio = NULL, *a_avio = NULL;
    AVPacket *vpkt = NULL, *apkt = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;

    if (open_input_memory(video_data, video_size, &v_ifmt, &v_avio, &video_bd) < 0)
        goto cleanup;
    if (open_input_memory(audio_data, audio_size, &a_ifmt, &a_avio, &audio_bd) < 0)
        goto cleanup;

    int video_idx = find_stream(v_ifmt, AVMEDIA_TYPE_VIDEO);
    int audio_idx = find_stream(a_ifmt, AVMEDIA_TYPE_AUDIO);
    if (video_idx < 0 || audio_idx < 0) goto cleanup;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    AVStream *v_out = avformat_new_stream(ofmt_ctx, NULL);
    AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
    if (!v_out || !a_out) goto cleanup;

    if (avcodec_parameters_copy(v_out->codecpar, v_ifmt->streams[video_idx]->codecpar) < 0)
        goto cleanup;
    if (avcodec_parameters_copy(a_out->codecpar, a_ifmt->streams[audio_idx]->codecpar) < 0)
        goto cleanup;
    v_out->codecpar->codec_tag = 0;
    a_out->codecpar->codec_tag = 0;
    v_out->time_base = v_ifmt->streams[video_idx]->time_base;
    a_out->time_base = a_ifmt->streams[audio_idx]->time_base;

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    vpkt = av_packet_alloc();
    apkt = av_packet_alloc();
    if (!vpkt || !apkt) goto cleanup;

    int got_v = read_next_stream_packet(v_ifmt, video_idx, vpkt);
    int got_a = read_next_stream_packet(a_ifmt, audio_idx, apkt);
    if (got_v < 0 || got_a < 0) goto cleanup;

    int64_t v_first_pts = AV_NOPTS_VALUE, v_first_dts = AV_NOPTS_VALUE;
    int64_t a_first_pts = AV_NOPTS_VALUE, a_first_dts = AV_NOPTS_VALUE;

    double video_duration_sec = (v_ifmt->duration > 0)
        ? (double)v_ifmt->duration / AV_TIME_BASE
        : 1e18;

    while (got_v == 1 || got_a == 1) {
        int use_video = 0;
        if (got_v == 1 && got_a == 1) {
            AVRational vtb = v_ifmt->streams[video_idx]->time_base;
            AVRational atb = a_ifmt->streams[audio_idx]->time_base;
            int64_t v_ref = (vpkt->pts != AV_NOPTS_VALUE) ? vpkt->pts : vpkt->dts;
            int64_t a_ref = (apkt->pts != AV_NOPTS_VALUE) ? apkt->pts : apkt->dts;
            double v_sec = (v_ref != AV_NOPTS_VALUE) ? v_ref * av_q2d(vtb) : 0.0;
            double a_sec = (a_ref != AV_NOPTS_VALUE) ? a_ref * av_q2d(atb) : 0.0;
            use_video = (v_sec <= a_sec);
        } else if (got_v == 1) {
            use_video = 1;
        } else {
            use_video = 0;
        }

        if (use_video) {
            if (v_first_pts == AV_NOPTS_VALUE && vpkt->pts != AV_NOPTS_VALUE) v_first_pts = vpkt->pts;
            if (v_first_dts == AV_NOPTS_VALUE && vpkt->dts != AV_NOPTS_VALUE) v_first_dts = vpkt->dts;
            if (vpkt->pts != AV_NOPTS_VALUE && v_first_pts != AV_NOPTS_VALUE) vpkt->pts -= v_first_pts;
            if (vpkt->dts != AV_NOPTS_VALUE && v_first_dts != AV_NOPTS_VALUE) vpkt->dts -= v_first_dts;
            if (vpkt->pts != AV_NOPTS_VALUE && vpkt->pts < 0) vpkt->pts = 0;
            if (vpkt->dts != AV_NOPTS_VALUE && vpkt->dts < 0) vpkt->dts = 0;

            vpkt->stream_index = v_out->index;
            av_packet_rescale_ts(vpkt, v_ifmt->streams[video_idx]->time_base, v_out->time_base);
            vpkt->pos = -1;
            av_interleaved_write_frame(ofmt_ctx, vpkt);
            av_packet_unref(vpkt);
            got_v = read_next_stream_packet(v_ifmt, video_idx, vpkt);
            if (got_v < 0) goto cleanup;
        } else {
            int skip_audio = 0;
            if (trim_to_video) {
                int64_t a_ref = (apkt->pts != AV_NOPTS_VALUE) ? apkt->pts : apkt->dts;
                if (a_ref != AV_NOPTS_VALUE) {
                    double a_sec = a_ref * av_q2d(a_ifmt->streams[audio_idx]->time_base);
                    if (a_sec > video_duration_sec) skip_audio = 1;
                }
            }

            if (!skip_audio) {
                if (a_first_pts == AV_NOPTS_VALUE && apkt->pts != AV_NOPTS_VALUE) a_first_pts = apkt->pts;
                if (a_first_dts == AV_NOPTS_VALUE && apkt->dts != AV_NOPTS_VALUE) a_first_dts = apkt->dts;
                if (apkt->pts != AV_NOPTS_VALUE && a_first_pts != AV_NOPTS_VALUE) apkt->pts -= a_first_pts;
                if (apkt->dts != AV_NOPTS_VALUE && a_first_dts != AV_NOPTS_VALUE) apkt->dts -= a_first_dts;
                if (apkt->pts != AV_NOPTS_VALUE && apkt->pts < 0) apkt->pts = 0;
                if (apkt->dts != AV_NOPTS_VALUE && apkt->dts < 0) apkt->dts = 0;

                apkt->stream_index = a_out->index;
                av_packet_rescale_ts(apkt, a_ifmt->streams[audio_idx]->time_base, a_out->time_base);
                apkt->pos = -1;
                av_interleaved_write_frame(ofmt_ctx, apkt);
            }
            av_packet_unref(apkt);
            got_a = read_next_stream_packet(a_ifmt, audio_idx, apkt);
            if (got_a < 0) goto cleanup;
        }
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
    if (apkt) av_packet_free(&apkt);
    if (vpkt) av_packet_free(&vpkt);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            uint8_t *d;
            avio_close_dyn_buf(ofmt_ctx->pb, &d);
            av_free(d);
        }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&a_ifmt, &a_avio);
    close_input(&v_ifmt, &v_avio);
    return result;
}

PYMEDIA_API void pymedia_free(void *ptr) {
    free(ptr);
}

