#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PLAYER_BACKEND_AUTO = 0,
    PLAYER_BACKEND_MPV,
    PLAYER_BACKEND_FFMPEG
} PlayerBackend;

typedef struct {
    bool running;
    bool idle;
    bool paused;
    double time_pos;
    double duration;
    double volume;
} PlayerInfo;

typedef struct Player Player;

bool player_backend_from_string(const char *name, PlayerBackend *backend);
const char *player_backend_name(PlayerBackend backend);
bool player_backend_is_compiled(PlayerBackend backend);

Player *player_create(PlayerBackend requested, char *error, size_t error_size);
void player_destroy(Player *player);
int player_load(Player *player, const char *path);
int player_set_pause(Player *player, bool pause);
int player_seek_relative(Player *player, double seconds);
int player_set_volume(Player *player, double volume);
int player_stop(Player *player);
int player_poll(Player *player);
bool player_is_running(Player *player);

PlayerBackend player_active_backend(const Player *player);
const PlayerInfo *player_info(const Player *player);
const char *player_error(const Player *player);
const char *player_notice(const Player *player);

#endif
