
/*
 * libdsre_audio.c
 *
 * DSRE native audio I/O wrapper for Android / Buildozer environments where
 * subprocess execution is unavailable.
 *
 * Python side:
 *   from ctypes import CDLL
 *   lib = CDLL("libdsre_audio.so")
 *
 * Exported C ABI:
 *   int dsre_decode_to_f32(...)
 *   int dsre_encode_from_f32(...)
 *   void dsre_free(void*)
 *   const char* dsre_last_error(void)
 *
 * PCM layout at this C ABI boundary:
 *   decode output: interleaved float32, samples x channels
 *   encode input : interleaved float32, samples x channels
 *
 * Cover art:
 *   dsre_encode_from_f32() now attempts to copy an attached picture stream
 *   from original_path into the output container. This is best-effort:
 *   if the target muxer rejects the attached picture, audio encoding continues.
 *
 * FFmpeg API assumption:
 *   Modern FFmpeg with AVChannelLayout / swr_alloc_set_opts2.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <strings.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#if defined(_WIN32)
#define DSRE_EXPORT __declspec(dllexport)
#else
#define DSRE_EXPORT __attribute__((visibility("default")))
#endif

#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 64
#endif

#define DSRE_OK 0
#define DSRE_ERR_ARG -1
#define DSRE_ERR_ALLOC -2
#define DSRE_ERR_FFMPEG -3
#define DSRE_ERR_STREAM -4
#define DSRE_ERR_CODEC -5
#define DSRE_ERR_RESAMPLE -6
#define DSRE_ERR_IO -7
#define DSRE_ERR_UNSUPPORTED -8

static char g_last_error[2048] = {0};

static void dsre_set_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
}

static void dsre_set_av_error(const char* prefix, int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, errbuf, sizeof(errbuf));
    dsre_set_error("%s: %s (%d)", prefix, errbuf, errnum);
}

DSRE_EXPORT const char* dsre_last_error(void) {
    return g_last_error;
}

DSRE_EXPORT void dsre_free(void* ptr) {
    if (ptr) {
        av_free(ptr);
    }
}

static int dsre_grow_float_buffer(float** buffer, int* capacity_floats, int required_floats) {
    int new_capacity;
    float* new_buf;

    if (required_floats <= *capacity_floats) {
        return DSRE_OK;
    }

    new_capacity = (*capacity_floats > 0) ? *capacity_floats : 262144;
    while (new_capacity < required_floats) {
        if (new_capacity > INT32_MAX / 2) {
            dsre_set_error("PCM buffer too large");
            return DSRE_ERR_ALLOC;
        }
        new_capacity *= 2;
    }

    new_buf = (float*)av_realloc_f(*buffer, (size_t)new_capacity, sizeof(float));
    if (!new_buf) {
        dsre_set_error("Failed to grow PCM buffer to %d floats", new_capacity);
        av_free(*buffer);
        *buffer = NULL;
        *capacity_floats = 0;
        return DSRE_ERR_ALLOC;
    }

    *buffer = new_buf;
    *capacity_floats = new_capacity;
    return DSRE_OK;
}

static int dsre_output_is_name_like(AVFormatContext* out_fmt, const char* token) {
    if (!out_fmt || !out_fmt->oformat || !out_fmt->oformat->name || !token) {
        return 0;
    }
    return strstr(out_fmt->oformat->name, token) != NULL;
}

static int dsre_cover_codec_allowed_for_output(AVFormatContext* out_fmt, enum AVCodecID codec_id) {
    int is_mp4_like = 0;
    int is_mp3_like = 0;
    int is_flac_like = 0;

    if (!out_fmt || !out_fmt->oformat) {
        return 0;
    }

    is_mp4_like = dsre_output_is_name_like(out_fmt, "mp4") ||
                  dsre_output_is_name_like(out_fmt, "ipod") ||
                  dsre_output_is_name_like(out_fmt, "mov") ||
                  dsre_output_is_name_like(out_fmt, "3gp");

    is_mp3_like = dsre_output_is_name_like(out_fmt, "mp3");
    is_flac_like = dsre_output_is_name_like(out_fmt, "flac");

    /*
     * Conservative policy:
     * - JPEG/MJPEG cover art is the safest first target.
     * - PNG/WebP may be accepted by some muxers/builds, but can also cause
     *   avformat_write_header(EINVAL) depending on the output container and
     *   FFmpeg build.
     *
     * If you later want PNG support, extend this condition after confirming
     * your FFmpeg build and target container accept it.
     */
    if (is_mp4_like || is_mp3_like || is_flac_like) {
        return codec_id == AV_CODEC_ID_MJPEG;
    }

    return codec_id == AV_CODEC_ID_MJPEG;
}

