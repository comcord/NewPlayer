//
//  ff_new_player.c
//  IJKMediaPlayer
//
//  Created by dutingfu on 2021/11/14.
//  Copyright © 2021 bilibili. All rights reserved.
//

#include "ff_new_player.h"

#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#if CONFIG_AVDEVICE
#include "libavdevice/avdevice.h"
#endif
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

#if CONFIG_AVFILTER
# include "libavcodec/avcodec.h"
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif

#include "ijksdl/ijksdl_log.h"
#include <stdio.h>

int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);
int packet_queue_init(PacketQueue *q);
void print_error(const char *filename, int err);
static int video_refresh_thread(void *arg){
    return 0;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len){
    
}

static int audio_open(FFPlayer *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    NewFFPlayer *ffp = opaque;
    PlayerState *is = ffp->videoState;
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
#ifdef FFP_MERGE
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
#endif
    static const int next_sample_rates[] = {0, 44100, 48000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AoutGetAudioPerSecondCallBacks(ffp->aout)));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (SDL_AoutOpenAudio(ffp->aout, &wanted_spec, &spec) < 0) {
        /* avoid infinity loop on exit. --by bbcallen */
        if (is->abort_request)
            return -1;
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    SDL_AoutSetDefaultLatencySeconds(ffp->aout, ((double)(2 * spec.size)) / audio_hw_params->bytes_per_sec);
    return spec.size;
}


