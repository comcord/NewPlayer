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
#include "ijkavformat.h"

#include <stdio.h>

static AVPacket flush_pkt;


int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);
int packet_queue_init(PacketQueue *q);
void print_error(const char *filename, int err);
int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);
int packet_queue_get_or_buffering(NewFFPlayer *ffp, PacketQueue *q, AVPacket *pkt, int *serial, int *finished){
    assert(finished);
//    if (!ffp->packet_buffering)
        return packet_queue_get(q, pkt, 1, serial);

//    while (1) {
//        int new_packet = packet_queue_get(q, pkt, 0, serial);
//        if (new_packet < 0)
//            return -1;
//        else if (new_packet == 0) {
//            if (q->is_buffer_indicator && !*finished)
//                ffp_toggle_buffering(ffp, 1);
//            new_packet = packet_queue_get(q, pkt, 1, serial);
//            if (new_packet < 0)
//                return -1;
//        }
//
//        if (*finished == *serial) {
//            av_packet_unref(pkt);
//            continue;
//        }
//        else
//            break;
//    }

    return 1;
}
Frame *frame_queue_peek_writable(FrameQueue *f);
void frame_queue_push(FrameQueue *f);

static void free_picture(Frame *vp)
{
    if (vp->bmp) {
        SDL_VoutFreeYUVOverlay(vp->bmp);
        vp->bmp = NULL;
    }
}