static int dsre_copy_metadata_and_cover_from_original(
    AVFormatContext* out_fmt,
    const char* original_path,
    AVPacket** out_cover_pkt,
    int* out_cover_stream_index
) {
    AVFormatContext* in_fmt = NULL;
    int ret;
    unsigned int i;

    if (out_cover_pkt) {
        *out_cover_pkt = NULL;
    }
    if (out_cover_stream_index) {
        *out_cover_stream_index = -1;
    }

    if (!out_fmt || !original_path || original_path[0] == '\0') {
        return DSRE_OK;
    }

    ret = avformat_open_input(&in_fmt, original_path, NULL, NULL);
    if (ret < 0) {
        return DSRE_OK;
    }

    ret = avformat_find_stream_info(in_fmt, NULL);
    if (ret >= 0 && in_fmt->metadata) {
        av_dict_copy(&out_fmt->metadata, in_fmt->metadata, 0);
    }

    if (!out_cover_pkt || !out_cover_stream_index) {
        avformat_close_input(&in_fmt);
        return DSRE_OK;
    }

    for (i = 0; i < in_fmt->nb_streams; ++i) {
        AVStream* in_stream = in_fmt->streams[i];
        AVStream* out_stream;

        if (!in_stream || !in_stream->codecpar) {
            continue;
        }

        if (!(in_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            continue;
        }

        if (in_stream->attached_pic.size <= 0 || !in_stream->attached_pic.data) {
            continue;
        }

        if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }

        if (in_stream->codecpar->width <= 0 || in_stream->codecpar->height <= 0) {
            continue;
        }

        if (!dsre_cover_codec_allowed_for_output(out_fmt, in_stream->codecpar->codec_id)) {
            /*
             * Unsupported cover format.
             * Skip it instead of making avformat_write_header fail.
             */
            continue;
        }

        out_stream = avformat_new_stream(out_fmt, NULL);
        if (!out_stream) {
            break;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            out_stream->codecpar->codec_type = AVMEDIA_TYPE_UNKNOWN;
            continue;
        }

        out_stream->codecpar->codec_tag = 0;
        out_stream->time_base = (AVRational){1, 90000};
        out_stream->disposition |= AV_DISPOSITION_ATTACHED_PIC;

        /*
         * Important:
         * Attached pictures should be stored in AVStream.attached_pic before
         * avformat_write_header(). Do not write them later as normal packets.
         */
        ret = av_packet_ref(&out_stream->attached_pic, &in_stream->attached_pic);
        if (ret < 0) {
            out_stream->codecpar->codec_type = AVMEDIA_TYPE_UNKNOWN;
            break;
        }

        out_stream->attached_pic.stream_index = out_stream->index;
        out_stream->attached_pic.pts = 0;
        out_stream->attached_pic.dts = 0;
        out_stream->attached_pic.duration = 0;
        out_stream->attached_pic.flags |= AV_PKT_FLAG_KEY;

        *out_cover_pkt = NULL;
        *out_cover_stream_index = out_stream->index;
        break;
    }

    avformat_close_input(&in_fmt);
    return DSRE_OK;
}

