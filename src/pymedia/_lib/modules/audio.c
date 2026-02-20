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
    } else if (strcmp(format, "flac") == 0) {
        *encoder_name = "flac"; *muxer_name = "flac";
        *sample_fmt = AV_SAMPLE_FMT_S16; *bitrate = 0;
    } else if (strcmp(format, "opus") == 0) {
        *encoder_name = "libopus"; *muxer_name = "opus";
        *sample_fmt = AV_SAMPLE_FMT_FLTP; *bitrate = 128000;
    } else {
        return -1;
    }
    return 0;
}

static enum AVSampleFormat pick_sample_fmt(const AVCodec *codec, enum AVSampleFormat preferred) {
    const enum AVSampleFormat *fmts = codec ? codec->sample_fmts : NULL;
    if (!fmts) return preferred;
    for (const enum AVSampleFormat *p = fmts; *p != AV_SAMPLE_FMT_NONE; ++p) {
        if (*p == preferred) return preferred;
    }
    return fmts[0];
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

PYMEDIA_API uint8_t* extract_audio(uint8_t *video_data, size_t video_size,
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
    if (!encoder && strcmp(format, "ogg") == 0) {
        // Some FFmpeg builds ship native Vorbis without the "libvorbis" alias.
        encoder = avcodec_find_encoder_by_name("vorbis");
    }
    if (!encoder && strcmp(format, "ogg") == 0) {
        encoder = avcodec_find_encoder(AV_CODEC_ID_VORBIS);
    }
    if (!encoder && strcmp(format, "opus") == 0) {
        encoder = avcodec_find_encoder_by_name("opus");
    }
    if (!encoder && strcmp(format, "opus") == 0) {
        encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }
    if (!encoder) goto cleanup;
    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) goto cleanup;
    enc_ctx->sample_rate = 44100;
    enc_ctx->sample_fmt = pick_sample_fmt(encoder, enc_sample_fmt);
    if (strcmp(format, "ogg") == 0) {
        // Native Vorbis is experimental in some FFmpeg builds.
        enc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }
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

PYMEDIA_API uint8_t* transcode_audio_advanced(uint8_t *video_data, size_t video_size,
                                              const char *format, int bitrate,
                                              int sample_rate, int channels,
                                              size_t *out_size) {
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
    int default_bitrate;
    if (get_audio_format_info(format, &encoder_name, &muxer_name,
                              &enc_sample_fmt, &default_bitrate) < 0) {
        fprintf(stderr, "Unsupported audio format: %s\n", format);
        return NULL;
    }

    int out_bitrate = (bitrate > 0) ? bitrate : default_bitrate;
    int out_sample_rate = (sample_rate > 0) ? sample_rate : 44100;
    int out_channels = (channels > 0) ? channels : 2;
    if (out_channels < 1 || out_channels > 8) return NULL;

    if (open_input_memory(video_data, video_size, &ifmt_ctx,
                          &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int audio_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO);
    if (audio_idx < 0) goto cleanup;

    AVCodecParameters *codecpar = ifmt_ctx->streams[audio_idx]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) goto cleanup;
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) goto cleanup;
    avcodec_parameters_to_context(dec_ctx, codecpar);
    if (avcodec_open2(dec_ctx, decoder, NULL) < 0) goto cleanup;

    const AVCodec *encoder = avcodec_find_encoder_by_name(encoder_name);
    if (!encoder && strcmp(format, "ogg") == 0) {
        encoder = avcodec_find_encoder_by_name("vorbis");
    }
    if (!encoder && strcmp(format, "ogg") == 0) {
        encoder = avcodec_find_encoder(AV_CODEC_ID_VORBIS);
    }
    if (!encoder && strcmp(format, "opus") == 0) {
        encoder = avcodec_find_encoder_by_name("opus");
    }
    if (!encoder && strcmp(format, "opus") == 0) {
        encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }
    if (!encoder) goto cleanup;

    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) goto cleanup;
    enc_ctx->sample_rate = out_sample_rate;
    enc_ctx->sample_fmt = pick_sample_fmt(encoder, enc_sample_fmt);
    if (strcmp(format, "ogg") == 0) {
        enc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }
#if FF_NEW_CHANNEL_LAYOUT
    av_channel_layout_default(&enc_ctx->ch_layout, out_channels);
#else
    enc_ctx->channel_layout = av_get_default_channel_layout(out_channels);
    enc_ctx->channels = out_channels;
#endif
    if (out_bitrate > 0) enc_ctx->bit_rate = out_bitrate;
    enc_ctx->time_base = (AVRational){1, out_sample_rate};

    const AVOutputFormat *ofmt = av_guess_format(muxer_name, NULL, NULL);
    if (ofmt && (ofmt->flags & AVFMT_GLOBALHEADER))
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc_ctx, encoder, NULL) < 0) goto cleanup;

    int frame_size = enc_ctx->frame_size > 0 ? enc_ctx->frame_size : 1024;

#if FF_NEW_CHANNEL_LAYOUT
    {
        AVChannelLayout out_layout;
        AVChannelLayout in_layout;
        av_channel_layout_default(&out_layout, out_channels);
        if (dec_ctx->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&in_layout, &dec_ctx->ch_layout);
        else
            av_channel_layout_default(&in_layout, 2);
        swr_alloc_set_opts2(&swr, &out_layout, enc_sample_fmt, out_sample_rate,
            &in_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate, 0, NULL);
        av_channel_layout_uninit(&out_layout);
        av_channel_layout_uninit(&in_layout);
    }
#else
    swr = swr_alloc_set_opts(NULL,
        av_get_default_channel_layout(out_channels), enc_sample_fmt, out_sample_rate,
        dec_ctx->channel_layout ? dec_ctx->channel_layout
            : av_get_default_channel_layout(dec_ctx->channels),
        dec_ctx->sample_fmt, dec_ctx->sample_rate, 0, NULL);
#endif
    if (!swr || swr_init(swr) < 0) goto cleanup;

    fifo = av_audio_fifo_alloc(enc_sample_fmt, out_channels, frame_size);
    if (!fifo) goto cleanup;

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
                    av_samples_alloc_array_and_samples(&resamp_buf, NULL, out_channels,
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

    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, dec_frame) == 0) {
        int out_samples = swr_get_out_samples(swr, dec_frame->nb_samples);
        if (out_samples <= 0) continue;
        if (out_samples > resamp_buf_size) {
            if (resamp_buf) av_freep(&resamp_buf[0]);
            av_freep(&resamp_buf);
            av_samples_alloc_array_and_samples(&resamp_buf, NULL, out_channels,
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

    for (;;) {
        int out_samples = 1024;
        if (out_samples > resamp_buf_size) {
            if (resamp_buf) av_freep(&resamp_buf[0]);
            av_freep(&resamp_buf);
            av_samples_alloc_array_and_samples(&resamp_buf, NULL, out_channels,
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