static void alloc_picture(NewFFPlayer *ffp, int frame_format)
{
    PlayerState *is = ffp->videoState;
    Frame *vp;
#ifdef FFP_MERGE
    int sdl_format;
#endif

    vp = &is->pictq.queue[is->pictq.windex];

    free_picture(vp);

#ifdef FFP_MERGE
    video_open(is, vp);
#endif

    SDL_VoutSetOverlayFormat(ffp->vout, ffp->overlay_format);
    vp->bmp = SDL_Vout_CreateOverlay(vp->width, vp->height,
                                   frame_format,
                                   ffp->vout);
#ifdef FFP_MERGE
    if (vp->format == AV_PIX_FMT_YUV420P)
        sdl_format = SDL_PIXELFORMAT_YV12;
    else
        sdl_format = SDL_PIXELFORMAT_ARGB8888;

    if (realloc_texture(&vp->bmp, sdl_format, vp->width, vp->height, SDL_BLENDMODE_NONE, 0) < 0) {
#else
    /* RV16, RV32 contains only one plane */
    if (!vp->bmp || (!vp->bmp->is_private && vp->bmp->pitches[0] < vp->width)) {
#endif
        /* SDL allocates a buffer smaller than requested if the video
         * overlay hardware is unable to support the requested size. */
        av_log(NULL, AV_LOG_FATAL,
               "Error: the video system does not support an image\n"
                        "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
                        "to reduce the image size.\n", vp->width, vp->height );
        free_picture(vp);
    }

    SDL_LockMutex(is->pictq.mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq.cond);
    SDL_UnlockMutex(is->pictq.mutex);
}

int queue_picture(NewFFPlayer *ffp, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial){
    PlayerState *is = ffp->videoState;
    Frame *vp;
    int video_accurate_seek_fail = 0;
    int64_t video_seek_pos = 0;
    int64_t now = 0;
    int64_t deviation = 0;

    int64_t deviation2 = 0;
    int64_t deviation3 = 0;

//    if (ffp->enable_accurate_seek && is->video_accurate_seek_req && !is->seek_req) {
//        if (!isnan(pts)) {
//            video_seek_pos = is->seek_pos;
//            is->accurate_seek_vframe_pts = pts * 1000 * 1000;
//            deviation = llabs((int64_t)(pts * 1000 * 1000) - is->seek_pos);
//            if ((pts * 1000 * 1000 < is->seek_pos) || deviation > MAX_DEVIATION) {
//                now = av_gettime_relative() / 1000;
//                if (is->drop_vframe_count == 0) {
//                    SDL_LockMutex(is->accurate_seek_mutex);
//                    if (is->accurate_seek_start_time <= 0 && (is->audio_stream < 0 || is->audio_accurate_seek_req)) {
//                        is->accurate_seek_start_time = now;
//                    }
//                    SDL_UnlockMutex(is->accurate_seek_mutex);
//                    av_log(NULL, AV_LOG_INFO, "video accurate_seek start, is->seek_pos=%lld, pts=%lf, is->accurate_seek_time = %lld\n", is->seek_pos, pts, is->accurate_seek_start_time);
//                }
//                is->drop_vframe_count++;
//
//                while (is->audio_accurate_seek_req && !is->abort_request) {
//                    int64_t apts = is->accurate_seek_aframe_pts ;
//                    deviation2 = apts - pts * 1000 * 1000;
//                    deviation3 = apts - is->seek_pos;
//
//                    if (deviation2 > -100 * 1000 && deviation3 < 0) {
//                        break;
//                    } else {
//                        av_usleep(20 * 1000);
//                    }
//                    now = av_gettime_relative() / 1000;
//                    if ((now - is->accurate_seek_start_time) > ffp->accurate_seek_timeout) {
//                        break;
//                    }
//                }
//
//                if ((now - is->accurate_seek_start_time) <= ffp->accurate_seek_timeout) {
//                    return 1;  // drop some old frame when do accurate seek
//                } else {
//                    av_log(NULL, AV_LOG_WARNING, "video accurate_seek is error, is->drop_vframe_count=%d, now = %lld, pts = %lf\n", is->drop_vframe_count, now, pts);
//                    video_accurate_seek_fail = 1;  // if KEY_FRAME interval too big, disable accurate seek
//                }
//            } else {
//                av_log(NULL, AV_LOG_INFO, "video accurate_seek is ok, is->drop_vframe_count =%d, is->seek_pos=%lld, pts=%lf\n", is->drop_vframe_count, is->seek_pos, pts);
//                if (video_seek_pos == is->seek_pos) {
//                    is->drop_vframe_count       = 0;
//                    SDL_LockMutex(is->accurate_seek_mutex);
//                    is->video_accurate_seek_req = 0;
//                    SDL_CondSignal(is->audio_accurate_seek_cond);
//                    if (video_seek_pos == is->seek_pos && is->audio_accurate_seek_req && !is->abort_request) {
//                        SDL_CondWaitTimeout(is->video_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
//                    } else {
//                        ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(pts * 1000));
//                    }
//                    if (video_seek_pos != is->seek_pos && !is->abort_request) {
//                        is->video_accurate_seek_req = 1;
//                        SDL_UnlockMutex(is->accurate_seek_mutex);
//                        return 1;
//                    }
//
//                    SDL_UnlockMutex(is->accurate_seek_mutex);
//                }
//            }
//        } else {
//            video_accurate_seek_fail = 1;
//        }
//
//        if (video_accurate_seek_fail) {
//            is->drop_vframe_count = 0;
//            SDL_LockMutex(is->accurate_seek_mutex);
//            is->video_accurate_seek_req = 0;
//            SDL_CondSignal(is->audio_accurate_seek_cond);
//            if (is->audio_accurate_seek_req && !is->abort_request) {
//                SDL_CondWaitTimeout(is->video_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
//            } else {
//                if (!isnan(pts)) {
//                    ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(pts * 1000));
//                } else {
//                    ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, 0);
//                }
//            }
//            SDL_UnlockMutex(is->accurate_seek_mutex);
//        }
//        is->accurate_seek_start_time = 0;
//        video_accurate_seek_fail = 0;
//        is->accurate_seek_vframe_pts = 0;
//    }

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
#ifdef FFP_MERGE
    vp->uploaded = 0;
#endif

    /* alloc or resize hardware picture buffer */
    if (!vp->bmp || !vp->allocated ||
        vp->width  != src_frame->width ||
        vp->height != src_frame->height ||
        vp->format != src_frame->format) {

        if (vp->width != src_frame->width || vp->height != src_frame->height)
            ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, src_frame->width, src_frame->height);

        vp->allocated = 0;
        vp->width = src_frame->width;
        vp->height = src_frame->height;
        vp->format = src_frame->format;

        /* the allocation must be done in the main thread to avoid
           locking problems. */
        alloc_picture(ffp, src_frame->format);

        if (is->videoq.abort_request)
            return -1;
    }

    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        /* get a pointer on the bitmap */
        SDL_VoutLockYUVOverlay(vp->bmp);

#ifdef FFP_MERGE
#if CONFIG_AVFILTER
        // FIXME use direct rendering
        av_image_copy(data, linesize, (const uint8_t **)src_frame->data, src_frame->linesize,
                        src_frame->format, vp->width, vp->height);
#else
        // sws_getCachedContext(...);
#endif
#endif
        // FIXME: set swscale options
        if (SDL_VoutFillFrameYUVOverlay(vp->bmp, src_frame) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            exit(1);
        }
        /* update the bitmap content */
        SDL_VoutUnlockYUVOverlay(vp->bmp);

        vp->pts = pts;
        vp->duration = duration;
        vp->pos = pos;
        vp->serial = serial;
        vp->sar = src_frame->sample_aspect_ratio;
        vp->bmp->sar_num = vp->sar.num;
        vp->bmp->sar_den = vp->sar.den;

#ifdef FFP_MERGE
        av_frame_move_ref(vp->frame, src_frame);
#endif
        frame_queue_push(&is->pictq);
        if (!is->viddec.first_frame_decoded) {
            ALOGD("Video: first frame decoded\n");
            ffp_notify_msg1(ffp, FFP_MSG_VIDEO_DECODED_START);
            is->viddec.first_frame_decoded_time = SDL_GetTickHR();
            is->viddec.first_frame_decoded = 1;
        }
    }
    return 0;
}