DSRE_EXPORT int dsre_decode_to_f32(
    const char* input_path,
    int target_sr,
    float** out_pcm,
    int* out_channels,
    int* out_samples
) {
    int ret = 0;
    int audio_stream_index = -1;
    int output_channels = 0;
    int total_samples = 0;
    int pcm_count = 0;
    int pcm_capacity = 0;
    int layout_ready = 0;

    AVFormatContext* fmt_ctx = NULL;
    AVCodecContext* dec_ctx = NULL;
    const AVCodec* decoder = NULL;
    AVPacket* packet = NULL;
    AVFrame* frame = NULL;
    SwrContext* swr = NULL;
    float* pcm = NULL;
    AVChannelLayout in_ch_layout = {0};
    AVChannelLayout out_ch_layout = {0};

    if (!input_path || !out_pcm || !out_channels || !out_samples || target_sr <= 0) {
        dsre_set_error("Invalid argument to dsre_decode_to_f32");
        return DSRE_ERR_ARG;
    }

    *out_pcm = NULL;
    *out_channels = 0;
    *out_samples = 0;
    g_last_error[0] = '\0';

    ret = avformat_open_input(&fmt_ctx, input_path, NULL, NULL);
    if (ret < 0) {
        dsre_set_av_error("avformat_open_input failed", ret);
        ret = DSRE_ERR_FFMPEG;
        goto cleanup;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        dsre_set_av_error("avformat_find_stream_info failed", ret);
        ret = DSRE_ERR_FFMPEG;
        goto cleanup;
    }

    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (ret < 0) {
        dsre_set_av_error("No audio stream found", ret);
        ret = DSRE_ERR_STREAM;
        goto cleanup;
    }
    audio_stream_index = ret;

    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        dsre_set_error("avcodec_alloc_context3 failed");
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    ret = avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[audio_stream_index]->codecpar);
    if (ret < 0) {
        dsre_set_av_error("avcodec_parameters_to_context failed", ret);
        ret = DSRE_ERR_FFMPEG;
        goto cleanup;
    }

    ret = avcodec_open2(dec_ctx, decoder, NULL);
    if (ret < 0) {
        dsre_set_av_error("avcodec_open2 failed", ret);
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    output_channels = dec_ctx->ch_layout.nb_channels;
    if (output_channels <= 0 && fmt_ctx->streams[audio_stream_index]->codecpar) {
        output_channels = fmt_ctx->streams[audio_stream_index]->codecpar->ch_layout.nb_channels;
    }
    if (output_channels <= 0) {
        output_channels = 2;
    }

    ret = av_channel_layout_copy(&in_ch_layout, &dec_ctx->ch_layout);
    if (ret < 0 || in_ch_layout.nb_channels <= 0) {
        if (ret >= 0) {
            av_channel_layout_uninit(&in_ch_layout);
        }
        if (fmt_ctx->streams[audio_stream_index]->codecpar &&
            fmt_ctx->streams[audio_stream_index]->codecpar->ch_layout.nb_channels > 0) {
            ret = av_channel_layout_copy(&in_ch_layout, &fmt_ctx->streams[audio_stream_index]->codecpar->ch_layout);
            if (ret < 0 || in_ch_layout.nb_channels <= 0) {
                if (ret >= 0) {
                    av_channel_layout_uninit(&in_ch_layout);
                }
                av_channel_layout_default(&in_ch_layout, output_channels);
            }
        } else {
            av_channel_layout_default(&in_ch_layout, output_channels);
        }
    }

    ret = av_channel_layout_copy(&out_ch_layout, &in_ch_layout);
    if (ret < 0 || out_ch_layout.nb_channels <= 0) {
        if (ret >= 0) {
            av_channel_layout_uninit(&out_ch_layout);
        }
        av_channel_layout_default(&out_ch_layout, output_channels);
    }
    output_channels = out_ch_layout.nb_channels;
    layout_ready = 1;

    ret = swr_alloc_set_opts2(
        &swr,
        &out_ch_layout,
        AV_SAMPLE_FMT_FLT,
        target_sr,
        &in_ch_layout,
        dec_ctx->sample_fmt,
        dec_ctx->sample_rate,
        0,
        NULL
    );
    if (ret < 0 || !swr) {
        dsre_set_av_error("swr_alloc_set_opts2 failed", ret);
        ret = DSRE_ERR_RESAMPLE;
        goto cleanup;
    }

    ret = swr_init(swr);
    if (ret < 0) {
        dsre_set_av_error("swr_init failed", ret);
        ret = DSRE_ERR_RESAMPLE;
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        dsre_set_error("Failed to allocate packet/frame");
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    while ((ret = av_read_frame(fmt_ctx, packet)) >= 0) {
        if (packet->stream_index != audio_stream_index) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(dec_ctx, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            dsre_set_av_error("avcodec_send_packet failed", ret);
            ret = DSRE_ERR_CODEC;
            goto cleanup;
        }

        while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
            int64_t delay = swr_get_delay(swr, dec_ctx->sample_rate);
            int out_count = (int)av_rescale_rnd(delay + frame->nb_samples, target_sr, dec_ctx->sample_rate, AV_ROUND_UP);
            int needed;
            int converted;
            uint8_t* out_planes[1];

            if (out_count <= 0) {
                av_frame_unref(frame);
                continue;
            }

            needed = pcm_count + out_count * output_channels;
            ret = dsre_grow_float_buffer(&pcm, &pcm_capacity, needed);
            if (ret != DSRE_OK) {
                goto cleanup;
            }

            out_planes[0] = (uint8_t*)(pcm + pcm_count);
            converted = swr_convert(
                swr,
                out_planes,
                out_count,
                (const uint8_t**)frame->extended_data,
                frame->nb_samples
            );
            if (converted < 0) {
                dsre_set_av_error("swr_convert failed", converted);
                ret = DSRE_ERR_RESAMPLE;
                goto cleanup;
            }

            pcm_count += converted * output_channels;
            total_samples += converted;
            av_frame_unref(frame);
        }

        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            dsre_set_av_error("avcodec_receive_frame failed", ret);
            ret = DSRE_ERR_CODEC;
            goto cleanup;
        }
    }

    if (ret != AVERROR_EOF) {
        dsre_set_av_error("av_read_frame failed", ret);
        ret = DSRE_ERR_FFMPEG;
        goto cleanup;
    }

    ret = avcodec_send_packet(dec_ctx, NULL);
    if (ret >= 0) {
        while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
            int64_t delay = swr_get_delay(swr, dec_ctx->sample_rate);
            int out_count = (int)av_rescale_rnd(delay + frame->nb_samples, target_sr, dec_ctx->sample_rate, AV_ROUND_UP);
            if (out_count > 0) {
                int needed = pcm_count + out_count * output_channels;
                int converted;
                uint8_t* out_planes[1];
                ret = dsre_grow_float_buffer(&pcm, &pcm_capacity, needed);
                if (ret != DSRE_OK) {
                    goto cleanup;
                }
                out_planes[0] = (uint8_t*)(pcm + pcm_count);
                converted = swr_convert(
                    swr,
                    out_planes,
                    out_count,
                    (const uint8_t**)frame->extended_data,
                    frame->nb_samples
                );
                if (converted < 0) {
                    dsre_set_av_error("swr_convert flush-frame failed", converted);
                    ret = DSRE_ERR_RESAMPLE;
                    goto cleanup;
                }
                pcm_count += converted * output_channels;
                total_samples += converted;
            }
            av_frame_unref(frame);
        }
    }

    while (1) {
        int out_count = 4096;
        int needed = pcm_count + out_count * output_channels;
        int converted;
        uint8_t* out_planes[1];
        ret = dsre_grow_float_buffer(&pcm, &pcm_capacity, needed);
        if (ret != DSRE_OK) {
            goto cleanup;
        }
        out_planes[0] = (uint8_t*)(pcm + pcm_count);
        converted = swr_convert(swr, out_planes, out_count, NULL, 0);
        if (converted < 0) {
            dsre_set_av_error("swr_convert final flush failed", converted);
            ret = DSRE_ERR_RESAMPLE;
            goto cleanup;
        }
        if (converted == 0) {
            break;
        }
        pcm_count += converted * output_channels;
        total_samples += converted;
    }

    if (!pcm || total_samples <= 0 || pcm_count <= 0) {
        dsre_set_error("Decoded audio is empty");
        ret = DSRE_ERR_FFMPEG;
        goto cleanup;
    }

    *out_pcm = pcm;
    *out_channels = output_channels;
    *out_samples = total_samples;
    pcm = NULL;
    ret = DSRE_OK;

