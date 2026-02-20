// ============================================================
// subtitle track operations — add/remove/extract soft subtitles
// ============================================================

static void json_append(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 >= *cap) {
        while (*len + n + 1 >= *cap) *cap *= 2;
        char *tmp = realloc(*buf, *cap);
        if (!tmp) return;
        *buf = tmp;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void json_append_escaped(char **buf, size_t *len, size_t *cap, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        char tmp[8];
        if (*p == '\\' || *p == '"') {
            snprintf(tmp, sizeof(tmp), "\\%c", *p);
            json_append(buf, len, cap, tmp);
        } else if (*p == '\n') {
            json_append(buf, len, cap, "\\n");
        } else if (*p == '\r') {
            json_append(buf, len, cap, "\\r");
        } else if (*p == '\t') {
            json_append(buf, len, cap, "\\t");
        } else if (*p < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
            json_append(buf, len, cap, tmp);
        } else {
            char c[2] = {(char)*p, '\0'};
            json_append(buf, len, cap, c);
        }
    }
}

PYMEDIA_API char* extract_subtitles_json(uint8_t *video_data, size_t video_size) {
    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVPacket *pkt = NULL;

    size_t cap = 4096;
    size_t len = 0;
    char *json = malloc(cap);
    if (!json) return NULL;
    json[0] = '\0';
    json_append(&json, &len, &cap, "[");

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    int first_obj = 1;
    for (unsigned si = 0; si < ifmt_ctx->nb_streams; si++) {
        AVStream *st = ifmt_ctx->streams[si];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;

        const char *lang = av_dict_get(st->metadata, "language", NULL, 0)
            ? av_dict_get(st->metadata, "language", NULL, 0)->value
            : "und";
        const char *codec_name = avcodec_get_name(st->codecpar->codec_id);

        char *text_buf = malloc(8192);
        if (!text_buf) continue;
        text_buf[0] = '\0';
        size_t tlen = 0;

        av_seek_frame(ifmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
        while (av_read_frame(ifmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == (int)si && pkt->size > 0) {
                size_t copy_n = (size_t)pkt->size;
                if (copy_n > 512) copy_n = 512;
                if (tlen + copy_n + 2 < 8192) {
                    memcpy(text_buf + tlen, pkt->data, copy_n);
                    tlen += copy_n;
                    text_buf[tlen++] = '\n';
                    text_buf[tlen] = '\0';
                }
                if (tlen > 2048) {
                    av_packet_unref(pkt);
                    break;
                }
            }
            av_packet_unref(pkt);
        }

        if (!first_obj) json_append(&json, &len, &cap, ",");
        first_obj = 0;

        json_append(&json, &len, &cap, "{\"stream_index\":");
        char idxbuf[32];
        snprintf(idxbuf, sizeof(idxbuf), "%u", si);
        json_append(&json, &len, &cap, idxbuf);

        json_append(&json, &len, &cap, ",\"language\":\"");
        json_append_escaped(&json, &len, &cap, lang);
        json_append(&json, &len, &cap, "\",\"codec\":\"");
        json_append_escaped(&json, &len, &cap, codec_name ? codec_name : "unknown");
        json_append(&json, &len, &cap, "\",\"text\":\"");
        json_append_escaped(&json, &len, &cap, text_buf);
        json_append(&json, &len, &cap, "\"}");

        free(text_buf);
    }

    json_append(&json, &len, &cap, "]");

cleanup:
    if (pkt) av_packet_free(&pkt);
    close_input(&ifmt_ctx, &input_avio_ctx);
    return json;
}

PYMEDIA_API uint8_t* remove_subtitle_tracks(uint8_t *video_data, size_t video_size,
                                            const char *language, size_t *out_size) {
    (void)language;
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

    const char *iname = ifmt_ctx->iformat->name;
    const char *fmt = (strstr(iname, "matroska") || strstr(iname, "webm")) ? "matroska" : "mp4";

    avformat_alloc_output_context2(&ofmt_ctx, NULL, fmt, NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

    stream_mapping = calloc(ifmt_ctx->nb_streams, sizeof(int));
    if (!stream_mapping) goto cleanup;
    int out_idx = 0;

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVCodecParameters *par = ifmt_ctx->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }
        AVStream *out_s = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_s) goto cleanup;
        if (avcodec_parameters_copy(out_s->codecpar, par) < 0) goto cleanup;
        out_s->codecpar->codec_tag = 0;
        out_s->time_base = ifmt_ctx->streams[i]->time_base;
        stream_mapping[i] = out_idx++;
    }

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

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

PYMEDIA_API uint8_t* add_subtitle_track(uint8_t *video_data, size_t video_size,
                                        const char *srt_text, const char *lang,
                                        const char *codec, size_t *out_size) {
    *out_size = 0;
    if (!srt_text || !srt_text[0]) return NULL;
    if (!lang || !lang[0]) lang = "eng";

    int cue_count = 0;
    SubtitleCue *cues = parse_srt_cues(srt_text, &cue_count);
    if (!cues || cue_count <= 0) return NULL;

    BufferData bd;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVIOContext *input_avio_ctx = NULL;
    AVPacket *pkt = NULL;
    uint8_t *output_buffer = NULL;
    uint8_t *result = NULL;
    int *stream_mapping = NULL;

    enum AVCodecID sub_codec = AV_CODEC_ID_SUBRIP;
    const char *out_fmt = "matroska";
    if (codec && strcmp(codec, "mov_text") == 0) {
        sub_codec = AV_CODEC_ID_MOV_TEXT;
        out_fmt = "mp4";
    }

    if (open_input_memory(video_data, video_size, &ifmt_ctx, &input_avio_ctx, &bd) < 0)
        goto cleanup;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, out_fmt, NULL);
    if (!ofmt_ctx) goto cleanup;
    if (avio_open_dyn_buf(&ofmt_ctx->pb) < 0) goto cleanup;

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

    AVStream *sub_s = avformat_new_stream(ofmt_ctx, NULL);
    if (!sub_s) goto cleanup;
    sub_s->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    sub_s->codecpar->codec_id = sub_codec;
    sub_s->time_base = (AVRational){1, 1000};
    av_dict_set(&sub_s->metadata, "language", lang, 0);
    int sub_index = out_idx++;

    if (avformat_write_header(ofmt_ctx, NULL) < 0) goto cleanup;

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

    for (int i = 0; i < cue_count; i++) {
        AVPacket spkt;
        av_init_packet(&spkt);
        spkt.data = NULL;
        spkt.size = 0;

        const char *txt = cues[i].text ? cues[i].text : "";
        int txt_len = (int)strlen(txt);
        if (txt_len <= 0) continue;
        if (av_new_packet(&spkt, txt_len) < 0) continue;
        memcpy(spkt.data, txt, txt_len);

        int64_t start_ms = (int64_t)(cues[i].start_sec * 1000.0 + 0.5);
        int64_t end_ms = (int64_t)(cues[i].end_sec * 1000.0 + 0.5);
        if (end_ms < start_ms) end_ms = start_ms;
        spkt.pts = start_ms;
        spkt.dts = start_ms;
        spkt.duration = end_ms - start_ms;
        spkt.stream_index = sub_index;

        av_interleaved_write_frame(ofmt_ctx, &spkt);
        av_packet_unref(&spkt);
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
    if (cues) free_srt_cues(cues, cue_count);
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
