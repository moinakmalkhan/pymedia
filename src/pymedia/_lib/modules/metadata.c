// ============================================================
// 14. strip_metadata — remove all metadata tags
// ============================================================

PYMEDIA_API uint8_t* strip_metadata(uint8_t *video_data, size_t video_size,
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

PYMEDIA_API uint8_t* set_metadata(uint8_t *video_data, size_t video_size,
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
