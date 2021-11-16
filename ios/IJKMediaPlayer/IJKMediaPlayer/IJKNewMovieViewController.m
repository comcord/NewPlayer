//
//  IJKNewMovieViewController.m
//  IJKMediaPlayer
//
//  Created by DZ0401025 on 2021/11/16.
//  Copyright © 2021 bilibili. All rights reserved.
//

#import "IJKNewMovieViewController.h"
#import "ff_new_player.h"
#import "ff_new_player_ios.h"
#import "IJKSDLGLView.h"
@interface IJKNewMovieViewController ()
@property(nonatomic, copy) NSString * url;
@end

@implementation IJKNewMovieViewController
{
    NewFFPlayer * _player;
    IJKSDLGLView * _glView;
}
- (id)initWithContentURL:(NSString *)aUrl
{
    if (self = [super init]) {
        self.url = aUrl;
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // Do any additional setup after loading the view.
    _player = malloc(sizeof(NewFFPlayer));

    ffp_new_prepare_async_l(_player, [self.url UTF8String]);
    _glView = [[IJKSDLGLView alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    _glView.isThirdGLView = NO;
    [self.view addSubview:_glView];
    ijkmp_new_ios_set_glview_l(_player, _glView);

}

/*
#pragma mark - Navigation

// In a storyboard-based application, you will often want to do a little preparation before navigation
- (void)prepareForSegue:(UIStoryboardSegue *)segue sender:(id)sender {
    // Get the new view controller using [segue destinationViewController].
    // Pass the selected object to the new view controller.
}
*/

@end
