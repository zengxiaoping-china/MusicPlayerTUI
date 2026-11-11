#include "playlist.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_SCAN_DEPTH 32

static const char *const audio_extensions[] = {
    "aac", "aif", "aiff", "alac", "ape", "flac", "m4a", "m4b",
    "mid", "midi", "mka", "mp2", "mp3", "mpc", "oga", "ogg",
    "opus", "wav", "wma", "wv", "webm", "caf", "amr", "au"
};

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    if (error == NULL || error_size == 0 || error[0] != '\0')
        return;

    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static int has_audio_extension(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    size_t i;

    if (dot == NULL || (slash != NULL && dot < slash))
        return 0;
    dot++;

    for (i = 0; i < sizeof(audio_extensions) / sizeof(audio_extensions[0]); i++) {
        if (strcasecmp(dot, audio_extensions[i]) == 0)
            return 1;
    }
    return 0;
}

static char *join_path(const char *directory, const char *name)
{
    size_t dir_len = strlen(directory);
    size_t name_len = strlen(name);
    int needs_slash = dir_len > 0 && directory[dir_len - 1] != '/';
    char *path;

    path = malloc(dir_len + (size_t)needs_slash + name_len + 1);
    if (path == NULL)
        return NULL;

    memcpy(path, directory, dir_len);
    if (needs_slash)
        path[dir_len++] = '/';
    memcpy(path + dir_len, name, name_len + 1);
    return path;
}

static char *make_display_name(const char *path)
{
    const char *base = strrchr(path, '/');
    char *name;
    size_t i;

    base = base == NULL ? path : base + 1;
    name = strdup(base);
    if (name == NULL)
        return NULL;

    for (i = 0; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f)
            name[i] = '?';
    }
    return name;
}

static int add_track(Playlist *playlist, const char *path)
{
    Track *track;

    if (playlist->count == playlist->capacity) {
        size_t new_capacity = playlist->capacity == 0 ? 64 : playlist->capacity * 2;
        Track *new_items = realloc(playlist->items,
                                   new_capacity * sizeof(*new_items));
        if (new_items == NULL)
            return -1;
        playlist->items = new_items;
        playlist->capacity = new_capacity;
    }

    track = &playlist->items[playlist->count];
    track->path = strdup(path);
    track->name = make_display_name(path);
    if (track->path == NULL || track->name == NULL) {
        free(track->path);
        free(track->name);
        track->path = NULL;
        track->name = NULL;
        return -1;
    }

    playlist->count++;
    return 0;
}

static int scan_directory(Playlist *playlist, const char *directory, int depth,
                          char *error, size_t error_size)
{
    DIR *dir;
    struct dirent *entry;

    if (depth > MAX_SCAN_DEPTH)
        return 0;

    dir = opendir(directory);
    if (dir == NULL) {
        set_error(error, error_size, "无法打开 '%s': %s",
                  directory, strerror(errno));
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        char *path;
        struct stat info;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        path = join_path(directory, entry->d_name);
        if (path == NULL) {
            closedir(dir);
            set_error(error, error_size, "内存不足");
            return -1;
        }

        if (stat(path, &info) == 0) {
            if (S_ISDIR(info.st_mode)) {
                if (entry->d_name[0] != '.') {
                    if (scan_directory(playlist, path, depth + 1,
                                       error, error_size) != 0) {
                        free(path);
                        closedir(dir);
                        return -1;
                    }
                }
            } else if (S_ISREG(info.st_mode) && has_audio_extension(path)) {
                if (add_track(playlist, path) != 0) {
                    free(path);
                    closedir(dir);
                    set_error(error, error_size, "内存不足");
                    return -1;
                }
            }
        }
        free(path);
    }

    closedir(dir);
    return 0;
}

static int compare_tracks(const void *left, const void *right)
{
    const Track *a = left;
    const Track *b = right;
    int result = strcasecmp(a->path, b->path);

    if (result != 0)
        return result;
    return strcmp(a->path, b->path);
}

static void remove_duplicates(Playlist *playlist)
{
    size_t read_index;
    size_t write_index = 0;
    size_t original_count = playlist->count;

    if (playlist->count < 2)
        return;

    for (read_index = 0; read_index < original_count; read_index++) {
        Track *track = &playlist->items[read_index];

        if (write_index > 0 &&
            strcmp(playlist->items[write_index - 1].path, track->path) == 0) {
            free(track->path);
            free(track->name);
            track->path = NULL;
            track->name = NULL;
            continue;
        }

        if (write_index != read_index) {
            playlist->items[write_index] = *track;
            track->path = NULL;
            track->name = NULL;
        }
        write_index++;
    }

    for (read_index = write_index; read_index < original_count; read_index++) {
        free(playlist->items[read_index].path);
        free(playlist->items[read_index].name);
        playlist->items[read_index].path = NULL;
        playlist->items[read_index].name = NULL;
    }
    playlist->count = write_index;
}

static int scan_paths(Playlist *playlist, char *const *paths, size_t path_count,
                     char *error, size_t error_size)
{
    size_t i;

    for (i = 0; i < path_count; i++) {
        char *real_path = realpath(paths[i], NULL);
        struct stat info;

        if (real_path == NULL) {
            set_error(error, error_size, "无法访问 '%s': %s",
                      paths[i], strerror(errno));
            continue;
        }

        if (stat(real_path, &info) != 0) {
            set_error(error, error_size, "无法访问 '%s': %s",
                      real_path, strerror(errno));
        } else if (S_ISDIR(info.st_mode)) {
            if (scan_directory(playlist, real_path, 0, error, error_size) != 0) {
                free(real_path);
                return -1;
            }
        } else if (S_ISREG(info.st_mode) && has_audio_extension(real_path)) {
            if (add_track(playlist, real_path) != 0) {
                free(real_path);
                set_error(error, error_size, "内存不足");
                return -1;
            }
        } else {
            set_error(error, error_size, "'%s' 不是支持的音频文件",
                      paths[i]);
        }
        free(real_path);
    }
    return 0;
}

static void sort_and_deduplicate(Playlist *playlist)
{
    if (playlist->count > 1)
        qsort(playlist->items, playlist->count, sizeof(*playlist->items),
              compare_tracks);
    remove_duplicates(playlist);
}

int playlist_scan(Playlist *playlist, char *const *paths, size_t path_count,
                  char *error, size_t error_size)
{
    memset(playlist, 0, sizeof(*playlist));
    if (error != NULL && error_size > 0)
        error[0] = '\0';

    if (scan_paths(playlist, paths, path_count, error, error_size) != 0)
        return -1;

    sort_and_deduplicate(playlist);

    if (playlist->count == 0 && error != NULL && error_size > 0 && error[0] == '\0')
        snprintf(error, error_size, "未找到支持的音频文件");

    return 0;
}

int playlist_add(Playlist *playlist, char *const *paths, size_t path_count,
                 char *error, size_t error_size)
{
    size_t old_count = playlist->count;

    if (error != NULL && error_size > 0)
        error[0] = '\0';

    if (scan_paths(playlist, paths, path_count, error, error_size) != 0)
        return -1;

    sort_and_deduplicate(playlist);

    if (playlist->count == old_count && error != NULL && error_size > 0 &&
        error[0] == '\0')
        snprintf(error, error_size, "未找到支持的音频文件");

    return 0;
}

void playlist_free(Playlist *playlist)
{
    size_t i;

    for (i = 0; i < playlist->count; i++) {
        free(playlist->items[i].path);
        free(playlist->items[i].name);
    }
    free(playlist->items);
    memset(playlist, 0, sizeof(*playlist));
}
