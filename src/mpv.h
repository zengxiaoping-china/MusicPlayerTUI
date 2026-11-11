#ifndef MPV_H
#define MPV_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    pid_t pid;
    int fd;
    int next_request_id;
    bool running;
    bool idle;
    bool paused;
    double time_pos;
    double duration;
    double volume;
    char tmpdir[128];
    char sockpath[160];
    char logpath[160];
    char error[256];
    char read_buffer[8192];
    size_t read_length;
} Mpv;

int mpv_start(Mpv *mpv);
void mpv_shutdown(Mpv *mpv);
bool mpv_is_running(Mpv *mpv);
int mpv_load(Mpv *mpv, const char *path);
int mpv_set_pause(Mpv *mpv, bool pause);
int mpv_seek_relative(Mpv *mpv, double seconds);
int mpv_set_volume(Mpv *mpv, double volume);
int mpv_stop(Mpv *mpv);
int mpv_poll(Mpv *mpv);
const char *mpv_error(const Mpv *mpv);

#endif