static int stream_component_open(NewFFPlayer *ffp, int stream_index)
{
    PlayerState *is = ffp->videoState;
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec = NULL;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
//    int stream_lowres = ffp->lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
//        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name = ffp->audio_codec_name;
            break;
//        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = ffp->subtitle_codec_name; break;
//        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name = ffp->video_codec_name;
//            break;
        default: break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with id %d\n", avctx->codec_id);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
//    if(stream_lowres > av_codec_get_max_lowres(codec)){
//        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
//                av_codec_get_max_lowres(codec));
//        stream_lowres = av_codec_get_max_lowres(codec);
//    }
//    av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
    if(stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
//    if (ffp->fast)
//        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
    if(codec->capabilities & AV_CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif

//    opts = filter_codec_opts(ffp->codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
//    if (!av_dict_get(opts, "threads", NULL, 0))
//        av_dict_set(&opts, "threads", "auto", 0);
//    if (stream_lowres)
//        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
#ifdef FFP_MERGE
        ret =  AVERROR_OPTION_NOT_FOUND;
        goto fail;
#endif
    }

//    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterContext *sink;

            is->audio_filter_src.freq           = avctx->sample_rate;
            is->audio_filter_src.channels       = avctx->channels;
            is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            SDL_LockMutex(ffp->af_mutex);
            if ((ret = configure_audio_filters(ffp, ffp->afilters, 0)) < 0) {
                SDL_UnlockMutex(ffp->af_mutex);
                goto fail;
            }
            ffp->af_changed = 0;
            SDL_UnlockMutex(ffp->af_mutex);
            sink = is->out_audio_filter;
            sample_rate    = av_buffersink_get_sample_rate(sink);
            nb_channels    = av_buffersink_get_channels(sink);
            channel_layout = av_buffersink_get_channel_layout(sink);
        }
#else
        sample_rate    = avctx->sample_rate;
        nb_channels    = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif

        /* prepare audio output */
        if ((ret = audio_open(ffp, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        ffp_set_audio_codec_info(ffp, AVCODEC_MODULE_NAME, avcodec_get_name(avctx->codec_id));
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = 2.0 * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, ffp, "ff_audio_dec")) < 0)
            goto out;
        SDL_AoutPauseAudio(ffp->aout, 0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        if (ffp->async_init_decoder) {
            while (!is->initialized_decoder) {
                SDL_Delay(5);
            }
            if (ffp->node_vdec) {
                is->viddec.avctx = avctx;
                ret = ffpipeline_config_video_decoder(ffp->pipeline, ffp);
            }
            if (ret || !ffp->node_vdec) {
                decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
                ffp->node_vdec = ffpipeline_open_video_decoder(ffp->pipeline, ffp);
                if (!ffp->node_vdec)
                    goto fail;
            }
        } else {
            decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
            ffp->node_vdec = ffpipeline_open_video_decoder(ffp->pipeline, ffp);
            if (!ffp->node_vdec)
                goto fail;
        }
        if ((ret = decoder_start(&is->viddec, video_thread, ffp, "ff_video_dec")) < 0)
            goto out;

        is->queue_attachments_req = 1;

        if (ffp->max_fps >= 0) {
            if(is->video_st->avg_frame_rate.den && is->video_st->avg_frame_rate.num) {
                double fps = av_q2d(is->video_st->avg_frame_rate);
                SDL_ProfilerReset(&is->viddec.decode_profiler, fps + 0.5);
                if (fps > ffp->max_fps && fps < 130.0) {
                    is->is_video_high_fps = 1;
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (too high)\n", fps);
                } else {
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (normal)\n", fps);
                }
            }
            if(is->video_st->r_frame_rate.den && is->video_st->r_frame_rate.num) {
                double tbr = av_q2d(is->video_st->r_frame_rate);
                if (tbr > ffp->max_fps && tbr < 130.0) {
                    is->is_video_high_fps = 1;
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (too high)\n", tbr);
                } else {
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (normal)\n", tbr);
                }
            }
        }

        if (is->is_video_high_fps) {
            avctx->skip_frame       = FFMAX(avctx->skip_frame, AVDISCARD_NONREF);
            avctx->skip_loop_filter = FFMAX(avctx->skip_loop_filter, AVDISCARD_NONREF);
            avctx->skip_idct        = FFMAX(avctx->skip_loop_filter, AVDISCARD_NONREF);
        }

        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (!ffp->subtitle) break;

        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        ffp_set_subtitle_codec_info(ffp, AVCODEC_MODULE_NAME, avcodec_get_name(avctx->codec_id));

        decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
        if ((ret = decoder_start(&is->subdec, subtitle_thread, ffp, "ff_subtitle_dec")) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}

 static int read_thread(void *arg){
     NewFFPlayer *ffp = arg;
     PlayerState *is = ffp->videoState;
     AVFormatContext *ic = NULL;
     AVPacket pkt1, *pkt = &pkt1;
     int err, i, ret __unused;
     int st_index[AVMEDIA_TYPE_NB];

     ic = avformat_alloc_context();
     if (!ic) {
         av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
         ret = AVERROR(ENOMEM);
         goto fail;
     }
     if (ffp->iformat_name)
         is->iformat = av_find_input_format(ffp->iformat_name);
//     ic->interrupt_callback.callback = decode_interrupt_cb;
     ic->interrupt_callback.opaque = arg;
     err = avformat_open_input(&ic, is->filename, is->iformat, NULL);
     if (err < 0) {
         print_error(is->filename, err);
         ret = -1;
         goto fail;
     }
     err = avformat_find_stream_info(ic, NULL);
     
     av_dump_format(ic, 0, is->filename, 0);

     int video_stream_count = 0;
     int h264_stream_count = 0;
     int first_h264_stream = -1;
     for (i = 0; i < ic->nb_streams; i++) {
         AVStream *st = ic->streams[i];
         enum AVMediaType type = st->codecpar->codec_type;
         st->discard = AVDISCARD_ALL;
         if (type >= 0&& st_index[type] == -1)
                 st_index[type] = i;
         // choose first h264

         if (type == AVMEDIA_TYPE_VIDEO) {
             enum AVCodecID codec_id = st->codecpar->codec_id;
             video_stream_count++;
             if (codec_id == AV_CODEC_ID_H264) {
                 h264_stream_count++;
                 if (first_h264_stream < 0)
                     first_h264_stream = i;
             }
         }
     }
     if (video_stream_count > 1 && st_index[AVMEDIA_TYPE_VIDEO] < 0) {
         st_index[AVMEDIA_TYPE_VIDEO] = first_h264_stream;
         av_log(NULL, AV_LOG_WARNING, "multiple video stream found, prefer first h264 stream: %d\n", first_h264_stream);
     }
     
     /* open the streams */
     if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
         stream_component_open(ffp, st_index[AVMEDIA_TYPE_AUDIO]);
     } else {
//         ffp->av_sync_type = AV_SYNC_VIDEO_MASTER;
//         is->av_sync_type  = ffp->av_sync_type;
     }

     ret = -1;
     if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
         ret = stream_component_open(ffp, st_index[AVMEDIA_TYPE_VIDEO]);
     }
     
     if (is->audio_stream >= 0) {
         is->audioq.is_buffer_indicator = 1;
         is->buffer_indicator_queue = &is->audioq;
     } else if (is->video_stream >= 0) {
         is->videoq.is_buffer_indicator = 1;
         is->buffer_indicator_queue = &is->videoq;
     } else {
         assert("invalid streams");
     }
     
     for (;;) {
         if (is->abort_request)
             break;
 #ifdef FFP_MERGE
//         if (is->paused != is->last_paused) {
//             is->last_paused = is->paused;
//             if (is->paused)
//                 is->read_pause_return = av_read_pause(ic);
//             else
//                 av_read_play(ic);
//         }
 #endif
 #if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
//         if (is->paused &&
//                 (!strcmp(ic->iformat->name, "rtsp") ||
//                  (ic->pb && !strncmp(ffp->input_filename, "mmsh:", 5)))) {
//             /* wait 10 ms to avoid trying to get another packet */
//             /* XXX: horrible */
//             SDL_Delay(10);
//             continue;
//         }
 #endif
//         if (is->seek_req) {
//             int64_t seek_target = is->seek_pos;
//             int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
//             int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// // FIXME the +-2 is due to rounding being not done in the correct direction in generation
// //      of the seek_pos/seek_rel variables
//
//             ffp_toggle_buffering(ffp, 1);
//             ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_UPDATE, 0, 0);
//             ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
//             if (ret < 0) {
//                 av_log(NULL, AV_LOG_ERROR,
//                        "%s: error while seeking\n", is->ic->filename);
//             } else {
//                 if (is->audio_stream >= 0) {
//                     packet_queue_flush(&is->audioq);
//                     packet_queue_put(&is->audioq, &flush_pkt);
//                     // TODO: clear invaild audio data
//                     // SDL_AoutFlushAudio(ffp->aout);
//                 }
//                 if (is->subtitle_stream >= 0) {
//                     packet_queue_flush(&is->subtitleq);
//                     packet_queue_put(&is->subtitleq, &flush_pkt);
//                 }
//                 if (is->video_stream >= 0) {
//                     if (ffp->node_vdec) {
//                         ffpipenode_flush(ffp->node_vdec);
//                     }
//                     packet_queue_flush(&is->videoq);
//                     packet_queue_put(&is->videoq, &flush_pkt);
//                 }
//                 if (is->seek_flags & AVSEEK_FLAG_BYTE) {
//                    set_clock(&is->extclk, NAN, 0);
//                 } else {
//                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
//                 }
//
//                 is->latest_video_seek_load_serial = is->videoq.serial;
//                 is->latest_audio_seek_load_serial = is->audioq.serial;
//                 is->latest_seek_load_start_at = av_gettime();
//             }
//             ffp->dcc.current_high_water_mark_in_ms = ffp->dcc.first_high_water_mark_in_ms;
//             is->seek_req = 0;
//             is->queue_attachments_req = 1;
//             is->eof = 0;
// #ifdef FFP_MERGE
//             if (is->paused)
//                 step_to_next_frame(is);
// #endif
//             completed = 0;
//             SDL_LockMutex(ffp->is->play_mutex);
//             if (ffp->auto_resume) {
//                 is->pause_req = 0;
//                 if (ffp->packet_buffering)
//                     is->buffering_on = 1;
//                 ffp->auto_resume = 0;
//                 stream_update_pause_l(ffp);
//             }
//             if (is->pause_req)
//                 step_to_next_frame_l(ffp);
//             SDL_UnlockMutex(ffp->is->play_mutex);
//
//             if (ffp->enable_accurate_seek) {
//                 is->drop_aframe_count = 0;
//                 is->drop_vframe_count = 0;
//                 SDL_LockMutex(is->accurate_seek_mutex);
//                 if (is->video_stream >= 0) {
//                     is->video_accurate_seek_req = 1;
//                 }
//                 if (is->audio_stream >= 0) {
//                     is->audio_accurate_seek_req = 1;
//                 }
//                 SDL_CondSignal(is->audio_accurate_seek_cond);
//                 SDL_CondSignal(is->video_accurate_seek_cond);
//                 SDL_UnlockMutex(is->accurate_seek_mutex);
//             }
//
//             ffp_notify_msg3(ffp, FFP_MSG_SEEK_COMPLETE, (int)fftime_to_milliseconds(seek_target), ret);
//             ffp_toggle_buffering(ffp, 1);
//         }
         
//         if (is->queue_attachments_req) {
//             if (is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
//                 AVPacket copy = { 0 };
//                 if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
//                     goto fail;
//                 packet_queue_put(&is->videoq, &copy);
//                 packet_queue_put_nullpacket(&is->videoq, is->video_stream);
//             }
//             is->queue_attachments_req = 0;
//         }
//
//         /* if the queue are full, no need to read more */
//         if (ffp->infinite_buffer<1 && !is->seek_req &&
// #ifdef FFP_MERGE
//               (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
// #else
//               (is->audioq.size + is->videoq.size + is->subtitleq.size > ffp->dcc.max_buffer_size
// #endif
//             || (   stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq, MIN_FRAMES)
//                 && stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq, MIN_FRAMES)
//                 && stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq, MIN_FRAMES)))) {
//             if (!is->eof) {
//                 ffp_toggle_buffering(ffp, 0);
//             }
//             /* wait 10 ms */
//             SDL_LockMutex(wait_mutex);
//             SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
//             SDL_UnlockMutex(wait_mutex);
//             continue;
//         }
//         if ((!is->paused || completed) &&
//             (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
//             (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
//             if (ffp->loop != 1 && (!ffp->loop || --ffp->loop)) {
//                 stream_seek(is, ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0, 0, 0);
//             } else if (ffp->autoexit) {
//                 ret = AVERROR_EOF;
//                 goto fail;
//             } else {
//                 ffp_statistic_l(ffp);
//                 if (completed) {
//                     av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: eof\n");
//                     SDL_LockMutex(wait_mutex);
//                     // infinite wait may block shutdown
//                     while(!is->abort_request && !is->seek_req)
//                         SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 100);
//                     SDL_UnlockMutex(wait_mutex);
//                     if (!is->abort_request)
//                         continue;
//                 } else {
//                     completed = 1;
//                     ffp->auto_resume = 0;
//
//                     // TODO: 0 it's a bit early to notify complete here
//                     ffp_toggle_buffering(ffp, 0);
//                     toggle_pause(ffp, 1);
//                     if (ffp->error) {
//                         av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: error: %d\n", ffp->error);
//                         ffp_notify_msg1(ffp, FFP_MSG_ERROR);
//                     } else {
//                         av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: completed: OK\n");
//                         ffp_notify_msg1(ffp, FFP_MSG_COMPLETED);
//                     }
//                 }
//             }
//         }
         pkt->flags = 0;
         ret = av_read_frame(ic, pkt);
         if (ret < 0) {
             int pb_eof = 0;
             int pb_error = 0;
             if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                 ffp_check_buffering_l(ffp);
                 pb_eof = 1;
                 // check error later
             }
             if (ic->pb && ic->pb->error) {
                 pb_eof = 1;
                 pb_error = ic->pb->error;
             }
             if (ret == AVERROR_EXIT) {
                 pb_eof = 1;
                 pb_error = AVERROR_EXIT;
             }

             if (pb_eof) {
                 if (is->video_stream >= 0)
                     packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                 if (is->audio_stream >= 0)
                     packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                 if (is->subtitle_stream >= 0)
                     packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                 is->eof = 1;
             }
             if (pb_error) {
                 if (is->video_stream >= 0)
                     packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                 if (is->audio_stream >= 0)
                     packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
//                 if (is->subtitle_stream >= 0)
//                     packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                 is->eof = 1;
                 ffp->error = pb_error;
                 av_log(ffp, AV_LOG_ERROR, "av_read_frame error: %s\n", ffp_get_error_string(ffp->error));
                 // break;
             } else {
                 ffp->error = 0;
             }
             if (is->eof) {
                 ffp_toggle_buffering(ffp, 0);
                 SDL_Delay(100);
             }
//             SDL_LockMutex(wait_mutex);
//             SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
//             SDL_UnlockMutex(wait_mutex);
//             ffp_statistic_l(ffp);
             continue;
         } else {
             is->eof = 0;
         }

         if (pkt->flags & AV_PKT_FLAG_DISCONTINUITY) {
             if (is->audio_stream >= 0) {
                 packet_queue_put(&is->audioq, &flush_pkt);
             }
//             if (is->subtitle_stream >= 0) {
//                 packet_queue_put(&is->subtitleq, &flush_pkt);
//             }
             if (is->video_stream >= 0) {
                 packet_queue_put(&is->videoq, &flush_pkt);
             }
         }

         /* check if packet is in play range specified by user, then queue, otherwise discard */
         stream_start_time = ic->streams[pkt->stream_index]->start_time;
         pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
         pkt_in_play_range = ffp->duration == AV_NOPTS_VALUE ||
                 (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                 av_q2d(ic->streams[pkt->stream_index]->time_base) -
                 (double)(ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0) / 1000000
                 <= ((double)ffp->duration / 1000000);
         if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
             packet_queue_put(&is->audioq, pkt);
         } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                    && !(is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))) {
             packet_queue_put(&is->videoq, pkt);
         } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
             packet_queue_put(&is->subtitleq, pkt);
         } else {
             av_packet_unref(pkt);
         }

