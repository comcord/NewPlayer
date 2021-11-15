//
//  ff_new_player_def.h
//  IJKMediaPlayer
//
//  Created by dutingfu on 2021/11/14.
//  Copyright © 2021 bilibili. All rights reserved.
//

#ifndef ff_new_player_def_h
#define ff_new_player_def_h
#include "ff_ffplay_def.h"

typedef struct PlayerState {
    AVStream *video_st;
    PacketQueue videoq;
    
    AVStream *audio_st;
    PacketQueue audioq;
    
    FrameQueue pictq;
    FrameQueue sampq;
    SDL_Thread *video_refresh_tid;
    SDL_Thread _video_refresh_tid;
    
    SDL_Thread *read_tid;
    SDL_Thread _read_tid;
    char *filename;
    AVInputFormat *iformat;
    AVFormatContext *ic;
    bool abort_request;
    struct AudioParams audio_tgt;
    int audio_stream;
    int video_stream;
    PacketQueue *buffer_indicator_queue;
    
    Decoder auddec;
    Decoder viddec;
    SDL_cond *continue_read_thread;

} PlayerState;


typedef struct NewFFPlayer{
    PlayerState * videoState;
    char *iformat_name;
    SDL_Aout *aout;
    SDL_Vout *vout;
    
} NewFFPlayer;

int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);
#endif /* ff_new_player_def_h */
