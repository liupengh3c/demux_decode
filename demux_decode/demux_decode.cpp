﻿// demux_decode.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
}
AVFormatContext* fmt_ctx = NULL;
AVCodecContext* video_dec_ctx = NULL, * audio_dec_ctx;
int width, height;
enum AVPixelFormat pix_fmt;
AVStream* video_stream = NULL, * audio_stream = NULL;
const char* src_filename = NULL;
const char* video_dst_filename = NULL;
const char* audio_dst_filename = NULL;
FILE* video_dst_file = NULL;
FILE* audio_dst_file = NULL;

uint8_t* video_dst_data[4] = { NULL };
int      video_dst_linesize[4];
int video_dst_bufsize;

int video_stream_idx = -1, audio_stream_idx = -1;
AVFrame* frame = NULL;
AVPacket pkt;
int video_frame_count = 0;
int audio_frame_count = 0;

static int output_video_frame(AVFrame* frame)
{
    if (frame->width != width || frame->height != height ||
        frame->format != pix_fmt) {
        /* To handle this change, one could call av_image_alloc again and
         * decode the following frames into another rawvideo file. */
        fprintf(stderr, "Error: Width, height and pixel format have to be "
            "constant in a rawvideo file, but the width, height or "
            "pixel format of the input video changed:\n"
            "old: width = %d, height = %d, format = %s\n"
            "new: width = %d, height = %d, format = %s\n",
            width, height, av_get_pix_fmt_name(pix_fmt),
            frame->width, frame->height,
            av_get_pix_fmt_name(AVPixelFormat(frame->format)));
        return -1;
    }

    printf("video_frame n:%d coded_n:%d\n",
        video_frame_count++, frame->coded_picture_number);

    /* copy decoded frame to destination buffer:
     * this is required since rawvideo expects non aligned data */
    av_image_copy(video_dst_data, video_dst_linesize,
        (const uint8_t**)(frame->data), frame->linesize,
        pix_fmt, width, height);

    /* write to rawvideo file */
    int tmp = width * height;
    //fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
    fwrite(video_dst_data[0], 1, tmp, video_dst_file);
    fwrite(video_dst_data[1], 1, width * height / 4, video_dst_file);
    fwrite(video_dst_data[2], 1, width * height / 4, video_dst_file);

    // 写y分量
    //for (size_t i = 0; i < height; i++)
    //{
    //    fwrite(video_dst_data[0] + video_dst_linesize[0] * i, 1, width, video_dst_file);
    //}

    //// 写u分量
    //for (size_t i = 0; i < height/2; i++)
    //{
    //    fwrite(video_dst_data[1] + video_dst_linesize[1] * i, 1, width / 2, video_dst_file);
    //   
    //}
    //// 写v分量
    //for (size_t i = 0; i < height / 2; i++)
    //{
    //    fwrite(video_dst_data[2] + video_dst_linesize[2] * i, 1, width / 2, video_dst_file);
    //}
    return 0;
}

static int output_audio_frame(AVFrame* frame)
{
    size_t unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample(AVSampleFormat(frame->format));


    /* Write the raw audio data samples of the first plane. This works
     * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
     * most audio decoders output planar audio, which uses a separate
     * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
     * In other words, this code will write only the first audio channel
     * in these cases.
     * You should use libswresample or libavfilter to convert the frame
     * to packed data. */
    fwrite(frame->extended_data[0], 1, unpadded_linesize, audio_dst_file);

    return 0;
}

static int decode_packet(AVCodecContext* dec, const AVPacket* pkt)
{
    int ret = 0;

    // submit the packet to the decoder
    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0) {
        return ret;
    }

    // get all the available frames from the decoder
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret < 0) {
            // those two return values are special and mean there is no output
            // frame available, but there were no errors during decoding
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                return 0;

            return ret;
        }

        // write the frame data to output file
        if (dec->codec->type == AVMEDIA_TYPE_VIDEO)
            ret = output_video_frame(frame);
        else
            ret = output_audio_frame(frame);

        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int open_codec_context(int* stream_idx,
    AVCodecContext** dec_ctx, AVFormatContext* fmt_ctx, enum AVMediaType type)
{
    int ret, stream_index;
    AVStream* st;
    AVCodec* dec = NULL;
    AVDictionary* opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
            av_get_media_type_string(type), src_filename);
        return ret;
    }
    else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders */
        if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

