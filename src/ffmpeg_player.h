#ifndef FFMPEG_PLAYER_H
#define FFMPEG_PLAYER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool running;
    bool idle;
    bool paused;
    double time_pos;
    double duration;
    double volume;
    char error[256];
} FfmpegPlayerStatus;

typedef struct FfmpegPlayer FfmpegPlayer;

bool ffmpeg_player_compiled(void);
FfmpegPlayer *ffmpeg_player_create(char *error, size_t error_size);
void ffmpeg_player_destroy(FfmpegPlayer *player);
int ffmpeg_player_load(FfmpegPlayer *player, const char *path);
int ffmpeg_player_set_pause(FfmpegPlayer *player, bool pause);
int ffmpeg_player_seek_relative(FfmpegPlayer *player, double seconds);
int ffmpeg_player_set_volume(FfmpegPlayer *player, double volume);
int ffmpeg_player_stop(FfmpegPlayer *player);
int ffmpeg_player_poll(FfmpegPlayer *player, FfmpegPlayerStatus *status);
bool ffmpeg_player_is_running(FfmpegPlayer *player);
const char *ffmpeg_player_error(const FfmpegPlayer *player);

#endif