void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) ;
Frame *frame_queue_peek_readable(FrameQueue *f);
int frame_queue_nb_remaining(FrameQueue *f);
Frame *frame_queue_peek_last(FrameQueue *f);
void frame_queue_next(FrameQueue *f);
Frame *frame_queue_peek(FrameQueue *f);

static void video_image_display2(NewFFPlayer *ffp)
{
    VideoState *is = ffp->videoState;
    Frame *vp;
    Frame *sp = NULL;

    vp = frame_queue_peek_last(&is->pictq);

    if (vp->bmp) {
        if (is->subtitle_st) {
            if (frame_queue_nb_remaining(&is->subpq) > 0) {
                sp = frame_queue_peek(&is->subpq);

                if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                    if (!sp->uploaded) {
                        if (sp->sub.num_rects > 0) {
                            char buffered_text[4096];
                            if (sp->sub.rects[0]->text) {
                                strncpy(buffered_text, sp->sub.rects[0]->text, 4096);
                            }
                            else if (sp->sub.rects[0]->ass) {
                              //  parse_ass_subtitle(sp->sub.rects[0]->ass, buffered_text);
                            }
                            ffp_notify_msg4(ffp, FFP_MSG_TIMED_TEXT, 0, 0, buffered_text, sizeof(buffered_text));
                        }
                        sp->uploaded = 1;
                    }
                }
            }
        }
        
//        if (ffp->render_wait_start && !ffp->start_on_prepared && is->pause_req) {
//            if (!ffp->first_video_frame_rendered) {
//                ffp->first_video_frame_rendered = 1;
//                ffp_notify_msg1(ffp, FFP_MSG_VIDEO_RENDERING_START);
//            }
//            while (is->pause_req && !is->abort_request) {
//                SDL_Delay(20);
//            }
//        }
        SDL_VoutDisplayYUVOverlay(ffp->vout, vp->bmp);
        SDL_Delay(20);

//        ffp->stat.vfps = SDL_SpeedSamplerAdd(&ffp->vfps_sampler, FFP_SHOW_VFPS_FFPLAY, "vfps[ffplay]");
//        if (!ffp->first_video_frame_rendered) {
//            ffp->first_video_frame_rendered = 1;
//            ffp_notify_msg1(ffp, FFP_MSG_VIDEO_RENDERING_START);
//        }

//        if (is->latest_video_seek_load_serial == vp->serial) {
//            int latest_video_seek_load_serial = __atomic_exchange_n(&(is->latest_video_seek_load_serial), -1, memory_order_seq_cst);
//            if (latest_video_seek_load_serial == vp->serial) {
//                ffp->stat.latest_seek_load_duration = (av_gettime() - is->latest_seek_load_start_at) / 1000;
//                if (ffp->av_sync_type == AV_SYNC_VIDEO_MASTER) {
//                    ffp_notify_msg2(ffp, FFP_MSG_VIDEO_SEEK_RENDERING_START, 1);
//                } else {
//                    ffp_notify_msg2(ffp, FFP_MSG_VIDEO_SEEK_RENDERING_START, 0);
//                }
//            }
//        }
    }
}


