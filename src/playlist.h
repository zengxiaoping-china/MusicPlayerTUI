#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <stddef.h>

typedef struct {
    char *path;
    char *name;
} Track;

typedef struct {
    Track *items;
    size_t count;
    size_t capacity;
} Playlist;

int playlist_scan(Playlist *playlist, char *const *paths, size_t path_count,
                  char *error, size_t error_size);
int playlist_add(Playlist *playlist, char *const *paths, size_t path_count,
                 char *error, size_t error_size);
void playlist_free(Playlist *playlist);

#endif