//         ffp_statistic_l(ffp);

//         if (ffp->ijkmeta_delay_init && !init_ijkmeta &&
//                 (ffp->first_video_frame_rendered || !is->video_st) && (ffp->first_audio_frame_rendered || !is->audio_st)) {
//             ijkmeta_set_avformat_context_l(ffp->meta, ic);
//             init_ijkmeta = 1;
//         }

//         if (ffp->packet_buffering) {
//             io_tick_counter = SDL_GetTickHR();
//             if ((!ffp->first_video_frame_rendered && is->video_st) || (!ffp->first_audio_frame_rendered && is->audio_st)) {
//                 if (abs((int)(io_tick_counter - prev_io_tick_counter)) > FAST_BUFFERING_CHECK_PER_MILLISECONDS) {
//                     prev_io_tick_counter = io_tick_counter;
//                     ffp->dcc.current_high_water_mark_in_ms = ffp->dcc.first_high_water_mark_in_ms;
//                     ffp_check_buffering_l(ffp);
//                 }
//             } else {
//                 if (abs((int)(io_tick_counter - prev_io_tick_counter)) > BUFFERING_CHECK_PER_MILLISECONDS) {
//                     prev_io_tick_counter = io_tick_counter;
//                     ffp_check_buffering_l(ffp);
//                 }
//             }
//         }
     }

     ret = 0;
  fail:
     if (ic && !is->ic)
         avformat_close_input(&ic);

     if (!ffp->prepared || !is->abort_request) {
         ffp->last_error = last_error;
         ffp_notify_msg2(ffp, FFP_MSG_ERROR, last_error);
     }
     
     
 fail:
    if (ic)
        avformat_close_input(&ic);
    return ret;
}

int  ffp_new_prepare_async_l(NewFFPlayer *ffp, const char *file_name){
    PlayerState * is = av_mallocz(sizeof(PlayerState));
    if (is == NULL) {
        printf("player malloc mem error\n");
        return -1;
    }
    
    int pictq_size = 100;
    
    
    if (frame_queue_init(&is->pictq, &is->videoq, pictq_size, 1) < 0)
        goto fail;
    
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 )
        goto fail;
    
    is->video_refresh_tid = SDL_CreateThreadEx(&is->_video_refresh_tid, video_refresh_thread, ffp, "ff_vout");
    if (!is->video_refresh_tid) {
        av_freep(&is);
        return -1;
    }

    is->read_tid = SDL_CreateThreadEx(&is->_read_tid, read_thread, ffp, "ff_read");
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        goto fail;
    }
    
fail:
//    is->initialized_decoder = 1;
//    is->abort_request = true;
//    if (is->video_refresh_tid)
//        SDL_WaitThread(is->video_refresh_tid, NULL);
//    stream_close(ffp);
    return 0;

}


