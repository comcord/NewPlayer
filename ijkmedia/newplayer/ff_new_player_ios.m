//
//  ff_new_player_ios.m
//  IJKMediaFramework
//
//  Created by DZ0401025 on 2021/11/15.
//  Copyright © 2021 bilibili. All rights reserved.
//

#import "ff_new_player_ios.h"
#import "ijksdl_vout_ios_gles2.h"

void ijkmp_new_ios_set_glview_l(NewFFPlayer *mp, IJKSDLGLView *glView)
{
    SDL_VoutIos_SetGLView(mp->vout, glView);
}