static void video_display2(NewFFPlayer *ffp)
{
    PlayerState *is = ffp->videoState;
    if (is->video_st)
        video_image_display2(ffp);
}

static void video_refresh(NewFFPlayer *opaque, double *remaining_time)
{
    NewFFPlayer *ffp = opaque;
    PlayerState *is = ffp->videoState;
    double time;
    
    Frame *sp, *sp2;
    
    //    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
    //        check_external_clock_speed(is);
    
    //    if (!ffp->display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
    //        time = av_gettime_relative() / 1000000.0;
    //        if (is->force_refresh || is->last_vis_time + ffp->rdftspeed < time) {
    //            video_display2(ffp);
    //            is->last_vis_time = time;
    //        }
    //        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + ffp->rdftspeed - time);
    //    }
    time = av_gettime_relative() / 1000000.0;
    if (1) {
        // video_display2(ffp);
        // is->last_vis_time = time;
    }
    //*remaining_time = FFMIN(*remaining_time, is->last_vis_time + ffp->rdftspeed - time);
    
    
    if (is->video_st) {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;
            
            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);
            
//            if (vp->serial != is->videoq.serial) {
//                frame_queue_next(&is->pictq);
//                // goto retry;
//            }
            
            //            if (lastvp->serial != vp->serial)
            //                is->frame_timer = av_gettime_relative() / 1000000.0;
            //
            //            if (is->paused)
            //                goto display;
            
            /* compute nominal last_duration */
            //            last_duration = vp_duration(is, lastvp, vp);
            //            delay = compute_target_delay(ffp, last_duration, is);
            //
            //            time= av_gettime_relative()/1000000.0;
            //            if (isnan(is->frame_timer) || time < is->frame_timer)
            //                is->frame_timer = time;
            //            if (time < is->frame_timer + delay) {
            //                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
            //                goto display;
            //            }
            //
            //            is->frame_timer += delay;
            //            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
            //                is->frame_timer = time;
            //
            //            SDL_LockMutex(is->pictq.mutex);
            //            if (!isnan(vp->pts))
            //                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            //            SDL_UnlockMutex(is->pictq.mutex);
            //
            //            if (frame_queue_nb_remaining(&is->pictq) > 1) {
            //                Frame *nextvp = frame_queue_peek_next(&is->pictq);
            //                duration = vp_duration(is, vp, nextvp);
            //                if(!is->step && (ffp->framedrop > 0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration) {
            //                    frame_queue_next(&is->pictq);
            //                    goto retry;
            //                }
            //            }
            
            //            if (is->subtitle_st) {
            //                while (frame_queue_nb_remaining(&is->subpq) > 0) {
            //                    sp = frame_queue_peek(&is->subpq);
            //
            //                    if (frame_queue_nb_remaining(&is->subpq) > 1)
            //                        sp2 = frame_queue_peek_next(&is->subpq);
            //                    else
            //                        sp2 = NULL;
            //
            //                    if (sp->serial != is->subtitleq.serial
            //                        || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
            //                        || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
            //                    {
            //                        if (sp->uploaded) {
            //                            ffp_notify_msg4(ffp, FFP_MSG_TIMED_TEXT, 0, 0, "", 1);
            //                        }
            //                        frame_queue_next(&is->subpq);
            //                    } else {
            //                        break;
            //                    }
            //                }
            //            }
            
            frame_queue_next(&is->pictq);
            //is->force_refresh = 1;
            
            //            SDL_LockMutex(ffp->is->play_mutex);
            //            if (is->step) {
            //                is->step = 0;
            //                if (!is->paused)
            //                    stream_update_pause_l(ffp);
            //            }
            //            SDL_UnlockMutex(ffp->is->play_mutex);
        }
    display:
        /* display picture */
        video_display2(ffp);
    }
    //    is->force_refresh = 0;
    //    if (ffp->show_status) {
    //        static int64_t last_time;
    //        int64_t cur_time;
    //        int aqsize, vqsize, sqsize __unused;
    //        double av_diff;
    //
    //        cur_time = av_gettime_relative();
    //        if (!last_time || (cur_time - last_time) >= 30000) {
    //            aqsize = 0;
    //            vqsize = 0;
    //            sqsize = 0;
    //            if (is->audio_st)
    //                aqsize = is->audioq.size;
    //            if (is->video_st)
    //                vqsize = is->videoq.size;
    //#ifdef FFP_MERGE
    //            if (is->subtitle_st)
    //                sqsize = is->subtitleq.size;
    //#else
    //            sqsize = 0;
    //#endif
    //            av_diff = 0;
    //            if (is->audio_st && is->video_st)
    //                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
    //            else if (is->video_st)
    //                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
    //            else if (is->audio_st)
    //                av_diff = get_master_clock(is) - get_clock(&is->audclk);
    //            av_log(NULL, AV_LOG_INFO,
    //                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
    //                   get_master_clock(is),
    //                   (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
    //                   av_diff,
    //                   is->frame_drops_early + is->frame_drops_late,
    //                   aqsize / 1024,
    //                   vqsize / 1024,
    //                   sqsize,
    //                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
    //                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);
    //            fflush(stdout);
    //            last_time = cur_time;
    //        }
    //    }
}

