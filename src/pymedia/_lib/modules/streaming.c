// ============================================================
// streaming/probe helpers — fragmented mp4 and packet timeline probes
// ============================================================

PYMEDIA_API uint8_t* create_fragmented_mp4(uint8_t *video_data, size_t video_size,
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

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    AVDictionary *mux_opts = NULL;
    av_dict_set(&mux_opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);

    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_s = ifmt_ctx->streams[i];
        AVStream *out_s = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_s) goto cleanup;
        if (avcodec_parameters_copy(out_s->codecpar, in_s->codecpar) < 0) goto cleanup;
        out_s->codecpar->codec_tag = 0;
        out_s->time_base = in_s->time_base;
        stream_mapping[i] = out_idx++;
    }

    if (avformat_write_header(ofmt_ctx, &mux_opts) < 0) goto cleanup;
    av_dict_free(&mux_opts);

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        int si = pkt->stream_index;
        if (si < 0 || (unsigned)si >= ifmt_ctx->nb_streams || stream_mapping[si] < 0) {
            av_packet_unref(pkt);
            continue;
        }
        AVStream *in_s = ifmt_ctx->streams[si];
        AVStream *out_s = ofmt_ctx->streams[stream_mapping[si]];
        pkt->stream_index = stream_mapping[si];
        av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
        pkt->pos = -1;
        av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(ofmt_ctx);
    {
        int out_n = avio_close_dyn_buf(ofmt_ctx->pb, &output_buffer);
        ofmt_ctx->pb = NULL;
        if (out_n > 0) {
            result = malloc(out_n);
            if (result) {
                memcpy(result, output_buffer, out_n);
                *out_size = out_n;
            }
        }
        av_free(output_buffer);
    }

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

PYMEDIA_API char* list_video_packet_timestamps_json(uint8_t *video_data, size_t video_size) {
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVPacket *pkt = NULL;

    size_t cap = 4096;
    size_t len = 0;
    char *json = malloc(cap);
    if (!json) return NULL;
    json[len++] = '[';
    json[len] = '\0';
    int first = 1;

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    int video_idx = find_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (video_idx < 0) goto cleanup;
    AVStream *vs = ifmt_ctx->streams[video_idx];

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            if (ts != AV_NOPTS_VALUE) {
                double t = ts * av_q2d(vs->time_base);
                char item[64];
                int n = snprintf(item, sizeof(item), first ? "%.6f" : ",%.6f", t);
                if (len + (size_t)n + 2 >= cap) {
                    while (len + (size_t)n + 2 >= cap) cap *= 2;
                    char *tmp = realloc(json, cap);
                    if (!tmp) goto cleanup;
                    json = tmp;
                }
                memcpy(json + len, item, (size_t)n);
                len += (size_t)n;
                json[len] = '\0';
                first = 0;
            }
        }
        av_packet_unref(pkt);
    }

cleanup:
    if (len + 2 >= cap) {
        cap += 2;
        char *tmp = realloc(json, cap);
        if (tmp) json = tmp;
    }
    json[len++] = ']';
    json[len] = '\0';

    if (pkt) av_packet_free(&pkt);
    close_input(&ifmt_ctx, &input_avio_ctx);
    return json;
}
