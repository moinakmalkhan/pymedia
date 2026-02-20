// ============================================================
// 9. adjust_volume — change audio volume level
// factor > 1.0 amplifies, < 1.0 reduces, 0.0 = silence
// ============================================================

PYMEDIA_API uint8_t* adjust_volume(uint8_t *video_data, size_t video_size,
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

PYMEDIA_API uint8_t* merge_videos(uint8_t *data1, size_t size1,
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

PYMEDIA_API uint8_t* reverse_video(uint8_t *video_data, size_t video_size,
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
// 12. stabilize_video — lightweight temporal stabilization
// ============================================================

PYMEDIA_API uint8_t* stabilize_video(uint8_t *video_data, size_t video_size,
                                     int strength, size_t *out_size) {
    *out_size = 0;
    if (strength < 1) strength = 1;
    if (strength > 32) strength = 32;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL, *venc_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVPacket *pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *yuv_frame = NULL, *prev_frame = NULL;
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
    av_opt_set(venc_ctx->priv_data, "crf", "23", 0);
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
    if (!v_out) goto cleanup;
    avcodec_parameters_from_context(v_out->codecpar, venc_ctx);
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

    int audio_out_idx = -1;
    if (audio_idx >= 0) {
        AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
        if (!a_out) goto cleanup;
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

    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    yuv_frame = av_frame_alloc();
    prev_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame || !yuv_frame || !prev_frame) goto cleanup;

    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = src_w; yuv_frame->height = src_h;
    if (av_frame_get_buffer(yuv_frame, 0) < 0) goto cleanup;

    prev_frame->format = AV_PIX_FMT_YUV420P;
    prev_frame->width = src_w; prev_frame->height = src_h;
    if (av_frame_get_buffer(prev_frame, 0) < 0) goto cleanup;

    int have_prev = 0;
    int w_prev = strength;
    int w_curr = 32 - strength;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(yuv_frame);
                    sws_scale(sws, (const uint8_t *const *)dec_frame->data,
                              dec_frame->linesize, 0, src_h,
                              yuv_frame->data, yuv_frame->linesize);

                    if (have_prev) {
                        for (int p = 0; p < 3; p++) {
                            int plane_h = (p == 0) ? src_h : src_h / 2;
                            int plane_w = (p == 0) ? src_w : src_w / 2;
                            for (int y = 0; y < plane_h; y++) {
                                uint8_t *dst = yuv_frame->data[p] + y * yuv_frame->linesize[p];
                                uint8_t *prv = prev_frame->data[p] + y * prev_frame->linesize[p];
                                for (int x = 0; x < plane_w; x++) {
                                    dst[x] = (uint8_t)((dst[x] * w_curr + prv[x] * w_prev) / 32);
                                }
                            }
                        }
                    }

                    av_frame_make_writable(prev_frame);
                    for (int p = 0; p < 3; p++) {
                        int plane_h = (p == 0) ? src_h : src_h / 2;
                        int plane_w = (p == 0) ? src_w : src_w / 2;
                        for (int y = 0; y < plane_h; y++) {
                            memcpy(
                                prev_frame->data[p] + y * prev_frame->linesize[p],
                                yuv_frame->data[p] + y * yuv_frame->linesize[p],
                                plane_w
                            );
                        }
                    }
                    have_prev = 1;

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
                  dec_frame->linesize, 0, src_h,
                  yuv_frame->data, yuv_frame->linesize);
        if (have_prev) {
            for (int p = 0; p < 3; p++) {
                int plane_h = (p == 0) ? src_h : src_h / 2;
                int plane_w = (p == 0) ? src_w : src_w / 2;
                for (int y = 0; y < plane_h; y++) {
                    uint8_t *dst = yuv_frame->data[p] + y * yuv_frame->linesize[p];
                    uint8_t *prv = prev_frame->data[p] + y * prev_frame->linesize[p];
                    for (int x = 0; x < plane_w; x++) {
                        dst[x] = (uint8_t)((dst[x] * w_curr + prv[x] * w_prev) / 32);
                    }
                }
            }
        }
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
    if (prev_frame) av_frame_free(&prev_frame);
    if (yuv_frame) av_frame_free(&yuv_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (pkt) av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
    if (venc_ctx) avcodec_free_context(&venc_ctx);
    if (vdec_ctx) avcodec_free_context(&vdec_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) { uint8_t *d; avio_close_dyn_buf(ofmt_ctx->pb, &d); av_free(d); }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 12. subtitle_burn_in — render SRT subtitles into video frames
// ============================================================

PYMEDIA_API uint8_t* subtitle_burn_in(uint8_t *video_data, size_t video_size,
                                      const char *srt_text, int font_size,
                                      int margin_bottom, int crf, const char *preset,
                                      size_t *out_size) {
    *out_size = 0;
    if (!srt_text || !srt_text[0]) {
        return remux_video(video_data, video_size, NULL, -1, -1, 1, 1, out_size);
    }
    if (!preset || !preset[0]) preset = "medium";
    if (crf < 0) crf = 23;
    if (crf > 51) crf = 51;
    if (margin_bottom < 0) margin_bottom = 24;

    int cue_count = 0;
    SubtitleCue *cues = parse_srt_cues(srt_text, &cue_count);
    if (!cues || cue_count <= 0) {
        free_srt_cues(cues, cue_count);
        return remux_video(video_data, video_size, NULL, -1, -1, 1, 1, out_size);
    }

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVCodecContext *vdec_ctx = NULL, *venc_ctx = NULL;
    struct SwsContext *sws_to_rgba = NULL, *sws_to_yuv = NULL;
    AVPacket *pkt = NULL, *enc_pkt = NULL;
    AVFrame *dec_frame = NULL, *rgba_frame = NULL, *yuv_frame = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int *stream_mapping = NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);
    if (video_idx < 0) goto cleanup;

    AVCodecParameters *in_vpar = ifmt_ctx->streams[video_idx]->codecpar;
    int src_w = in_vpar->width;
    int src_h = in_vpar->height;

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
    avcodec_parameters_from_context(v_out->codecpar, venc_ctx);
    v_out->time_base = venc_ctx->time_base;
    stream_mapping[video_idx] = out_idx++;

    int audio_out_idx = -1;
    if (audio_idx >= 0) {
        AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
        if (!a_out) goto cleanup;
        avcodec_parameters_copy(a_out->codecpar, ifmt_ctx->streams[audio_idx]->codecpar);
        a_out->codecpar->codec_tag = 0;
        a_out->time_base = ifmt_ctx->streams[audio_idx]->time_base;
        audio_out_idx = out_idx++;
        stream_mapping[audio_idx] = audio_out_idx;
    }
    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        if ((int)i != video_idx && (int)i != audio_idx) stream_mapping[i] = -1;
    }
    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    sws_to_rgba = sws_getContext(src_w, src_h, vdec_ctx->pix_fmt,
                                 src_w, src_h, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, NULL, NULL, NULL);
    sws_to_yuv = sws_getContext(src_w, src_h, AV_PIX_FMT_RGBA,
                                src_w, src_h, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_to_rgba || !sws_to_yuv) goto cleanup;

    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    rgba_frame = av_frame_alloc();
    yuv_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame || !rgba_frame || !yuv_frame) goto cleanup;

    rgba_frame->format = AV_PIX_FMT_RGBA;
    rgba_frame->width = src_w;
    rgba_frame->height = src_h;
    if (av_frame_get_buffer(rgba_frame, 0) < 0) goto cleanup;

    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = src_w;
    yuv_frame->height = src_h;
    if (av_frame_get_buffer(yuv_frame, 0) < 0) goto cleanup;

    int cue_hint_idx = 0;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            if (avcodec_send_packet(vdec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(vdec_ctx, dec_frame) == 0) {
                    av_frame_make_writable(rgba_frame);
                    sws_scale(sws_to_rgba,
                              (const uint8_t *const *)dec_frame->data, dec_frame->linesize,
                              0, src_h, rgba_frame->data, rgba_frame->linesize);

                    int64_t ts = dec_frame->best_effort_timestamp;
                    if (ts == AV_NOPTS_VALUE) ts = dec_frame->pts;
                    double sec = (ts == AV_NOPTS_VALUE) ? 0.0
                        : ts * av_q2d(ifmt_ctx->streams[video_idx]->time_base);
                    const char *active = active_subtitle_text(cues, cue_count, sec, &cue_hint_idx);
                    if (active) draw_block_subtitle(rgba_frame, active, margin_bottom, font_size);

                    av_frame_make_writable(yuv_frame);
                    sws_scale(sws_to_yuv,
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
        sws_scale(sws_to_rgba,
                  (const uint8_t *const *)dec_frame->data, dec_frame->linesize,
                  0, src_h, rgba_frame->data, rgba_frame->linesize);
        int64_t ts = dec_frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = dec_frame->pts;
        double sec = (ts == AV_NOPTS_VALUE) ? 0.0
            : ts * av_q2d(ifmt_ctx->streams[video_idx]->time_base);
        const char *active = active_subtitle_text(cues, cue_count, sec, &cue_hint_idx);
        if (active) draw_block_subtitle(rgba_frame, active, margin_bottom, font_size);

        av_frame_make_writable(yuv_frame);
        sws_scale(sws_to_yuv,
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
    free_srt_cues(cues, cue_count);
    free(stream_mapping);
    if (yuv_frame) av_frame_free(&yuv_frame);
    if (rgba_frame) av_frame_free(&rgba_frame);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (pkt) av_packet_free(&pkt);
    if (sws_to_yuv) sws_freeContext(sws_to_yuv);
    if (sws_to_rgba) sws_freeContext(sws_to_rgba);
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
    close_input(&ifmt_ctx, &input_avio_ctx);
    return result;
}

// ============================================================
// 13. create_audio_image_video — build slideshow video from audio + images
// ============================================================

PYMEDIA_API uint8_t* create_audio_image_video(uint8_t *audio_data, size_t audio_size,
                                              uint8_t **image_data, size_t *image_sizes,
                                              int image_count, double seconds_per_image,
                                              const char *transition,
                                              int width, int height,
                                              size_t *out_size) {
    *out_size = 0;
    if (!audio_data || audio_size == 0 || !image_data || !image_sizes || image_count <= 0) return NULL;
    if (seconds_per_image <= 0.0) seconds_per_image = 2.0;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) return NULL;

    BufferData abd;
    AVFormatContext *a_ifmt = NULL, *ofmt_ctx = NULL;
    AVIOContext *a_avio = NULL;
    AVCodecContext *venc_ctx = NULL;
    AVPacket *enc_pkt = NULL, *apkt = NULL;
    AVFrame *work_frame = NULL;
    AVFrame **slides = NULL;
    uint8_t *output_buffer = NULL, *result = NULL;
    int audio_idx = -1;
    const int fps = 25;

    if (open_input_memory(audio_data, audio_size, &a_ifmt, &a_avio, &abd) < 0) goto cleanup;
    audio_idx = find_stream(a_ifmt, AVMEDIA_TYPE_AUDIO);
    if (audio_idx < 0) goto cleanup;

    slides = calloc((size_t)image_count, sizeof(AVFrame *));
    if (!slides) goto cleanup;
    for (int i = 0; i < image_count; i++) {
        if (decode_first_frame_to_yuv420(image_data[i], image_sizes[i], width, height, &slides[i]) < 0) goto cleanup;
    }

    double audio_duration_sec = 0.0;
    if (a_ifmt->duration > 0) {
        audio_duration_sec = (double)a_ifmt->duration / AV_TIME_BASE;
    } else {
        AVStream *as = a_ifmt->streams[audio_idx];
        if (as->duration > 0) audio_duration_sec = as->duration * av_q2d(as->time_base);
    }
    if (audio_duration_sec <= 0.0) {
        audio_duration_sec = image_count * seconds_per_image;
    }

    int64_t total_frames = (int64_t)(audio_duration_sec * fps + 0.5);
    if (total_frames < 1) total_frames = 1;
    int transition_frames = (int)(fps * 0.4);
    if (transition_frames < 1) transition_frames = 1;

    const AVCodec *vencoder = avcodec_find_encoder_by_name("libx264");
    if (!vencoder) goto cleanup;
    venc_ctx = avcodec_alloc_context3(vencoder);
    if (!venc_ctx) goto cleanup;
    venc_ctx->width = width;
    venc_ctx->height = height;
    venc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    venc_ctx->time_base = (AVRational){1, fps};
    venc_ctx->framerate = (AVRational){fps, 1};
    av_opt_set(venc_ctx->priv_data, "crf", "20", 0);
    av_opt_set(venc_ctx->priv_data, "preset", "medium", 0);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) venc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(venc_ctx, vencoder, NULL) < 0) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    AVStream *v_out = avformat_new_stream(ofmt_ctx, NULL);
    AVStream *a_out = avformat_new_stream(ofmt_ctx, NULL);
    if (!v_out || !a_out) goto cleanup;
    avcodec_parameters_from_context(v_out->codecpar, venc_ctx);
    v_out->time_base = venc_ctx->time_base;
    avcodec_parameters_copy(a_out->codecpar, a_ifmt->streams[audio_idx]->codecpar);
    a_out->codecpar->codec_tag = 0;
    a_out->time_base = a_ifmt->streams[audio_idx]->time_base;

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

    work_frame = av_frame_alloc();
    enc_pkt = av_packet_alloc();
    apkt = av_packet_alloc();
    if (!work_frame || !enc_pkt || !apkt) goto cleanup;
    work_frame->format = AV_PIX_FMT_YUV420P;
    work_frame->width = width;
    work_frame->height = height;
    if (av_frame_get_buffer(work_frame, 0) < 0) goto cleanup;

    for (int64_t fi = 0; fi < total_frames; fi++) {
        double t_sec = fi / (double)fps;
        int idx = (int)(t_sec / seconds_per_image);
        if (idx < 0) idx = 0;
        if (idx >= image_count) idx = image_count - 1;
        int next_idx = (idx + 1 < image_count) ? idx + 1 : idx;
        double local = t_sec - idx * seconds_per_image;

        av_frame_make_writable(work_frame);
        if (next_idx != idx && transition && !str_eq_nocase(transition, "none") &&
            local > (seconds_per_image - transition_frames / (double)fps)) {
            double t = (local - (seconds_per_image - transition_frames / (double)fps)) /
                       (transition_frames / (double)fps);
            if (str_eq_nocase(transition, "slide_left")) {
                slide_left_yuv420_frames(work_frame, slides[idx], slides[next_idx], width, height, t);
            } else {
                blend_yuv420_frames(work_frame, slides[idx], slides[next_idx], width, height, t);
            }
        } else {
            copy_yuv420_frame(work_frame, slides[idx], width, height);
        }
        work_frame->pts = fi;
        avcodec_send_frame(venc_ctx, work_frame);
        while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
            enc_pkt->stream_index = v_out->index;
            av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
            av_interleaved_write_frame(ofmt_ctx, enc_pkt);
            av_packet_unref(enc_pkt);
        }
    }

    avcodec_send_frame(venc_ctx, NULL);
    while (avcodec_receive_packet(venc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = v_out->index;
        av_packet_rescale_ts(enc_pkt, venc_ctx->time_base, v_out->time_base);
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }

    int64_t a_first_pts = AV_NOPTS_VALUE;
    int64_t a_first_dts = AV_NOPTS_VALUE;
    while (av_read_frame(a_ifmt, apkt) >= 0) {
        if (apkt->stream_index != audio_idx) {
            av_packet_unref(apkt);
            continue;
        }

        int64_t a_ref = (apkt->pts != AV_NOPTS_VALUE) ? apkt->pts : apkt->dts;
        if (a_ref != AV_NOPTS_VALUE) {
            double a_sec = a_ref * av_q2d(a_ifmt->streams[audio_idx]->time_base);
            if (a_sec > audio_duration_sec) {
                av_packet_unref(apkt);
                break;
            }
        }

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
        av_packet_unref(apkt);
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
    if (slides) {
        for (int i = 0; i < image_count; i++) {
            if (slides[i]) av_frame_free(&slides[i]);
        }
        free(slides);
    }
    if (work_frame) av_frame_free(&work_frame);
    if (apkt) av_packet_free(&apkt);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (venc_ctx) avcodec_free_context(&venc_ctx);
    if (ofmt_ctx) {
        if (ofmt_ctx->pb) {
            uint8_t *d;
            avio_close_dyn_buf(ofmt_ctx->pb, &d);
            av_free(d);
        }
        avformat_free_context(ofmt_ctx);
    }
    close_input(&a_ifmt, &a_avio);
    return result;
}