static int video_refresh_thread(void *arg){
    NewFFPlayer *ffp = arg;
    PlayerState *is = ffp->videoState;
    double remaining_time = 0.0;
    while (!is->abort_request) {
        if (remaining_time > 0.0)
            av_usleep((int)(int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        video_refresh(ffp, &remaining_time);
    }

    return 0;
}

static void update_sample_display(PlayerState *is, short *samples, int samples_size)
{
//    int size, len;
//
//    size = samples_size / sizeof(short);
//    while (size > 0) {
//        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
//        if (len > size)
//            len = size;
//        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
//        samples += len;
//        is->sample_array_index += len;
//        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
//            is->sample_array_index = 0;
//        size -= len;
//    }
}

static int audio_decode_frame(NewFFPlayer *ffp)
{
    PlayerState *is = ffp->videoState;
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;



//    if (ffp->sync_av_start &&                       /* sync enabled */
//        is->video_st &&                             /* has video stream */
//        !is->viddec.first_frame_decoded &&          /* not hot */
//        is->viddec.finished != is->videoq.serial) { /* not finished */
//        /* waiting for first video frame */
//        Uint64 now = SDL_GetTickHR();
//        if (now < is->viddec.first_frame_decoded_time ||
//            now > is->viddec.first_frame_decoded_time + 2000) {
//            is->viddec.first_frame_decoded = 1;
//        } else {
//            /* video pipeline is not ready yet */
//            return -1;
//        }
//    }
reload:
    do {
//#if defined(_WIN32) || defined(__APPLE__)
//        while (frame_queue_nb_remaining(&is->sampq) == 0) {
//            if ((av_gettime_relative() - ffp->audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
//                return -1;
//            av_usleep (1000);
//        }
//#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, af->frame->channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    dec_channel_layout =
        (af->frame->channel_layout && af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
        af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
    //wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);
    wanted_nb_samples = af->frame->nb_samples;
    if (af->frame->format        != is->audio_src.fmt            ||
        dec_channel_layout       != is->audio_src.channel_layout ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        AVDictionary *swr_opts = NULL;
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL,
                                         is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                         dec_channel_layout,           af->frame->format, af->frame->sample_rate,
                                         0, NULL);
        if (!is->swr_ctx) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            return -1;
        }
        //av_dict_copy(&swr_opts, ffp->swr_opts, 0);
//        if (af->frame->channel_layout == AV_CH_LAYOUT_5POINT1_BACK)
//            av_opt_set_double(is->swr_ctx, "center_mix_level", ffp->preset_5_1_center_mix_level, 0);
        av_opt_set_dict(is->swr_ctx, &swr_opts);
        av_dict_free(&swr_opts);

        if (swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels       = af->frame->channels;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int)((int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256);
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);

        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        int bytes_per_sample = av_get_bytes_per_sample(is->audio_tgt.fmt);
        resampled_data_size = len2 * is->audio_tgt.channels * bytes_per_sample;
#if defined(__ANDROID__)
        if (ffp->soundtouch_enable && ffp->pf_playback_rate != 1.0f && !is->abort_request) {
            av_fast_malloc(&is->audio_new_buf, &is->audio_new_buf_size, out_size * translate_time);
            for (int i = 0; i < (resampled_data_size / 2); i++)
            {
                is->audio_new_buf[i] = (is->audio_buf1[i * 2] | (is->audio_buf1[i * 2 + 1] << 8));
            }

            int ret_len = ijk_soundtouch_translate(is->handle, is->audio_new_buf, (float)(ffp->pf_playback_rate), (float)(1.0f/ffp->pf_playback_rate),
                    resampled_data_size / 2, bytes_per_sample, is->audio_tgt.channels, af->frame->sample_rate);
            if (ret_len > 0) {
                is->audio_buf = (uint8_t*)is->audio_new_buf;
                resampled_data_size = ret_len;
            } else {
                translate_time++;
                goto reload;
            }
        }
#endif
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    //audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
//    if (!isnan(af->pts))
//        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
//    else
//        is->audio_clock = NAN;
//    is->audio_clock_serial = af->serial;
#ifdef FFP_SHOW_AUDIO_DELAY
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    if (!is->auddec.first_frame_decoded) {
        ALOGD("avcodec/Audio: first frame decoded\n");
        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_DECODED_START);
        is->auddec.first_frame_decoded_time = SDL_GetTickHR();
        is->auddec.first_frame_decoded = 1;
    }
    return resampled_data_size;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len){
    NewFFPlayer *ffp = opaque;
    PlayerState *is = ffp->videoState;
    int audio_size, len1;
    if (!ffp || !is) {
        memset(stream, 0, len);
        return;
    }

    int64_t audio_callback_time = av_gettime_relative();

//    if (ffp->pf_playback_rate_changed) {
//        ffp->pf_playback_rate_changed = 0;
//#if defined(__ANDROID__)
//        if (!ffp->soundtouch_enable) {
//            SDL_AoutSetPlaybackRate(ffp->aout, ffp->pf_playback_rate);
//        }
//#else
//        SDL_AoutSetPlaybackRate(ffp->aout, ffp->pf_playback_rate);
//#endif
//    }
//    if (ffp->pf_playback_volume_changed) {
//        ffp->pf_playback_volume_changed = 0;
//        SDL_AoutSetPlaybackVolume(ffp->aout, ffp->pf_playback_volume);
//    }

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           audio_size = audio_decode_frame(ffp);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf = NULL;
               is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
           } else {
               update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
//               if (is->show_mode != SHOW_MODE_VIDEO)
//                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        if (is->auddec.pkt_serial != is->audioq.serial) {
            is->audio_buf_index = is->audio_buf_size;
            memset(stream, 0, len);
            // stream += len;
            // len = 0;
            SDL_AoutFlushAudio(ffp->aout);
            break;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
          memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);

//        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
//            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
//        else {
            memset(stream, 0, len1);
////            if (!is->muted && is->audio_buf)
////                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
//        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
//    if (!isnan(is->audio_clock)) {
//        set_clock_at(&is->audclk, is->audio_clock - (double)(is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec - SDL_AoutGetLatencySeconds(ffp->aout), is->audio_clock_serial, ffp->audio_callback_time / 1000000.0);
//        sync_clock_to_slave(&is->extclk, &is->audclk);
//    }
//    if (!ffp->first_audio_frame_rendered) {
//        ffp->first_audio_frame_rendered = 1;
//        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_RENDERING_START);
//    }
//
//    if (is->latest_audio_seek_load_serial == is->audio_clock_serial) {
//        int latest_audio_seek_load_serial = __atomic_exchange_n(&(is->latest_audio_seek_load_serial), -1, memory_order_seq_cst);
//        if (latest_audio_seek_load_serial == is->audio_clock_serial) {
//            if (ffp->av_sync_type == AV_SYNC_AUDIO_MASTER) {
//                ffp_notify_msg2(ffp, FFP_MSG_AUDIO_SEEK_RENDERING_START, 1);
//            } else {
//                ffp_notify_msg2(ffp, FFP_MSG_AUDIO_SEEK_RENDERING_START, 0);
//            }
//        }
//    }

//    if (ffp->render_wait_start && !ffp->start_on_prepared && is->pause_req) {
//        while (is->pause_req && !is->abort_request) {
//            SDL_Delay(20);
//        }
//    }
}

static int audio_open(NewFFPlayer *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
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

static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

static int decoder_start(Decoder *d, int (*fn)(void *), void *arg, const char *name)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThreadEx(&d->_decoder_tid, fn, arg, name);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}


static int decoder_decode_frame(NewFFPlayer *ffp, Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;

        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                          //  ffp->stat.vdps = SDL_SpeedSamplerAdd(&ffp->vdps_sampler, FFP_SHOW_VDPS_AVCODEC, "vdps[avcodec]");
                            frame->pts = frame->best_effort_timestamp;

//                            if (ffp->decoder_reorder_pts == -1) {
//                                frame->pts = frame->best_effort_timestamp;
//                            } else if (!ffp->decoder_reorder_pts) {
//                                frame->pts = frame->pkt_dts;
//                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational){1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(d->avctx), tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                    default:
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                if (packet_queue_get_or_buffering(ffp, d->queue, &pkt, &d->pkt_serial, &d->finished) < 0)
                    return -1;
            }
        } while (d->queue->serial != d->pkt_serial);

        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {
                       d->packet_pending = 1;
                       av_packet_move_ref(&d->pkt, &pkt);
                    }
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);
        }
    }
}