cleanup:
    if (pcm) av_free(pcm);
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (swr) swr_free(&swr);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    if (layout_ready) {
        av_channel_layout_uninit(&in_ch_layout);
        av_channel_layout_uninit(&out_ch_layout);
    }
    if (ret != DSRE_OK) {
        *out_pcm = NULL;
        *out_channels = 0;
        *out_samples = 0;
    }
    return ret;
}

static enum AVCodecID dsre_codec_id_from_format(const char* format) {
    if (!format) return AV_CODEC_ID_ALAC;
    if (strcasecmp(format, "ALAC") == 0 || strcasecmp(format, "M4A") == 0) return AV_CODEC_ID_ALAC;
    if (strcasecmp(format, "FLAC") == 0) return AV_CODEC_ID_FLAC;
    if (strcasecmp(format, "MP3") == 0) return AV_CODEC_ID_MP3;
    return AV_CODEC_ID_NONE;
}

static const char* dsre_encoder_name_from_format(const char* format) {
    if (!format) return "alac";
    if (strcasecmp(format, "ALAC") == 0 || strcasecmp(format, "M4A") == 0) return "alac";
    if (strcasecmp(format, "FLAC") == 0) return "flac";
    if (strcasecmp(format, "MP3") == 0) return "libmp3lame";
    return NULL;
}

