#include "state.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_PATH_SIZE 4096
#define STATE_HEADER "musicplayertui-state 1"
#define STATE_DEFAULT_VOLUME 80.0

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    if (error == NULL || error_size == 0 || error[0] != '\0')
        return;

    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static bool get_state_directory(char *output, size_t output_size,
                                char *error, size_t error_size)
{
    const char *base = getenv("XDG_STATE_HOME");
    const char *home;
    int written;

    if (base != NULL && base[0] != '\0') {
        written = snprintf(output, output_size, "%s/musicplayertui", base);
    } else {
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            set_error(error, error_size, "无法确定状态文件位置（HOME 未设置）");
            return false;
        }
        written = snprintf(output, output_size,
                           "%s/.local/state/musicplayertui", home);
    }

    if (written < 0 || (size_t)written >= output_size) {
        set_error(error, error_size, "状态文件路径过长");
        return false;
    }
    return true;
}

static bool get_state_path(char *output, size_t output_size,
                           char *error, size_t error_size)
{
    char directory[STATE_PATH_SIZE];
    int written;

    if (!get_state_directory(directory, sizeof(directory), error, error_size))
        return false;

    written = snprintf(output, output_size, "%s/state", directory);
    if (written < 0 || (size_t)written >= output_size) {
        set_error(error, error_size, "状态文件路径过长");
        return false;
    }
    return true;
}

static bool make_directories(const char *path, char *error, size_t error_size)
{
    char *copy = strdup(path);
    char *position;
    bool ok = true;

    if (copy == NULL) {
        set_error(error, error_size, "内存不足");
        return false;
    }

    for (position = copy + 1; *position != '\0'; position++) {
        int saved_errno;

        if (*position != '/')
            continue;
        *position = '\0';
        if (mkdir(copy, 0700) != 0) {
            saved_errno = errno;
            if (saved_errno != EEXIST) {
                set_error(error, error_size, "无法创建状态目录 '%s': %s",
                          copy, strerror(saved_errno));
                ok = false;
                break;
            }
        }
        *position = '/';
    }

    if (ok) {
        int saved_errno;

        if (mkdir(copy, 0700) != 0) {
            saved_errno = errno;
            if (saved_errno != EEXIST) {
                set_error(error, error_size, "无法创建状态目录 '%s': %s",
                          copy, strerror(saved_errno));
                ok = false;
            }
        }
    }

    free(copy);
    return ok;
}