static int audio_thread(void *arg)
{
    NewFFPlayer *ffp = arg;
    VideoState *is = ffp->videoState;
    AVFrame *frame = av_frame_alloc();
    Frame *af;

    int got_frame = 0;
    AVRational tb;
    int ret = 0;
  

    if (!frame)
        return AVERROR(ENOMEM);

    do {
//        ffp_audio_statistic_l(ffp);
        if ((got_frame = decoder_decode_frame(ffp, &is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
    av_frame_free(&frame);
    return ret;
}

static int get_video_frame(FFPlayer *ffp, AVFrame *frame)
{
    VideoState *is = ffp->is;
    int got_picture;

//    ffp_video_statistic_l(ffp);
    if ((got_picture = decoder_decode_frame(ffp, &is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

//        if (ffp->framedrop>0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
//            ffp->stat.decode_frame_count++;
//            if (frame->pts != AV_NOPTS_VALUE) {
//                double diff = dpts - get_master_clock(is);
//                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
//                    diff - is->frame_last_filter_delay < 0 &&
//                    is->viddec.pkt_serial == is->vidclk.serial &&
//                    is->videoq.nb_packets) {
//                    is->frame_drops_early++;
//                    is->continuous_frame_drops_early++;
//                    if (is->continuous_frame_drops_early > ffp->framedrop) {
//                        is->continuous_frame_drops_early = 0;
//                    } else {
//                        ffp->stat.drop_frame_count++;
//                        ffp->stat.drop_frame_rate = (float)(ffp->stat.drop_frame_count) / (float)(ffp->stat.decode_frame_count);
//                        av_frame_unref(frame);
//                        got_picture = 0;
//                    }
//                }
//            }
//        }
    }

    return got_picture;
}


static int video_thread(void *arg)
{
    NewFFPlayer *ffp = arg;
    PlayerState *is = ffp->videoState;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);
    if (!frame) {
        return AVERROR(ENOMEM);
    }
    
    for (;;) {
        ret = get_video_frame(ffp, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;
        
        duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        ret = queue_picture(ffp, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
        av_frame_unref(frame);
    }
   

 the_end:

    av_frame_free(&frame);
    return 0;
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
    avctx->codec_id = codec->id;
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    }
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:{
            sample_rate    = avctx->sample_rate;
            nb_channels    = avctx->channels;
            channel_layout = avctx->channel_layout;
            if ((ret = audio_open(ffp, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
                goto fail;
            decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);

            if ((ret = decoder_start(&is->auddec, audio_thread, ffp, "ff_audio_dec")) < 0)
                goto out;
        }
            break;
        case AVMEDIA_TYPE_VIDEO:{
            is->video_stream = stream_index;
            is->video_st = ic->streams[stream_index];
            decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);

            if ((ret = decoder_start(&is->viddec, video_thread, ffp, "ff_video_dec")) < 0){
                goto fail;
            }
            
            break;}
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
     SDL_mutex *wait_mutex = SDL_CreateMutex();


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
//             if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof)
             if ((ret == AVERROR_EOF || avio_feof(ic->pb))) {
               //  ffp_check_buffering_l(ffp);
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
//                 if (is->subtitle_stream >= 0)
//                     packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
//                 is->eof = 1;
             }
             if (pb_error) {
                 if (is->video_stream >= 0)
                     packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                 if (is->audio_stream >= 0)
                     packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
//                 if (is->subtitle_stream >= 0)
//                     packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                // is->eof = 1;
//                 ffp->error = pb_error;
//                 av_log(ffp, AV_LOG_ERROR, "av_read_frame error: %s\n", ffp_get_error_string(ffp->error));
                 // break;
             } else {
                 //ffp->error = 0;
             }
//             if (is->eof) {
//                 ffp_toggle_buffering(ffp, 0);
//                 SDL_Delay(100);
//             }
             SDL_LockMutex(wait_mutex);
             SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
             SDL_UnlockMutex(wait_mutex);
//             ffp_statistic_l(ffp);
             continue;
         } else {
           //  is->eof = 0;
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
//         stream_start_time = ic->streams[pkt->stream_index]->start_time;
//         pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
//         pkt_in_play_range = ffp->duration == AV_NOPTS_VALUE ||
//                 (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
//                 av_q2d(ic->streams[pkt->stream_index]->time_base) -
//                 (double)(ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0) / 1000000
//                 <= ((double)ffp->duration / 1000000);
         int pkt_in_play_range = 1;
         if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
             packet_queue_put(&is->audioq, pkt);
         } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                    && !(is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))) {
             packet_queue_put(&is->videoq, pkt);
             
         }
//         } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
//             packet_queue_put(&is->subtitleq, pkt);
          else {
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
  //   }
     }

     ret = 0;
 fail:
    if (ic)
        avformat_close_input(&ic);
     SDL_DestroyMutex(wait_mutex);
    return ret;
}

SDL_Aout *SDL_AoutIos_CreateForAudioUnit();
SDL_Vout *SDL_VoutIos_CreateForGLES2();


    
int  ffp_new_prepare_async_l(NewFFPlayer *ffp, const char *file_name){
    PlayerState * is = av_mallocz(sizeof(PlayerState));
    if (is == NULL) {
        printf("player malloc mem error\n");
        return -1;
    }
    
    ffp->vout = SDL_VoutIos_CreateForGLES2();
    ffp->aout = SDL_AoutIos_CreateForAudioUnit();
    ffp->overlay_format         = SDL_FCC_RV32;
    int pictq_size = 10;
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
    
    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
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