static int dsre_send_frame_and_write(AVFormatContext* fmt_ctx, AVCodecContext* enc_ctx, AVStream* stream, AVFrame* frame) {
    int ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0) {
        dsre_set_av_error("avcodec_send_frame failed", ret);
        return DSRE_ERR_CODEC;
    }

    while (1) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            dsre_set_error("av_packet_alloc failed during encode");
            return DSRE_ERR_ALLOC;
        }

        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            return DSRE_OK;
        }
        if (ret < 0) {
            av_packet_free(&pkt);
            dsre_set_av_error("avcodec_receive_packet failed", ret);
            return DSRE_ERR_CODEC;
        }

        av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        ret = av_interleaved_write_frame(fmt_ctx, pkt);
        av_packet_free(&pkt);
        if (ret < 0) {
            dsre_set_av_error("av_interleaved_write_frame failed", ret);
            return DSRE_ERR_IO;
        }
    }
}

DSRE_EXPORT int dsre_encode_from_f32(
    const char* original_path,
    const float* pcm_interleaved,
    int channels,
    int samples,
    int sr,
    const char* output_path,
    const char* format
) {
    int ret = 0;
    int64_t pts = 0;
    int frame_size = 0;
    int header_written = 0;
    int in_layout_ready = 0;

    AVFormatContext* out_fmt = NULL;
    AVCodecContext* enc_ctx = NULL;
    AVStream* stream = NULL;
    const AVCodec* encoder = NULL;
    SwrContext* swr = NULL;
    AVFrame* in_frame = NULL;
    AVFrame* enc_frame = NULL;
    AVPacket* cover_pkt = NULL;
    int cover_stream_index = -1;
    AVChannelLayout in_ch_layout = {0};

    if (!pcm_interleaved || channels <= 0 || samples <= 0 || sr <= 0 || !output_path) {
        dsre_set_error("Invalid argument to dsre_encode_from_f32");
        return DSRE_ERR_ARG;
    }

    g_last_error[0] = '\0';

    enum AVCodecID codec_id = dsre_codec_id_from_format(format);
    const char* encoder_name = dsre_encoder_name_from_format(format);
    if (codec_id == AV_CODEC_ID_NONE || !encoder_name) {
        dsre_set_error("Unsupported output format: %s", format ? format : "(null)");
        return DSRE_ERR_UNSUPPORTED;
    }

    encoder = avcodec_find_encoder_by_name(encoder_name);
    if (!encoder && codec_id == AV_CODEC_ID_MP3) {
        encoder = avcodec_find_encoder(codec_id);
    }
    if (!encoder) {
        dsre_set_error("Encoder not found for format: %s", format ? format : "(null)");
        return DSRE_ERR_CODEC;
    }

    ret = avformat_alloc_output_context2(&out_fmt, NULL, NULL, output_path);
    if (ret < 0 || !out_fmt) {
        dsre_set_av_error("avformat_alloc_output_context2 failed", ret);
        ret = DSRE_ERR_FFMPEG;
        goto cleanup;
    }

    stream = avformat_new_stream(out_fmt, NULL);
    if (!stream) {
        dsre_set_error("avformat_new_stream failed");
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        dsre_set_error("avcodec_alloc_context3 failed for encoder");
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    enc_ctx->codec_id = codec_id;
    enc_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
    enc_ctx->sample_rate = sr;
    enc_ctx->bit_rate = (codec_id == AV_CODEC_ID_MP3) ? 320000 : 0;
    enc_ctx->time_base = (AVRational){1, sr};
    av_channel_layout_default(&enc_ctx->ch_layout, channels);

    if (encoder->sample_fmts) {
        const enum AVSampleFormat* p;
        enc_ctx->sample_fmt = encoder->sample_fmts[0];
        for (p = encoder->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p) {
            if (*p == AV_SAMPLE_FMT_FLTP || *p == AV_SAMPLE_FMT_FLT ||
                *p == AV_SAMPLE_FMT_S32P || *p == AV_SAMPLE_FMT_S32 ||
                *p == AV_SAMPLE_FMT_S16P || *p == AV_SAMPLE_FMT_S16) {
                enc_ctx->sample_fmt = *p;
                break;
            }
        }
    } else {
        enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }

    if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    {
        AVDictionary* codec_opts = NULL;
        if (codec_id == AV_CODEC_ID_MP3) {
            av_dict_set(&codec_opts, "b:a", "320k", 0);
        }
        ret = avcodec_open2(enc_ctx, encoder, &codec_opts);
        av_dict_free(&codec_opts);
    }
    if (ret < 0) {
        dsre_set_av_error("avcodec_open2 encoder failed", ret);
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    ret = avcodec_parameters_from_context(stream->codecpar, enc_ctx);
    if (ret < 0) {
        dsre_set_av_error("avcodec_parameters_from_context failed", ret);
        ret = DSRE_ERR_FFMPEG;
        goto cleanup;
    }
    stream->time_base = enc_ctx->time_base;

    dsre_copy_metadata_and_cover_from_original(out_fmt, original_path, &cover_pkt, &cover_stream_index);

    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_fmt->pb, output_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            dsre_set_av_error("avio_open failed", ret);
            ret = DSRE_ERR_IO;
            goto cleanup;
        }
    }

    ret = avformat_write_header(out_fmt, NULL);
    if (ret < 0) {
        dsre_set_av_error("avformat_write_header failed", ret);
        ret = DSRE_ERR_IO;
        goto cleanup;
    }
    header_written = 1;

    /*
     * Cover art is already stored in out_stream->attached_pic before header.
     * Do not write it here as a normal packet.
     */
    (void)cover_stream_index;

    av_channel_layout_default(&in_ch_layout, channels);
    in_layout_ready = 1;

    ret = swr_alloc_set_opts2(
        &swr,
        &enc_ctx->ch_layout,
        enc_ctx->sample_fmt,
        sr,
        &in_ch_layout,
        AV_SAMPLE_FMT_FLT,
        sr,
        0,
        NULL
    );
    if (ret < 0 || !swr) {
        dsre_set_av_error("swr_alloc_set_opts2 encode failed", ret);
        ret = DSRE_ERR_RESAMPLE;
        goto cleanup;
    }

    ret = swr_init(swr);
    if (ret < 0) {
        dsre_set_av_error("swr_init encode failed", ret);
        ret = DSRE_ERR_RESAMPLE;
        goto cleanup;
    }

    frame_size = enc_ctx->frame_size;
    if (frame_size <= 0) {
        frame_size = 1024;
    }

    in_frame = av_frame_alloc();
    enc_frame = av_frame_alloc();
    if (!in_frame || !enc_frame) {
        dsre_set_error("av_frame_alloc failed for encode");
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    for (int offset = 0; offset < samples; ) {
        int nb = samples - offset;
        if (nb > frame_size) nb = frame_size;

        av_frame_unref(in_frame);
        in_frame->format = AV_SAMPLE_FMT_FLT;
        in_frame->sample_rate = sr;
        in_frame->nb_samples = nb;
        av_channel_layout_default(&in_frame->ch_layout, channels);

        ret = av_frame_get_buffer(in_frame, 0);
        if (ret < 0) {
            dsre_set_av_error("av_frame_get_buffer input failed", ret);
            ret = DSRE_ERR_ALLOC;
            goto cleanup;
        }

        memcpy(in_frame->data[0], pcm_interleaved + ((int64_t)offset * channels), (size_t)nb * channels * sizeof(float));

        av_frame_unref(enc_frame);
        enc_frame->format = enc_ctx->sample_fmt;
        enc_frame->sample_rate = sr;
        enc_frame->nb_samples = nb;
        ret = av_channel_layout_copy(&enc_frame->ch_layout, &enc_ctx->ch_layout);
        if (ret < 0) {
            dsre_set_av_error("av_channel_layout_copy failed", ret);
            ret = DSRE_ERR_FFMPEG;
            goto cleanup;
        }
        ret = av_frame_get_buffer(enc_frame, 0);
        if (ret < 0) {
            dsre_set_av_error("av_frame_get_buffer encoder failed", ret);
            ret = DSRE_ERR_ALLOC;
            goto cleanup;
        }

        ret = swr_convert(swr, enc_frame->extended_data, nb, (const uint8_t**)in_frame->extended_data, nb);
        if (ret < 0) {
            dsre_set_av_error("swr_convert encode failed", ret);
            ret = DSRE_ERR_RESAMPLE;
            goto cleanup;
        }

        enc_frame->nb_samples = ret;
        enc_frame->pts = pts;
        pts += ret;

        ret = dsre_send_frame_and_write(out_fmt, enc_ctx, stream, enc_frame);
        if (ret != DSRE_OK) goto cleanup;

        offset += nb;
    }

    while (1) {
        av_frame_unref(enc_frame);
        enc_frame->format = enc_ctx->sample_fmt;
        enc_frame->sample_rate = sr;
        enc_frame->nb_samples = frame_size;
        ret = av_channel_layout_copy(&enc_frame->ch_layout, &enc_ctx->ch_layout);
        if (ret < 0) {
            dsre_set_av_error("av_channel_layout_copy flush failed", ret);
            ret = DSRE_ERR_FFMPEG;
            goto cleanup;
        }
        ret = av_frame_get_buffer(enc_frame, 0);
        if (ret < 0) {
            dsre_set_av_error("av_frame_get_buffer flush failed", ret);
            ret = DSRE_ERR_ALLOC;
            goto cleanup;
        }
        ret = swr_convert(swr, enc_frame->extended_data, frame_size, NULL, 0);
        if (ret < 0) {
            dsre_set_av_error("swr_convert final encode flush failed", ret);
            ret = DSRE_ERR_RESAMPLE;
            goto cleanup;
        }
        if (ret == 0) break;
        enc_frame->nb_samples = ret;
        enc_frame->pts = pts;
        pts += ret;
        ret = dsre_send_frame_and_write(out_fmt, enc_ctx, stream, enc_frame);
        if (ret != DSRE_OK) goto cleanup;
    }

    ret = dsre_send_frame_and_write(out_fmt, enc_ctx, stream, NULL);
    if (ret != DSRE_OK) goto cleanup;

    ret = av_write_trailer(out_fmt);
    if (ret < 0) {
        dsre_set_av_error("av_write_trailer failed", ret);
        ret = DSRE_ERR_IO;
        goto cleanup;
    }

    ret = DSRE_OK;

cleanup:
    if (cover_pkt) av_packet_free(&cover_pkt);
    if (in_frame) av_frame_free(&in_frame);
    if (enc_frame) av_frame_free(&enc_frame);
    if (swr) swr_free(&swr);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (in_layout_ready) av_channel_layout_uninit(&in_ch_layout);
    if (out_fmt) {
        if (!(out_fmt->oformat->flags & AVFMT_NOFILE) && out_fmt->pb) {
            avio_closep(&out_fmt->pb);
        }
        avformat_free_context(out_fmt);
    }
    (void)header_written;
    return ret;
}
