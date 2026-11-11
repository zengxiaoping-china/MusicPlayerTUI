#include "player.h"

#include "ffmpeg_player.h"
#include "mpv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct Player {
    PlayerBackend active;
    void *context;
    PlayerInfo info;
    char error[256];
    char notice[256];
};

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    size_t length;

    if (destination_size == 0)
        return;
    length = strlen(source);
    if (length >= destination_size)
        length = destination_size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

bool player_backend_from_string(const char *name, PlayerBackend *backend)
{
    if (name == NULL || backend == NULL)
        return false;

    if (strcasecmp(name, "auto") == 0)
        *backend = PLAYER_BACKEND_AUTO;
    else if (strcasecmp(name, "mpv") == 0)
        *backend = PLAYER_BACKEND_MPV;
    else if (strcasecmp(name, "ffmpeg") == 0)
        *backend = PLAYER_BACKEND_FFMPEG;
    else
        return false;
    return true;
}

const char *player_backend_name(PlayerBackend backend)
{
    switch (backend) {
    case PLAYER_BACKEND_MPV:
        return "mpv";
    case PLAYER_BACKEND_FFMPEG:
        return "FFmpeg";
    case PLAYER_BACKEND_AUTO:
    default:
        return "自动";
    }
}

bool player_backend_is_compiled(PlayerBackend backend)
{
    switch (backend) {
    case PLAYER_BACKEND_MPV:
        return true;
    case PLAYER_BACKEND_FFMPEG:
        return ffmpeg_player_compiled();
    case PLAYER_BACKEND_AUTO:
    default:
        return true;
    }
}

static int start_mpv(Player *player, char *error, size_t error_size)
{
    Mpv *mpv = calloc(1, sizeof(*mpv));

    if (mpv == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "内存不足");
        return -1;
    }

    if (mpv_start(mpv) != 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "%s", mpv_error(mpv));
        free(mpv);
        return -1;
    }

    player->active = PLAYER_BACKEND_MPV;
    player->context = mpv;
    player->info.running = true;
    player->info.idle = true;
    player->info.paused = false;
    player->info.time_pos = 0.0;
    player->info.duration = 0.0;
    player->info.volume = 80.0;
    return 0;
}

static int start_ffmpeg(Player *player, char *error, size_t error_size)
{
    FfmpegPlayer *ffmpeg = ffmpeg_player_create(error, error_size);

    if (ffmpeg == NULL)
        return -1;

    player->active = PLAYER_BACKEND_FFMPEG;
    player->context = ffmpeg;
    player->info.running = true;
    player->info.idle = true;
    player->info.paused = false;
    player->info.time_pos = 0.0;
    player->info.duration = 0.0;
    player->info.volume = 80.0;
    return 0;
}

Player *player_create(PlayerBackend requested, char *error, size_t error_size)
{
    Player *player = calloc(1, sizeof(*player));
    char ffmpeg_error[256] = "";
    char mpv_error_text[256] = "";

    if (error != NULL && error_size > 0)
        error[0] = '\0';

    if (player == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "内存不足");
        return NULL;
    }

    if (requested == PLAYER_BACKEND_FFMPEG) {
        if (!ffmpeg_player_compiled()) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size,
                         "编译时未启用 FFmpeg 后端（缺少 libav* 或 PulseAudio 开发库）");
            free(player);
            return NULL;
        }
        if (start_ffmpeg(player, ffmpeg_error, sizeof(ffmpeg_error)) != 0) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size, "%s", ffmpeg_error);
            free(player);
            return NULL;
        }
        return player;
    }

    if (requested == PLAYER_BACKEND_MPV) {
        if (start_mpv(player, mpv_error_text, sizeof(mpv_error_text)) != 0) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size, "%s", mpv_error_text);
            free(player);
            return NULL;
        }
        return player;
    }

    if (ffmpeg_player_compiled()) {
        if (start_ffmpeg(player, ffmpeg_error, sizeof(ffmpeg_error)) == 0)
            return player;
    }

    if (start_mpv(player, mpv_error_text, sizeof(mpv_error_text)) == 0) {
        if (ffmpeg_error[0] != '\0') {
            snprintf(player->notice, sizeof(player->notice),
                     "FFmpeg 后端不可用，已切换到 mpv：%s", ffmpeg_error);
        }
        return player;
    }

    if (error != NULL && error_size > 0) {
        if (ffmpeg_error[0] != '\0') {
            snprintf(error, error_size, "FFmpeg：%s；mpv：%s",
                     ffmpeg_error, mpv_error_text);
        } else {
            snprintf(error, error_size, "%s", mpv_error_text);
        }
    }
    free(player);
    return NULL;
}

void player_destroy(Player *player)
{
    if (player == NULL)
        return;

    switch (player->active) {
    case PLAYER_BACKEND_MPV:
        mpv_shutdown(player->context);
        free(player->context);
        break;
    case PLAYER_BACKEND_FFMPEG:
        ffmpeg_player_destroy(player->context);
        break;
    case PLAYER_BACKEND_AUTO:
    default:
        break;
    }
    free(player);
}

int player_load(Player *player, const char *path)
{
    if (player == NULL)
        return -1;

    if (player->active == PLAYER_BACKEND_MPV) {
        Mpv *mpv = player->context;

        if (mpv_load(mpv, path) != 0) {
            copy_text(player->error, sizeof(player->error), mpv_error(mpv));
            return -1;
        }
        player->error[0] = '\0';
        player->info.idle = false;
        player->info.paused = false;
        player->info.time_pos = 0.0;
        player->info.duration = 0.0;
        return 0;
    }

    if (ffmpeg_player_load(player->context, path) != 0) {
        copy_text(player->error, sizeof(player->error),
                  ffmpeg_player_error(player->context));
        return -1;
    }
    player->error[0] = '\0';
    player->info.idle = false;
    player->info.paused = false;
    player->info.time_pos = 0.0;
    player->info.duration = 0.0;
    return 0;
}

