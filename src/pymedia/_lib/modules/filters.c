// ============================================================
// basic filter operations — blur/denoise/sharpen/color/lut-gamma
// ============================================================

static int clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void blur_plane(uint8_t *dst, const uint8_t *src, int w, int h, int linesize, int radius) {
    if (radius < 1) radius = 1;
    if (radius > 6) radius = 6;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sum = 0;
            int cnt = 0;
            for (int dy = -radius; dy <= radius; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= h) continue;
                for (int dx = -radius; dx <= radius; dx++) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= w) continue;
                    sum += src[yy * linesize + xx];
                    cnt++;
                }
            }
            dst[y * linesize + x] = (uint8_t)(sum / (cnt ? cnt : 1));
        }
    }
}

static void filter_frame_yuv420(AVFrame *frame, int mode, double p1, double p2, double p3) {
    int w = frame->width;
    int h = frame->height;

    if (mode == 1 || mode == 2 || mode == 3) {
        uint8_t *tmp = malloc((size_t)frame->linesize[0] * h);
        if (!tmp) return;
        int radius = (int)round(p1);
        if (mode == 2) radius = radius < 1 ? 1 : radius; // denoise uses small blur
        blur_plane(tmp, frame->data[0], w, h, frame->linesize[0], radius <= 0 ? 1 : radius);

        if (mode == 1) {
            for (int y = 0; y < h; y++) {
                memcpy(frame->data[0] + y * frame->linesize[0], tmp + y * frame->linesize[0], w);
            }
        } else if (mode == 2) {
            // Weighted denoise: keep detail but suppress noise.
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int idx = y * frame->linesize[0] + x;
                    int v = (int)frame->data[0][idx];
                    int b = (int)tmp[idx];
                    frame->data[0][idx] = (uint8_t)((3 * v + 2 * b) / 5);
                }
            }
        } else {
            // Unsharp mask on luma.
            double amount = p1;
            if (amount < 0.0) amount = 0.0;
            if (amount > 3.0) amount = 3.0;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int idx = y * frame->linesize[0] + x;
                    int orig = frame->data[0][idx];
                    int blur = tmp[idx];
                    int out = (int)round(orig + amount * (orig - blur));
                    frame->data[0][idx] = (uint8_t)clamp_u8(out);
                }
            }
        }
        free(tmp);
        return;
    }

    if (mode == 4) {
        // color_correct: brightness/contrast on Y, saturation on U/V.
        double brightness = p1;   // -1..1 mapped to -255..255
        double contrast = p2;     // around 1.0
        double saturation = p3;   // around 1.0
        if (contrast < 0.0) contrast = 0.0;
        if (saturation < 0.0) saturation = 0.0;

        int bdelta = (int)round(brightness * 255.0);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int idx = y * frame->linesize[0] + x;
                int yv = frame->data[0][idx];
                int out = (int)round((yv - 128) * contrast + 128 + bdelta);
                frame->data[0][idx] = (uint8_t)clamp_u8(out);
            }
        }

        int cw = w / 2;
        int ch = h / 2;
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                int ui = y * frame->linesize[1] + x;
                int vi = y * frame->linesize[2] + x;
                int u = frame->data[1][ui];
                int v = frame->data[2][vi];
                int uo = (int)round((u - 128) * saturation + 128);
                int vo = (int)round((v - 128) * saturation + 128);
                frame->data[1][ui] = (uint8_t)clamp_u8(uo);
                frame->data[2][vi] = (uint8_t)clamp_u8(vo);
            }
        }
        return;
    }

    if (mode == 5) {
        // lut gamma on luma.
        double gamma = p1;
        if (gamma < 0.1) gamma = 0.1;
        if (gamma > 5.0) gamma = 5.0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int idx = y * frame->linesize[0] + x;
                double n = frame->data[0][idx] / 255.0;
                int out = (int)round(pow(n, gamma) * 255.0);
                frame->data[0][idx] = (uint8_t)clamp_u8(out);
            }
        }
        return;
    }
}

PYMEDIA_API uint8_t* filter_video_basic(uint8_t *video_data, size_t video_size,
                                        int mode, double p1, double p2, double p3,
                                        int crf, const char *preset,
                                        size_t *out_size) {
    *out_size = 0;
    if (!preset || preset[0] == '\0') preset = "medium";
    if (crf < 0) crf = 23;
    if (crf > 51) crf = 51;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL;
    AVCodecContext *venc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL;
    AVFrame *dec_frame = NULL;
    AVFrame *filt_frame = NULL;
    uint8_t *output_buffer = NULL;
    uint8_t *result = NULL;
    int *stream_mapping = NULL;

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
    filt_frame = av_frame_alloc();
    if (!pkt || !dec_frame || !filt_frame) goto cleanup;

    filt_frame->format = AV_PIX_FMT_YUV420P;
    filt_frame->width = src_w;
    filt_frame->height = src_h;
    if (av_frame_get_buffer(filt_frame, 0) < 0) goto cleanup;

    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(filt_frame);
                    sws_scale(sws,
                        (const uint8_t *const *)dec_frame->data,
                        dec_frame->linesize, 0, src_h,
                        filt_frame->data, filt_frame->linesize);

                    filter_frame_yuv420(filt_frame, mode, p1, p2, p3);
                    filt_frame->pts = dec_frame->pts;

                    avcodec_send_frame(venc_ctx, filt_frame);
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
        av_frame_make_writable(filt_frame);
        sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                  dec_frame->linesize, 0, src_h,
                  filt_frame->data, filt_frame->linesize);
        filter_frame_yuv420(filt_frame, mode, p1, p2, p3);
        filt_frame->pts = dec_frame->pts;

        avcodec_send_frame(venc_ctx, filt_frame);
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
    if (filt_frame) av_frame_free(&filt_frame);
    if (dec_frame) av_frame_free(&dec_frame);
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
