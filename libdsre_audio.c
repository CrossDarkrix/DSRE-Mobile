
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
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

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

static int dsre_attach_mjpeg_packet_to_output(
    AVFormatContext* out_fmt,
    AVPacket* jpg_pkt,
    int width,
    int height
) {
    AVStream* out_stream;
    int ret;

    if (!out_fmt || !jpg_pkt || !jpg_pkt->data || jpg_pkt->size <= 0 || width <= 0 || height <= 0) {
        return DSRE_ERR_ARG;
    }

    out_stream = avformat_new_stream(out_fmt, NULL);
    if (!out_stream) {
        return DSRE_ERR_ALLOC;
    }

    out_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    out_stream->codecpar->codec_id = AV_CODEC_ID_MJPEG;
    out_stream->codecpar->format = AV_PIX_FMT_YUVJ420P;
    out_stream->codecpar->width = width;
    out_stream->codecpar->height = height;
    out_stream->codecpar->codec_tag = 0;
    out_stream->time_base = (AVRational){1, 90000};
    out_stream->disposition |= AV_DISPOSITION_ATTACHED_PIC;

    ret = av_packet_ref(&out_stream->attached_pic, jpg_pkt);
    if (ret < 0) {
        out_stream->codecpar->codec_type = AVMEDIA_TYPE_UNKNOWN;
        return ret;
    }

    out_stream->attached_pic.stream_index = out_stream->index;
    out_stream->attached_pic.pts = 0;
    out_stream->attached_pic.dts = 0;
    out_stream->attached_pic.duration = 0;
    out_stream->attached_pic.flags |= AV_PKT_FLAG_KEY;
    return DSRE_OK;
}

static int dsre_attach_cover_direct_if_mjpeg(
    AVFormatContext* out_fmt,
    AVStream* in_stream
) {
    if (!in_stream || !in_stream->codecpar) {
        return DSRE_ERR_ARG;
    }
    if (in_stream->codecpar->codec_id != AV_CODEC_ID_MJPEG) {
        return DSRE_ERR_UNSUPPORTED;
    }
    return dsre_attach_mjpeg_packet_to_output(
        out_fmt,
        &in_stream->attached_pic,
        in_stream->codecpar->width,
        in_stream->codecpar->height
    );
}