int player_set_pause(Player *player, bool pause)
{
    int result;

    if (player == NULL)
        return -1;

    if (player->active == PLAYER_BACKEND_MPV) {
        Mpv *mpv = player->context;

        result = mpv_set_pause(mpv, pause);
        if (result != 0)
            copy_text(player->error, sizeof(player->error), mpv_error(mpv));
    } else {
        FfmpegPlayer *ffmpeg = player->context;

        result = ffmpeg_player_set_pause(ffmpeg, pause);
        if (result != 0)
            copy_text(player->error, sizeof(player->error),
                      ffmpeg_player_error(ffmpeg));
    }

    if (result == 0) {
        player->error[0] = '\0';
        player->info.paused = pause;
    }
    return result;
}

int player_seek_relative(Player *player, double seconds)
{
    int result;

    if (player == NULL)
        return -1;

    if (player->active == PLAYER_BACKEND_MPV) {
        Mpv *mpv = player->context;

        result = mpv_seek_relative(mpv, seconds);
        if (result != 0)
            copy_text(player->error, sizeof(player->error), mpv_error(mpv));
    } else {
        FfmpegPlayer *ffmpeg = player->context;

        result = ffmpeg_player_seek_relative(ffmpeg, seconds);
        if (result != 0)
            copy_text(player->error, sizeof(player->error),
                      ffmpeg_player_error(ffmpeg));
    }

    if (result == 0) {
        double position = player->info.time_pos + seconds;

        if (position < 0.0)
            position = 0.0;
        if (player->info.duration > 0.0 &&
            position > player->info.duration)
            position = player->info.duration;
        player->info.time_pos = position;
        player->error[0] = '\0';
    }
    return result;
}

int player_set_volume(Player *player, double volume)
{
    int result;

    if (player == NULL)
        return -1;

    if (player->active == PLAYER_BACKEND_MPV) {
        Mpv *mpv = player->context;

        result = mpv_set_volume(mpv, volume);
        if (result != 0)
            copy_text(player->error, sizeof(player->error), mpv_error(mpv));
    } else {
        FfmpegPlayer *ffmpeg = player->context;

        result = ffmpeg_player_set_volume(ffmpeg, volume);
        if (result != 0)
            copy_text(player->error, sizeof(player->error),
                      ffmpeg_player_error(ffmpeg));
    }

    if (result == 0) {
        player->info.volume = volume;
        player->error[0] = '\0';
    }
    return result;
}

int player_stop(Player *player)
{
    int result;

    if (player == NULL)
        return -1;

    if (player->active == PLAYER_BACKEND_MPV) {
        Mpv *mpv = player->context;

        result = mpv_stop(mpv);
        if (result != 0)
            copy_text(player->error, sizeof(player->error), mpv_error(mpv));
    } else {
        FfmpegPlayer *ffmpeg = player->context;

        result = ffmpeg_player_stop(ffmpeg);
        if (result != 0)
            copy_text(player->error, sizeof(player->error),
                      ffmpeg_player_error(ffmpeg));
    }

    if (result == 0) {
        player->info.idle = true;
        player->info.paused = false;
        player->info.time_pos = 0.0;
        player->info.duration = 0.0;
        player->error[0] = '\0';
    }
    return result;
}

int player_poll(Player *player)
{
    if (player == NULL)
        return -1;

    if (player->active == PLAYER_BACKEND_MPV) {
        Mpv *mpv = player->context;

        if (mpv_poll(mpv) != 0) {
            copy_text(player->error, sizeof(player->error), mpv_error(mpv));
            player->info.running = false;
            player->info.idle = true;
            return -1;
        }

        player->info.running = mpv->running;
        player->info.idle = mpv->idle;
        player->info.paused = mpv->paused;
        player->info.time_pos = mpv->time_pos;
        player->info.duration = mpv->duration;
        player->info.volume = mpv->volume;
        player->error[0] = '\0';
        return 0;
    }

    {
        FfmpegPlayer *ffmpeg = player->context;
        FfmpegPlayerStatus status;
        int result = ffmpeg_player_poll(ffmpeg, &status);

        player->info.running = status.running;
        player->info.idle = status.idle;
        player->info.paused = status.paused;
        player->info.time_pos = status.time_pos;
        player->info.duration = status.duration;
        player->info.volume = status.volume;
        if (status.error[0] != '\0')
            copy_text(player->error, sizeof(player->error), status.error);
        else
            player->error[0] = '\0';
        return result;
    }
}

bool player_is_running(Player *player)
{
    if (player == NULL)
        return false;

    if (player->active == PLAYER_BACKEND_MPV)
        return mpv_is_running(player->context);
    return ffmpeg_player_is_running(player->context);
}

PlayerBackend player_active_backend(const Player *player)
{
    if (player == NULL)
        return PLAYER_BACKEND_AUTO;
    return player->active;
}

const PlayerInfo *player_info(const Player *player)
{
    if (player == NULL)
        return NULL;
    return &player->info;
}

const char *player_error(const Player *player)
{
    if (player == NULL)
        return "播放器未初始化";
    return player->error;
}

const char *player_notice(const Player *player)
{
    if (player == NULL)
        return "";
    return player->notice;
}