static char *canonical_path(const char *path)
{
    char *resolved = realpath(path, NULL);

    if (resolved != NULL)
        return resolved;
    return strdup(path);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char *decode_hex(const char *text)
{
    size_t length = strlen(text);
    size_t i;
    char *output;

    if (length % 2 != 0)
        return NULL;

    output = malloc(length / 2 + 1);
    if (output == NULL)
        return NULL;

    for (i = 0; i < length; i += 2) {
        int high = hex_value(text[i]);
        int low = hex_value(text[i + 1]);

        if (high < 0 || low < 0) {
            free(output);
            return NULL;
        }
        output[i / 2] = (char)((high << 4) | low);
    }
    output[length / 2] = '\0';
    return output;
}

static bool write_hex_line(FILE *file, const char *key, const char *value)
{
    const unsigned char *character = (const unsigned char *)value;

    if (fprintf(file, "%s=", key) < 0)
        return false;
    while (*character != '\0') {
        if (fprintf(file, "%02x", *character) < 0)
            return false;
        character++;
    }
    return fputc('\n', file) != EOF;
}

void state_init(PersistedState *state)
{
    memset(state, 0, sizeof(*state));
    state->volume = STATE_DEFAULT_VOLUME;
    state->repeat_mode = 0;
    state->shuffle = false;
}

void state_free(PersistedState *state)
{
    size_t i;

    for (i = 0; i < state->source_count; i++)
        free(state->sources[i]);
    free(state->sources);
    free(state->selected_path);
    memset(state, 0, sizeof(*state));
}

bool state_add_source(PersistedState *state, const char *path)
{
    char *copy;
    char **items;
    size_t i;

    if (path == NULL || path[0] == '\0')
        return true;

    copy = canonical_path(path);
    if (copy == NULL)
        return false;

    for (i = 0; i < state->source_count; i++) {
        if (strcmp(state->sources[i], copy) == 0) {
            free(copy);
            return true;
        }
    }

    items = realloc(state->sources,
                    (state->source_count + 1) * sizeof(*state->sources));
    if (items == NULL) {
        free(copy);
        return false;
    }

    state->sources = items;
    state->sources[state->source_count++] = copy;
    return true;
}

bool state_replace_sources(PersistedState *state, char *const *paths,
                           size_t path_count)
{
    size_t i;

    for (i = 0; i < state->source_count; i++)
        free(state->sources[i]);
    free(state->sources);
    state->sources = NULL;
    state->source_count = 0;

    for (i = 0; i < path_count; i++) {
        if (!state_add_source(state, paths[i]))
            return false;
    }
    return true;
}

bool state_set_selected(PersistedState *state, const char *path)
{
    char *copy = NULL;

    if (path != NULL && path[0] != '\0') {
        copy = strdup(path);
        if (copy == NULL)
            return false;
    }

    free(state->selected_path);
    state->selected_path = copy;
    return true;
}

bool state_load(PersistedState *state, char *error, size_t error_size)
{
    char path[STATE_PATH_SIZE];
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    bool header_seen = false;
    bool ok = true;

    if (error != NULL && error_size > 0)
        error[0] = '\0';

    if (!get_state_path(path, sizeof(path), error, error_size))
        return false;

    file = fopen(path, "r");
    if (file == NULL) {
        if (errno != ENOENT)
            set_error(error, error_size, "无法读取状态文件 '%s': %s",
                      path, strerror(errno));
        return false;
    }

    while ((length = getline(&line, &capacity, file)) >= 0) {
        while (length > 0 &&
               (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';

        if (!header_seen) {
            if (strcmp(line, STATE_HEADER) != 0) {
                set_error(error, error_size, "不支持的状态文件版本");
                ok = false;
                break;
            }
            header_seen = true;
            continue;
        }

        if (strncmp(line, "volume=", 7) == 0) {
            char *end;
            double volume = strtod(line + 7, &end);

            if (end != line + 7 && volume >= 0.0 && volume <= 130.0)
                state->volume = volume;
        } else if (strncmp(line, "repeat=", 7) == 0) {
            int repeat = atoi(line + 7);

            if (repeat >= 0 && repeat <= 2)
                state->repeat_mode = repeat;
        } else if (strncmp(line, "shuffle=", 8) == 0) {
            int shuffle = atoi(line + 8);

            if (shuffle == 0 || shuffle == 1)
                state->shuffle = shuffle != 0;
        } else if (strncmp(line, "selected=", 9) == 0) {
            char *selected = decode_hex(line + 9);

            if (selected != NULL) {
                free(state->selected_path);
                state->selected_path = selected[0] != '\0' ? selected : NULL;
                if (selected[0] == '\0')
                    free(selected);
            }
        } else if (strncmp(line, "source=", 7) == 0) {
            char *source = decode_hex(line + 7);

            if (source != NULL) {
                if (!state_add_source(state, source)) {
                    set_error(error, error_size, "内存不足");
                    free(source);
                    ok = false;
                    break;
                }
                free(source);
            }
        }
    }

    if (ok && ferror(file)) {
        set_error(error, error_size, "读取状态文件失败");
        ok = false;
    }
    if (ok && !header_seen) {
        set_error(error, error_size, "状态文件格式错误");
        ok = false;
    }

    free(line);
    fclose(file);

    if (!ok) {
        state_free(state);
        state_init(state);
    }
    return ok;
}

bool state_save(const PersistedState *state, char *error, size_t error_size)
{
    char directory[STATE_PATH_SIZE];
    char path[STATE_PATH_SIZE];
    char temporary[STATE_PATH_SIZE + 32];
    FILE *file;
    size_t i;
    int written;
    bool ok = true;
    int close_result;

    if (error != NULL && error_size > 0)
        error[0] = '\0';

    if (!get_state_directory(directory, sizeof(directory), error, error_size))
        return false;
    if (!get_state_path(path, sizeof(path), error, error_size))
        return false;
    if (!make_directories(directory, error, error_size))
        return false;

    written = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
                       path, (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(temporary)) {
        set_error(error, error_size, "临时状态文件路径过长");
        return false;
    }

    file = fopen(temporary, "w");
    if (file == NULL) {
        set_error(error, error_size, "无法创建状态文件 '%s': %s",
                  temporary, strerror(errno));
        return false;
    }

    (void)fchmod(fileno(file), 0600);

    if (fprintf(file, "%s\n", STATE_HEADER) < 0 ||
        fprintf(file, "volume=%.2f\n", state->volume) < 0 ||
        fprintf(file, "repeat=%d\n", state->repeat_mode) < 0 ||
        fprintf(file, "shuffle=%d\n", state->shuffle ? 1 : 0) < 0 ||
        !write_hex_line(file, "selected",
                        state->selected_path != NULL ? state->selected_path : ""))
        ok = false;

    for (i = 0; ok && i < state->source_count; i++) {
        if (!write_hex_line(file, "source", state->sources[i]))
            ok = false;
    }

    if (ok && fflush(file) != 0)
        ok = false;
    if (ok && fsync(fileno(file)) != 0)
        ok = false;

    close_result = fclose(file);
    if (ok && close_result != 0)
        ok = false;

    if (!ok) {
        set_error(error, error_size, "无法写入状态文件 '%s'", temporary);
        (void)unlink(temporary);
        return false;
    }

    if (rename(temporary, path) != 0) {
        set_error(error, error_size, "无法更新状态文件 '%s': %s",
                  path, strerror(errno));
        (void)unlink(temporary);
        return false;
    }
    return true;
}