static int dsre_transcode_cover_to_mjpeg_and_attach(
    AVFormatContext* out_fmt,
    AVStream* in_stream
) {
    int ret = DSRE_OK;
    int dst_w = 0;
    int dst_h = 0;
    const int max_side = 1500;

    const AVCodec* decoder = NULL;
    AVCodecContext* dec_ctx = NULL;
    AVPacket* in_pkt = NULL;
    AVFrame* decoded = NULL;

    struct SwsContext* sws = NULL;
    AVFrame* yuv = NULL;
    enum AVPixelFormat dst_pix_fmt = AV_PIX_FMT_YUVJ420P;

    const AVCodec* encoder = NULL;
    AVCodecContext* enc_ctx = NULL;
    AVPacket* jpg_pkt = NULL;

    if (!out_fmt || !in_stream || !in_stream->codecpar ||
        in_stream->attached_pic.size <= 0 || !in_stream->attached_pic.data) {
        return DSRE_ERR_ARG;
    }

    decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!decoder) {
        return DSRE_ERR_UNSUPPORTED;
    }

    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    ret = avcodec_parameters_to_context(dec_ctx, in_stream->codecpar);
    if (ret < 0) {
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    ret = avcodec_open2(dec_ctx, decoder, NULL);
    if (ret < 0) {
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    in_pkt = av_packet_alloc();
    decoded = av_frame_alloc();
    if (!in_pkt || !decoded) {
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    ret = av_packet_ref(in_pkt, &in_stream->attached_pic);
    if (ret < 0) {
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    ret = avcodec_send_packet(dec_ctx, in_pkt);
    if (ret < 0) {
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    ret = avcodec_receive_frame(dec_ctx, decoded);
    if (ret < 0) {
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    if (decoded->width <= 0 || decoded->height <= 0 || decoded->format == AV_PIX_FMT_NONE) {
        ret = DSRE_ERR_UNSUPPORTED;
        goto cleanup;
    }

    dst_w = decoded->width;
    dst_h = decoded->height;
    if (dst_w > max_side || dst_h > max_side) {
        if (dst_w >= dst_h) {
            dst_h = (int)((int64_t)dst_h * max_side / dst_w);
            dst_w = max_side;
        } else {
            dst_w = (int)((int64_t)dst_w * max_side / dst_h);
            dst_h = max_side;
        }
        if (dst_w <= 0) dst_w = 1;
        if (dst_h <= 0) dst_h = 1;
    }

    yuv = av_frame_alloc();
    if (!yuv) {
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }
    yuv->format = dst_pix_fmt;
    yuv->width = dst_w;
    yuv->height = dst_h;

    ret = av_frame_get_buffer(yuv, 32);
    if (ret < 0) {
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    sws = sws_getContext(
        decoded->width,
        decoded->height,
        (enum AVPixelFormat)decoded->format,
        dst_w,
        dst_h,
        dst_pix_fmt,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );
    if (!sws) {
        ret = DSRE_ERR_RESAMPLE;
        goto cleanup;
    }

    ret = sws_scale(
        sws,
        (const uint8_t* const*)decoded->data,
        decoded->linesize,
        0,
        decoded->height,
        yuv->data,
        yuv->linesize
    );
    if (ret <= 0) {
        ret = DSRE_ERR_RESAMPLE;
        goto cleanup;
    }

    encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!encoder) {
        ret = DSRE_ERR_UNSUPPORTED;
        goto cleanup;
    }

    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }
    enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    enc_ctx->codec_id = AV_CODEC_ID_MJPEG;
    enc_ctx->width = dst_w;
    enc_ctx->height = dst_h;
    enc_ctx->pix_fmt = dst_pix_fmt;
    enc_ctx->time_base = (AVRational){1, 90000};

    ret = avcodec_open2(enc_ctx, encoder, NULL);
    if (ret < 0) {
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    jpg_pkt = av_packet_alloc();
    if (!jpg_pkt) {
        ret = DSRE_ERR_ALLOC;
        goto cleanup;
    }

    yuv->pts = 0;
    ret = avcodec_send_frame(enc_ctx, yuv);
    if (ret < 0) {
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    ret = avcodec_receive_packet(enc_ctx, jpg_pkt);
    if (ret < 0) {
        ret = DSRE_ERR_CODEC;
        goto cleanup;
    }

    ret = dsre_attach_mjpeg_packet_to_output(out_fmt, jpg_pkt, dst_w, dst_h);
    if (ret != DSRE_OK) {
        goto cleanup;
    }

    ret = DSRE_OK;

cleanup:
    if (jpg_pkt) av_packet_free(&jpg_pkt);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (yuv) av_frame_free(&yuv);
    if (sws) sws_freeContext(sws);
    if (decoded) av_frame_free(&decoded);
    if (in_pkt) av_packet_free(&in_pkt);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    return ret;
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

    if (out_cover_pkt) *out_cover_pkt = NULL;
    if (out_cover_stream_index) *out_cover_stream_index = -1;

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

    for (i = 0; i < in_fmt->nb_streams; ++i) {
        AVStream* in_stream = in_fmt->streams[i];

        if (!in_stream || !in_stream->codecpar) continue;
        /* Some files expose cover data without the attached_pic disposition. */
        if (in_stream->attached_pic.size <= 0 || !in_stream->attached_pic.data) continue;
        if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;

        if (in_stream->codecpar->codec_id == AV_CODEC_ID_MJPEG) {
            ret = dsre_attach_cover_direct_if_mjpeg(out_fmt, in_stream);
        } else {
            ret = dsre_transcode_cover_to_mjpeg_and_attach(out_fmt, in_stream);
        }

        /* Cover art is best-effort. Never fail the whole audio encode because of it. */
        if (ret == DSRE_OK) {
            int cover_index = (int)out_fmt->nb_streams - 1;
            if (out_cover_stream_index) *out_cover_stream_index = cover_index;
            if (out_cover_pkt && cover_index >= 0 && out_fmt->streams[cover_index]) {
                AVPacket* cloned = av_packet_clone(&out_fmt->streams[cover_index]->attached_pic);
                if (cloned) {
                    cloned->stream_index = cover_index;
                    cloned->pts = 0;
                    cloned->dts = 0;
                    cloned->duration = 0;
                    cloned->flags |= AV_PKT_FLAG_KEY;
                    *out_cover_pkt = cloned;
                }
            }
            break;
        }
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

static const char* dsre_muxer_name_from_format(const char* format) {
    if (!format) return "ipod";
    if (strcasecmp(format, "ALAC") == 0 || strcasecmp(format, "M4A") == 0) return "ipod";
    if (strcasecmp(format, "FLAC") == 0) return "flac";
    if (strcasecmp(format, "MP3") == 0) return "mp3";
    return NULL;
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
    const char* muxer_name = dsre_muxer_name_from_format(format);
    if (codec_id == AV_CODEC_ID_NONE || !encoder_name || !muxer_name) {
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

    ret = avformat_alloc_output_context2(&out_fmt, NULL, muxer_name, output_path);
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

/* ========================================================================== */
/* Streaming chunk API                                                        */
/* ========================================================================== */

typedef struct DSREDecoder {
    AVFormatContext* fmt_ctx;
    AVCodecContext* dec_ctx;
    const AVCodec* decoder;
    AVPacket* pkt;
    AVFrame* frame;
    SwrContext* swr;
    AVChannelLayout in_ch_layout;
    AVChannelLayout out_ch_layout;
    int layout_ready;
    int audio_stream_index;
    int target_sr;
    int channels;
    int input_sr;
    int decoder_flushed;
    int eof;
    float* pending;
    int pending_samples;
    int pending_capacity_samples;
} DSREDecoder;

typedef struct DSREEncoder {
    AVFormatContext* out_fmt;
    AVCodecContext* enc_ctx;
    AVStream* stream;
    const AVCodec* encoder;
    SwrContext* swr;
    AVChannelLayout in_ch_layout;
    int in_layout_ready;
    AVFrame* in_frame;
    AVFrame* enc_frame;
    int sr;
    int channels;
    int frame_size;
    int64_t pts;
    float* fifo;
    int fifo_samples;
    int fifo_capacity_samples;
    int closed;
} DSREEncoder;

DSRE_EXPORT void dsre_decoder_close(void* decoder);
DSRE_EXPORT void dsre_encoder_abort(void* encoder);


static int dsre_decoder_pending_reserve(DSREDecoder* d, int need_samples) {
    float* nb;
    int new_cap;
    if (!d || need_samples <= d->pending_capacity_samples) return DSRE_OK;
    new_cap = d->pending_capacity_samples > 0 ? d->pending_capacity_samples : 4096;
    while (new_cap < need_samples) {
        if (new_cap > INT32_MAX / 2) {
            dsre_set_error("decoder pending buffer too large");
            return DSRE_ERR_ALLOC;
        }
        new_cap *= 2;
    }
    nb = (float*)av_realloc_f(d->pending, (size_t)new_cap * d->channels, sizeof(float));
    if (!nb) {
        av_free(d->pending);
        d->pending = NULL;
        d->pending_capacity_samples = 0;
        d->pending_samples = 0;
        dsre_set_error("failed to grow decoder pending buffer");
        return DSRE_ERR_ALLOC;
    }
    d->pending = nb;
    d->pending_capacity_samples = new_cap;
    return DSRE_OK;
}

static int dsre_decoder_push_samples(
    DSREDecoder* d,
    const float* src,
    int samples,
    float* dst,
    int max_samples,
    int* produced
) {
    int room;
    int copy_now;
    int remain;
    int ret;
    if (!d || !src || samples <= 0 || !dst || !produced) return DSRE_OK;
    room = max_samples - *produced;
    copy_now = samples < room ? samples : room;
    if (copy_now > 0) {
        memcpy(dst + ((int64_t)(*produced) * d->channels), src, (size_t)copy_now * d->channels * sizeof(float));
        *produced += copy_now;
    }
    remain = samples - copy_now;
    if (remain > 0) {
        ret = dsre_decoder_pending_reserve(d, d->pending_samples + remain);
        if (ret != DSRE_OK) return ret;
        memcpy(
            d->pending + ((int64_t)d->pending_samples * d->channels),
            src + ((int64_t)copy_now * d->channels),
            (size_t)remain * d->channels * sizeof(float)
        );
        d->pending_samples += remain;
    }
    return DSRE_OK;
}

static int dsre_decoder_convert_frame(DSREDecoder* d, AVFrame* frame, float* dst, int max_samples, int* produced) {
    int64_t delay;
    int out_count;
    int converted;
    int ret;
    float* tmp = NULL;
    uint8_t* out_planes[1];

    if (!d || !frame || !dst || !produced) return DSRE_ERR_ARG;
    delay = swr_get_delay(d->swr, d->input_sr);
    out_count = (int)av_rescale_rnd(delay + frame->nb_samples, d->target_sr, d->input_sr, AV_ROUND_UP);
    if (out_count <= 0) return DSRE_OK;

    tmp = (float*)av_malloc_array((size_t)out_count * d->channels, sizeof(float));
    if (!tmp) {
        dsre_set_error("decoder temp buffer allocation failed");
        return DSRE_ERR_ALLOC;
    }
    out_planes[0] = (uint8_t*)tmp;
    converted = swr_convert(
        d->swr,
        out_planes,
        out_count,
        (const uint8_t**)frame->extended_data,
        frame->nb_samples
    );
    if (converted < 0) {
        av_free(tmp);
        dsre_set_av_error("stream decoder swr_convert failed", converted);
        return DSRE_ERR_RESAMPLE;
    }
    ret = dsre_decoder_push_samples(d, tmp, converted, dst, max_samples, produced);
    av_free(tmp);
    return ret;
}

static int dsre_decoder_flush_swr(DSREDecoder* d, float* dst, int max_samples, int* produced) {
    int ret;
    while (*produced < max_samples) {
        int out_count = max_samples - *produced;
        int converted;
        uint8_t* out_planes[1];
        out_planes[0] = (uint8_t*)(dst + ((int64_t)(*produced) * d->channels));
        converted = swr_convert(d->swr, out_planes, out_count, NULL, 0);
        if (converted < 0) {
            dsre_set_av_error("stream decoder swr final flush failed", converted);
            return DSRE_ERR_RESAMPLE;
        }
        if (converted == 0) break;
        *produced += converted;
    }
    ret = DSRE_OK;
    return ret;
}

DSRE_EXPORT int dsre_decoder_open(
    const char* input_path,
    int target_sr,
    int preferred_chunk_samples,
    void** out_decoder,
    int* out_sr,
    int* out_channels
) {
    int ret;
    int output_channels;
    DSREDecoder* d = NULL;

    (void)preferred_chunk_samples;
    if (!input_path || target_sr <= 0 || !out_decoder || !out_sr || !out_channels) {
        dsre_set_error("invalid argument to dsre_decoder_open");
        return DSRE_ERR_ARG;
    }
    g_last_error[0] = '\0';
    *out_decoder = NULL;
    *out_sr = 0;
    *out_channels = 0;

    d = (DSREDecoder*)av_calloc(1, sizeof(DSREDecoder));
    if (!d) {
        dsre_set_error("DSREDecoder allocation failed");
        return DSRE_ERR_ALLOC;
    }
    d->audio_stream_index = -1;
    d->target_sr = target_sr;

    ret = avformat_open_input(&d->fmt_ctx, input_path, NULL, NULL);
    if (ret < 0) { dsre_set_av_error("stream decoder avformat_open_input failed", ret); ret = DSRE_ERR_FFMPEG; goto fail; }
    ret = avformat_find_stream_info(d->fmt_ctx, NULL);
    if (ret < 0) { dsre_set_av_error("stream decoder avformat_find_stream_info failed", ret); ret = DSRE_ERR_FFMPEG; goto fail; }

    ret = av_find_best_stream(d->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &d->decoder, 0);
    if (ret < 0) { dsre_set_av_error("stream decoder no audio stream", ret); ret = DSRE_ERR_STREAM; goto fail; }
    d->audio_stream_index = ret;

    d->dec_ctx = avcodec_alloc_context3(d->decoder);
    if (!d->dec_ctx) { dsre_set_error("stream decoder context allocation failed"); ret = DSRE_ERR_ALLOC; goto fail; }
    ret = avcodec_parameters_to_context(d->dec_ctx, d->fmt_ctx->streams[d->audio_stream_index]->codecpar);
    if (ret < 0) { dsre_set_av_error("stream decoder parameters_to_context failed", ret); ret = DSRE_ERR_CODEC; goto fail; }
    d->dec_ctx->thread_count = 0;
    d->dec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ret = avcodec_open2(d->dec_ctx, d->decoder, NULL);
    if (ret < 0) { dsre_set_av_error("stream decoder avcodec_open2 failed", ret); ret = DSRE_ERR_CODEC; goto fail; }

    d->input_sr = d->dec_ctx->sample_rate;
    if (d->input_sr <= 0) d->input_sr = target_sr;
    output_channels = d->dec_ctx->ch_layout.nb_channels;
    if (output_channels <= 0 && d->fmt_ctx->streams[d->audio_stream_index]->codecpar) {
        output_channels = d->fmt_ctx->streams[d->audio_stream_index]->codecpar->ch_layout.nb_channels;
    }
    if (output_channels <= 0) output_channels = 2;
    d->channels = output_channels;

    ret = av_channel_layout_copy(&d->in_ch_layout, &d->dec_ctx->ch_layout);
    if (ret < 0 || d->in_ch_layout.nb_channels <= 0) {
        if (ret >= 0) av_channel_layout_uninit(&d->in_ch_layout);
        av_channel_layout_default(&d->in_ch_layout, d->channels);
    }
    ret = av_channel_layout_copy(&d->out_ch_layout, &d->in_ch_layout);
    if (ret < 0 || d->out_ch_layout.nb_channels <= 0) {
        if (ret >= 0) av_channel_layout_uninit(&d->out_ch_layout);
        av_channel_layout_default(&d->out_ch_layout, d->channels);
    }
    d->channels = d->out_ch_layout.nb_channels;
    d->layout_ready = 1;

    ret = swr_alloc_set_opts2(
        &d->swr,
        &d->out_ch_layout,
        AV_SAMPLE_FMT_FLT,
        target_sr,
        &d->in_ch_layout,
        d->dec_ctx->sample_fmt,
        d->input_sr,
        0,
        NULL
    );
    if (ret < 0 || !d->swr) { dsre_set_av_error("stream decoder swr_alloc_set_opts2 failed", ret); ret = DSRE_ERR_RESAMPLE; goto fail; }
    ret = swr_init(d->swr);
    if (ret < 0) { dsre_set_av_error("stream decoder swr_init failed", ret); ret = DSRE_ERR_RESAMPLE; goto fail; }

    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame) { dsre_set_error("stream decoder packet/frame allocation failed"); ret = DSRE_ERR_ALLOC; goto fail; }

    *out_decoder = d;
    *out_sr = target_sr;
    *out_channels = d->channels;
    return DSRE_OK;

fail:
    dsre_decoder_close(d);
    return ret;
}

DSRE_EXPORT int dsre_decoder_read_f32(
    void* decoder,
    float* out_interleaved,
    int max_samples,
    int* out_samples,
    int* out_eof
) {
    DSREDecoder* d = (DSREDecoder*)decoder;
    int produced = 0;
    int ret;

    if (!d || !out_interleaved || max_samples <= 0 || !out_samples || !out_eof) {
        dsre_set_error("invalid argument to dsre_decoder_read_f32");
        return DSRE_ERR_ARG;
    }
    *out_samples = 0;
    *out_eof = 0;

    if (d->pending_samples > 0) {
        int copy_now = d->pending_samples < max_samples ? d->pending_samples : max_samples;
        memcpy(out_interleaved, d->pending, (size_t)copy_now * d->channels * sizeof(float));
        produced += copy_now;
        if (copy_now < d->pending_samples) {
            memmove(
                d->pending,
                d->pending + ((int64_t)copy_now * d->channels),
                (size_t)(d->pending_samples - copy_now) * d->channels * sizeof(float)
            );
        }
        d->pending_samples -= copy_now;
        if (produced >= max_samples) {
            *out_samples = produced;
            return DSRE_OK;
        }
    }

    while (produced < max_samples && !d->eof) {
        ret = avcodec_receive_frame(d->dec_ctx, d->frame);
        if (ret == 0) {
            ret = dsre_decoder_convert_frame(d, d->frame, out_interleaved, max_samples, &produced);
            av_frame_unref(d->frame);
            if (ret != DSRE_OK) return ret;
            continue;
        }
        if (ret == AVERROR_EOF) {
            d->eof = 1;
            break;
        }
        if (ret == AVERROR(EAGAIN)) {
            if (d->decoder_flushed) {
                d->eof = 1;
                break;
            }
            while (1) {
                ret = av_read_frame(d->fmt_ctx, d->pkt);
                if (ret == AVERROR_EOF) {
                    ret = avcodec_send_packet(d->dec_ctx, NULL);
                    d->decoder_flushed = 1;
                    if (ret < 0 && ret != AVERROR_EOF) {
                        dsre_set_av_error("stream decoder send flush failed", ret);
                        return DSRE_ERR_CODEC;
                    }
                    break;
                }
                if (ret < 0) {
                    dsre_set_av_error("stream decoder av_read_frame failed", ret);
                    return DSRE_ERR_FFMPEG;
                }
                if (d->pkt->stream_index != d->audio_stream_index) {
                    av_packet_unref(d->pkt);
                    continue;
                }
                ret = avcodec_send_packet(d->dec_ctx, d->pkt);
                av_packet_unref(d->pkt);
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    dsre_set_av_error("stream decoder avcodec_send_packet failed", ret);
                    return DSRE_ERR_CODEC;
                }
                break;
            }
            continue;
        }
        dsre_set_av_error("stream decoder avcodec_receive_frame failed", ret);
        return DSRE_ERR_CODEC;
    }

    if (d->eof && produced < max_samples) {
        ret = dsre_decoder_flush_swr(d, out_interleaved, max_samples, &produced);
        if (ret != DSRE_OK) return ret;
    }

    *out_samples = produced;
    if (d->eof && d->pending_samples == 0 && produced == 0) *out_eof = 1;
    else *out_eof = 0;
    return DSRE_OK;
}

DSRE_EXPORT void dsre_decoder_close(void* decoder) {
    DSREDecoder* d = (DSREDecoder*)decoder;
    if (!d) return;
    if (d->pending) av_free(d->pending);
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->frame) av_frame_free(&d->frame);
    if (d->swr) swr_free(&d->swr);
    if (d->dec_ctx) avcodec_free_context(&d->dec_ctx);
    if (d->fmt_ctx) avformat_close_input(&d->fmt_ctx);
    if (d->layout_ready) {
        av_channel_layout_uninit(&d->in_ch_layout);
        av_channel_layout_uninit(&d->out_ch_layout);
    }
    av_free(d);
}

static int dsre_encoder_fifo_reserve(DSREEncoder* e, int need_samples) {
    float* nb;
    int new_cap;
    if (!e || need_samples <= e->fifo_capacity_samples) return DSRE_OK;
    new_cap = e->fifo_capacity_samples > 0 ? e->fifo_capacity_samples : 8192;
    while (new_cap < need_samples) {
        if (new_cap > INT32_MAX / 2) {
            dsre_set_error("encoder fifo too large");
            return DSRE_ERR_ALLOC;
        }
        new_cap *= 2;
    }
    nb = (float*)av_realloc_f(e->fifo, (size_t)new_cap * e->channels, sizeof(float));
    if (!nb) {
        av_free(e->fifo);
        e->fifo = NULL;
        e->fifo_capacity_samples = 0;
        e->fifo_samples = 0;
        dsre_set_error("failed to grow encoder fifo");
        return DSRE_ERR_ALLOC;
    }
    e->fifo = nb;
    e->fifo_capacity_samples = new_cap;
    return DSRE_OK;
}

static int dsre_encoder_encode_one_frame(DSREEncoder* e, const float* src, int nb_samples) {
    int ret;
    int converted;
    if (!e || !src || nb_samples <= 0) return DSRE_OK;

    av_frame_unref(e->in_frame);
    e->in_frame->format = AV_SAMPLE_FMT_FLT;
    e->in_frame->sample_rate = e->sr;
    e->in_frame->nb_samples = nb_samples;
    av_channel_layout_default(&e->in_frame->ch_layout, e->channels);
    ret = av_frame_get_buffer(e->in_frame, 0);
    if (ret < 0) { dsre_set_av_error("stream encoder input frame buffer failed", ret); return DSRE_ERR_ALLOC; }
    memcpy(e->in_frame->data[0], src, (size_t)nb_samples * e->channels * sizeof(float));

    av_frame_unref(e->enc_frame);
    e->enc_frame->format = e->enc_ctx->sample_fmt;
    e->enc_frame->sample_rate = e->sr;
    e->enc_frame->nb_samples = nb_samples;
    ret = av_channel_layout_copy(&e->enc_frame->ch_layout, &e->enc_ctx->ch_layout);
    if (ret < 0) { dsre_set_av_error("stream encoder channel layout copy failed", ret); return DSRE_ERR_FFMPEG; }
    ret = av_frame_get_buffer(e->enc_frame, 0);
    if (ret < 0) { dsre_set_av_error("stream encoder frame buffer failed", ret); return DSRE_ERR_ALLOC; }

    converted = swr_convert(e->swr, e->enc_frame->extended_data, nb_samples, (const uint8_t**)e->in_frame->extended_data, nb_samples);
    if (converted < 0) { dsre_set_av_error("stream encoder swr_convert failed", converted); return DSRE_ERR_RESAMPLE; }
    e->enc_frame->nb_samples = converted;
    e->enc_frame->pts = e->pts;
    e->pts += converted;
    return dsre_send_frame_and_write(e->out_fmt, e->enc_ctx, e->stream, e->enc_frame);
}

static int dsre_encoder_drain_fifo(DSREEncoder* e, int flush_all) {
    int ret;
    while (e->fifo_samples >= e->frame_size || (flush_all && e->fifo_samples > 0)) {
        int nb = e->fifo_samples >= e->frame_size ? e->frame_size : e->fifo_samples;
        ret = dsre_encoder_encode_one_frame(e, e->fifo, nb);
        if (ret != DSRE_OK) return ret;
        if (nb < e->fifo_samples) {
            memmove(e->fifo, e->fifo + ((int64_t)nb * e->channels), (size_t)(e->fifo_samples - nb) * e->channels * sizeof(float));
        }
        e->fifo_samples -= nb;
    }
    return DSRE_OK;
}

DSRE_EXPORT int dsre_encoder_open(
    const char* original_path,
    const char* output_path,
    const char* format,
    int sr,
    int channels,
    void** out_encoder
) {
    int ret;
    enum AVCodecID codec_id;
    const char* encoder_name;
    AVPacket* cover_pkt = NULL;
    int cover_stream_index = -1;
    DSREEncoder* e = NULL;

    if (!output_path || !format || sr <= 0 || channels <= 0 || !out_encoder) {
        dsre_set_error("invalid argument to dsre_encoder_open");
        return DSRE_ERR_ARG;
    }
    g_last_error[0] = '\0';
    *out_encoder = NULL;

    codec_id = dsre_codec_id_from_format(format);
    encoder_name = dsre_encoder_name_from_format(format);
    const char* muxer_name = dsre_muxer_name_from_format(format);
    if (codec_id == AV_CODEC_ID_NONE || !encoder_name || !muxer_name) {
        dsre_set_error("stream encoder unsupported format: %s", format ? format : "(null)");
        return DSRE_ERR_UNSUPPORTED;
    }
    e = (DSREEncoder*)av_calloc(1, sizeof(DSREEncoder));
    if (!e) { dsre_set_error("DSREEncoder allocation failed"); return DSRE_ERR_ALLOC; }
    e->sr = sr;
    e->channels = channels;

    e->encoder = avcodec_find_encoder_by_name(encoder_name);
    if (!e->encoder && codec_id == AV_CODEC_ID_MP3) e->encoder = avcodec_find_encoder(codec_id);
    if (!e->encoder) { dsre_set_error("stream encoder not found: %s", encoder_name); ret = DSRE_ERR_CODEC; goto fail; }

    ret = avformat_alloc_output_context2(&e->out_fmt, NULL, muxer_name, output_path);
    if (ret < 0 || !e->out_fmt) { dsre_set_av_error("stream encoder alloc output context failed", ret); ret = DSRE_ERR_FFMPEG; goto fail; }
    e->stream = avformat_new_stream(e->out_fmt, NULL);
    if (!e->stream) { dsre_set_error("stream encoder avformat_new_stream failed"); ret = DSRE_ERR_ALLOC; goto fail; }

    e->enc_ctx = avcodec_alloc_context3(e->encoder);
    if (!e->enc_ctx) { dsre_set_error("stream encoder context allocation failed"); ret = DSRE_ERR_ALLOC; goto fail; }
    e->enc_ctx->codec_id = codec_id;
    e->enc_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
    e->enc_ctx->sample_rate = sr;
    e->enc_ctx->bit_rate = (codec_id == AV_CODEC_ID_MP3) ? 320000 : 0;
    e->enc_ctx->time_base = (AVRational){1, sr};
    av_channel_layout_default(&e->enc_ctx->ch_layout, channels);

    if (e->encoder->sample_fmts) {
        const enum AVSampleFormat* p;
        e->enc_ctx->sample_fmt = e->encoder->sample_fmts[0];
        for (p = e->encoder->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p) {
            if (*p == AV_SAMPLE_FMT_FLTP || *p == AV_SAMPLE_FMT_FLT || *p == AV_SAMPLE_FMT_S32P || *p == AV_SAMPLE_FMT_S32 || *p == AV_SAMPLE_FMT_S16P || *p == AV_SAMPLE_FMT_S16) {
                e->enc_ctx->sample_fmt = *p;
                break;
            }
        }
    } else {
        e->enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }
    if (e->out_fmt->oformat->flags & AVFMT_GLOBALHEADER) e->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    e->enc_ctx->thread_count = 0;
    e->enc_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ret = avcodec_open2(e->enc_ctx, e->encoder, NULL);
    if (ret < 0) { dsre_set_av_error("stream encoder avcodec_open2 failed", ret); ret = DSRE_ERR_CODEC; goto fail; }
    ret = avcodec_parameters_from_context(e->stream->codecpar, e->enc_ctx);
    if (ret < 0) { dsre_set_av_error("stream encoder parameters_from_context failed", ret); ret = DSRE_ERR_FFMPEG; goto fail; }
    e->stream->time_base = e->enc_ctx->time_base;

    ret = dsre_copy_metadata_and_cover_from_original(e->out_fmt, original_path, &cover_pkt, &cover_stream_index);
    if (ret != DSRE_OK) goto fail;

    if (!(e->out_fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&e->out_fmt->pb, output_path, AVIO_FLAG_WRITE);
        if (ret < 0) { dsre_set_av_error("stream encoder avio_open failed", ret); ret = DSRE_ERR_IO; goto fail; }
    }
    ret = avformat_write_header(e->out_fmt, NULL);
    if (ret < 0) { dsre_set_av_error("stream encoder avformat_write_header failed", ret); ret = DSRE_ERR_IO; goto fail; }

    /* Best-effort explicit cover packet write for muxers/players that do not pick it up from attached_pic alone. */
    if (cover_pkt && cover_stream_index >= 0) {
        int cover_ret;
        cover_pkt->stream_index = cover_stream_index;
        cover_pkt->pts = 0;
        cover_pkt->dts = 0;
        cover_pkt->duration = 0;
        cover_pkt->flags |= AV_PKT_FLAG_KEY;
        cover_ret = av_interleaved_write_frame(e->out_fmt, cover_pkt);
        if (cover_ret < 0) {
            av_packet_unref(cover_pkt);
        }
        av_packet_free(&cover_pkt);
    }

    av_channel_layout_default(&e->in_ch_layout, channels);
    e->in_layout_ready = 1;
    ret = swr_alloc_set_opts2(&e->swr, &e->enc_ctx->ch_layout, e->enc_ctx->sample_fmt, sr, &e->in_ch_layout, AV_SAMPLE_FMT_FLT, sr, 0, NULL);
    if (ret < 0 || !e->swr) { dsre_set_av_error("stream encoder swr_alloc_set_opts2 failed", ret); ret = DSRE_ERR_RESAMPLE; goto fail; }
    ret = swr_init(e->swr);
    if (ret < 0) { dsre_set_av_error("stream encoder swr_init failed", ret); ret = DSRE_ERR_RESAMPLE; goto fail; }

    e->frame_size = e->enc_ctx->frame_size > 0 ? e->enc_ctx->frame_size : 1024;
    e->in_frame = av_frame_alloc();
    e->enc_frame = av_frame_alloc();
    if (!e->in_frame || !e->enc_frame) { dsre_set_error("stream encoder frame allocation failed"); ret = DSRE_ERR_ALLOC; goto fail; }

    *out_encoder = e;
    return DSRE_OK;

fail:
    if (cover_pkt) av_packet_free(&cover_pkt);
    dsre_encoder_abort(e);
    return ret;
}

DSRE_EXPORT int dsre_encoder_write_f32(void* encoder, const float* pcm_interleaved, int samples) {
    DSREEncoder* e = (DSREEncoder*)encoder;
    int ret;
    if (!e || !pcm_interleaved || samples < 0) {
        dsre_set_error("invalid argument to dsre_encoder_write_f32");
        return DSRE_ERR_ARG;
    }
    if (samples == 0) return DSRE_OK;
    ret = dsre_encoder_fifo_reserve(e, e->fifo_samples + samples);
    if (ret != DSRE_OK) return ret;
    memcpy(e->fifo + ((int64_t)e->fifo_samples * e->channels), pcm_interleaved, (size_t)samples * e->channels * sizeof(float));
    e->fifo_samples += samples;
    return dsre_encoder_drain_fifo(e, 0);
}

DSRE_EXPORT int dsre_encoder_close(void* encoder) {
    DSREEncoder* e = (DSREEncoder*)encoder;
    int ret;
    if (!e) return DSRE_OK;
    ret = dsre_encoder_drain_fifo(e, 1);
    if (ret == DSRE_OK) ret = dsre_send_frame_and_write(e->out_fmt, e->enc_ctx, e->stream, NULL);
    if (ret == DSRE_OK) {
        int tr = av_write_trailer(e->out_fmt);
        if (tr < 0) { dsre_set_av_error("stream encoder av_write_trailer failed", tr); ret = DSRE_ERR_IO; }
    }
    dsre_encoder_abort(e);
    return ret;
}

DSRE_EXPORT void dsre_encoder_abort(void* encoder) {
    DSREEncoder* e = (DSREEncoder*)encoder;
    if (!e) return;
    if (e->fifo) av_free(e->fifo);
    if (e->in_frame) av_frame_free(&e->in_frame);
    if (e->enc_frame) av_frame_free(&e->enc_frame);
    if (e->swr) swr_free(&e->swr);
    if (e->enc_ctx) avcodec_free_context(&e->enc_ctx);
    if (e->in_layout_ready) av_channel_layout_uninit(&e->in_ch_layout);
    if (e->out_fmt) {
        if (!(e->out_fmt->oformat->flags & AVFMT_NOFILE) && e->out_fmt->pb) avio_closep(&e->out_fmt->pb);
        avformat_free_context(e->out_fmt);
    }
    av_free(e);
}