static int get_format_from_sample_fmt(const char** fmt,
    enum AVSampleFormat sample_fmt)
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char* fmt_be, * fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry* entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr,
        "sample format %s is not supported as output format\n",
        av_get_sample_fmt_name(sample_fmt));
    return -1;
}

int main()
{
    int ret = 0;

    src_filename = "yues.mp4";
    video_dst_filename = "yueh.mp4.yuv";
    audio_dst_filename = "yueh.mp4.pcm";

    /* open input file, and allocate format context */
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", src_filename);
        return 0;
    }

    /* retrieve stream information */
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }

    if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
        video_stream = fmt_ctx->streams[video_stream_idx];

        video_dst_file = fopen(video_dst_filename, "wb+");
        if (!video_dst_file) {
            fprintf(stderr, "Could not open destination file %s\n", video_dst_filename);
            ret = 1;
            goto end;
        }

        /* allocate image where the decoded image will be put */
        width = video_dec_ctx->width;
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;
        ret = av_image_alloc(video_dst_data, video_dst_linesize,
            width, height, pix_fmt, 1);
        if (ret < 0) {
            fprintf(stderr, "Could not allocate raw video buffer\n");
            goto end;
        }
        video_dst_bufsize = ret;
    }

    if (open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
        audio_stream = fmt_ctx->streams[audio_stream_idx];
        audio_dst_file = fopen(audio_dst_filename, "wb");
        if (!audio_dst_file) {
            fprintf(stderr, "Could not open destination file %s\n", audio_dst_filename);
            ret = 1;
            goto end;
        }
    }

    /* dump input information to stderr */
    av_dump_format(fmt_ctx, 0, src_filename, 0);

    if (!audio_stream && !video_stream) {
        fprintf(stderr, "Could not find audio or video stream in the input, aborting\n");
        ret = 1;
        goto end;
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* initialize packet, set data to NULL, let the demuxer fill it */
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    if (video_stream)
        printf("Demuxing video from file '%s' into '%s'\n", src_filename, video_dst_filename);
    if (audio_stream)
        printf("Demuxing audio from file '%s' into '%s'\n", src_filename, audio_dst_filename);

    /* read frames from the file */
    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        // check if the packet belongs to a stream we are interested in, otherwise
        // skip it
        if (pkt.stream_index == video_stream_idx)
            ret = decode_packet(video_dec_ctx, &pkt);
        else if (pkt.stream_index == audio_stream_idx)
            ret = decode_packet(audio_dec_ctx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0)
            break;
    }

    /* flush the decoders */
    if (video_dec_ctx)
        decode_packet(video_dec_ctx, NULL);
    if (audio_dec_ctx)
        decode_packet(audio_dec_ctx, NULL);

    printf("Demuxing succeeded.\n");

    if (video_stream) {
        printf("Play the output video file with the command:\n"
            "ffplay -f rawvideo -pix_fmt %s -video_size %dx%d %s\n",
            av_get_pix_fmt_name(pix_fmt), width, height,
            video_dst_filename);
    }

    if (audio_stream) {
        enum AVSampleFormat sfmt = audio_dec_ctx->sample_fmt;
        int n_channels = audio_dec_ctx->channels;
        const char* fmt;

        if (av_sample_fmt_is_planar(sfmt)) {
            const char* packed = av_get_sample_fmt_name(sfmt);
            printf("Warning: the sample format the decoder produced is planar "
                "(%s). This example will output the first channel only.\n",
                packed ? packed : "?");
            sfmt = av_get_packed_sample_fmt(sfmt);
            n_channels = 1;
        }

        if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
            goto end;

        printf("Play the output audio file with the command:\n"
            "ffplay -f %s -ac %d -ar %d %s\n",
            fmt, n_channels, audio_dec_ctx->sample_rate,
            audio_dst_filename);
    }

end:
    avcodec_free_context(&video_dec_ctx);
    avcodec_free_context(&audio_dec_ctx);
    avformat_close_input(&fmt_ctx);
    if (video_dst_file)
        fclose(video_dst_file);
    if (audio_dst_file)
        fclose(audio_dst_file);
    av_frame_free(&frame);
    av_free(video_dst_data[0]);

    return ret < 0;
}

// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门使用技巧: 
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5. 转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件
