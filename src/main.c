#include "player.h"
#include "playlist.h"
#include "state.h"
#include "tui.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    REPEAT_OFF,
    REPEAT_ALL,
    REPEAT_ONE
} RepeatMode;

typedef struct {
    Playlist playlist;
    PersistedState persisted;
    Player *player;
    PlayerBackend backend;
    size_t selected;
    size_t scroll;
    long current;
    bool track_loaded;
    bool saw_active;
    bool player_ready;
    bool muted;
    double load_time;
    double volume;
    double next_poll;
    double message_until;
    RepeatMode repeat_mode;
    bool shuffle;
    bool quit;
    bool adding;
    char message[256];
    char input_path[1024];
    size_t input_length;
    char input_hint[256];
} App;

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t resize_requested = 0;

static void on_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void on_resize_signal(int signal_number)
{
    (void)signal_number;
    resize_requested = 1;
}

static double now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void set_message(App *app, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(app->message, sizeof(app->message), format, args);
    va_end(args);
    app->message_until = now_seconds() + 5.0;
}

static const char *repeat_name(RepeatMode mode)
{
    switch (mode) {
    case REPEAT_ALL:
        return "列表";
    case REPEAT_ONE:
        return "单曲";
    case REPEAT_OFF:
    default:
        return "关闭";
    }
}

static void format_time(double seconds, char *output, size_t output_size)
{
    long total;

    if (seconds < 0.0 || seconds != seconds || seconds > 359999.0) {
        snprintf(output, output_size, "--:--");
        return;
    }

    total = (long)seconds;
    if (total >= 3600) {
        snprintf(output, output_size, "%ld:%02ld:%02ld",
                 total / 3600, (total / 60) % 60, total % 60);
    } else {
        snprintf(output, output_size, "%02ld:%02ld",
                 total / 60, total % 60);
    }
}

static size_t random_other_track(const App *app, size_t current)
{
    size_t count = app->playlist.count;
    size_t index;

    if (count <= 1)
        return 0;

    index = (size_t)(rand() % (int)(count - 1));
    if (index >= current)
        index++;
    return index;
}

static bool ensure_player(App *app)
{
    char error[256];

    if (app->player_ready) {
        if (app->player != NULL && player_is_running(app->player))
            return true;
        player_destroy(app->player);
        app->player = NULL;
        app->player_ready = false;
    }

    app->player = player_create(app->backend, error, sizeof(error));
    if (app->player == NULL) {
        set_message(app, "无法启动播放器：%s", error);
        app->player_ready = false;
        return false;
    }

    app->player_ready = true;
    app->muted = false;
    app->next_poll = 0.0;
    if (player_notice(app->player)[0] != '\0')
        set_message(app, "%s", player_notice(app->player));
    if (player_set_volume(app->player, app->volume) != 0)
        set_message(app, "无法设置音量：%s", player_error(app->player));
    return true;
}

static bool app_play(App *app, size_t index)
{
    const Track *track;

    if (index >= app->playlist.count)
        return false;
    if (!ensure_player(app))
        return false;

    track = &app->playlist.items[index];
    if (player_load(app->player, track->path) != 0) {
        set_message(app, "无法播放「%s」：%s", track->name,
                    player_error(app->player));
        return false;
    }

    app->current = (long)index;
    app->selected = index;
    app->track_loaded = true;
    app->saw_active = false;
    app->load_time = now_seconds();
    app->next_poll = 0.0;
    return true;
}

static void app_advance_auto(App *app)
{
    size_t count = app->playlist.count;
    size_t next;

    if (count == 0)
        return;

    if (app->repeat_mode == REPEAT_ONE && app->current >= 0) {
        (void)app_play(app, (size_t)app->current);
        return;
    }

    if (app->shuffle) {
        next = random_other_track(app, app->current < 0 ? 0 : (size_t)app->current);
        (void)app_play(app, next);
        return;
    }

    if (app->current < 0) {
        (void)app_play(app, 0);
        return;
    }

    if ((size_t)(app->current + 1) < count) {
        (void)app_play(app, (size_t)app->current + 1);
        return;
    }

    if (app->repeat_mode == REPEAT_ALL) {
        (void)app_play(app, 0);
        return;
    }

    app->track_loaded = false;
    set_message(app, "已到播放列表末尾");
}

static void app_next(App *app, bool automatic)
{
    size_t count = app->playlist.count;
    size_t next;

    if (count == 0)
        return;

    if (automatic) {
        app_advance_auto(app);
        return;
    }

    if (app->current < 0) {
        (void)app_play(app, app->selected);
        return;
    }

    if (app->shuffle && count > 1)
        next = random_other_track(app, (size_t)app->current);
    else
        next = ((size_t)app->current + 1) % count;
    (void)app_play(app, next);
}

static void app_previous(App *app)
{
    size_t count = app->playlist.count;

    if (count == 0)
        return;

    if (app->current < 0) {
        (void)app_play(app, app->selected);
        return;
    }

    (void)app_play(app, ((size_t)app->current + count - 1) % count);
}

static void app_toggle_pause(App *app)
{
    const PlayerInfo *info;
    bool pause;

    if (!ensure_player(app))
        return;

    info = player_info(app->player);
    if (!app->track_loaded || info == NULL || info->idle) {
        if (app->current >= 0)
            (void)app_play(app, (size_t)app->current);
        else if (app->playlist.count > 0)
            (void)app_play(app, app->selected);
        return;
    }

    pause = !info->paused;
    if (player_set_pause(app->player, pause) != 0)
        set_message(app, "无法切换暂停状态：%s",
                    player_error(app->player));
}

static void app_seek(App *app, double seconds)
{
    const PlayerInfo *info;

    if (!app->player_ready || !app->track_loaded) {
        set_message(app, "当前没有正在播放的曲目");
        return;
    }

    info = player_info(app->player);
    if (info == NULL || info->idle) {
        set_message(app, "当前没有正在播放的曲目");
        return;
    }

    if (player_seek_relative(app->player, seconds) != 0)
        set_message(app, "跳转失败：%s", player_error(app->player));
}

static void app_change_volume(App *app, double delta)
{
    double volume;

    if (!ensure_player(app))
        return;

    volume = app->volume + delta;
    if (volume < 0.0)
        volume = 0.0;
    if (volume > 130.0)
        volume = 130.0;

    if (player_set_volume(app->player, volume) != 0) {
        set_message(app, "无法调节音量：%s", player_error(app->player));
        return;
    }
    app->volume = volume;
    app->muted = false;
}

static void app_toggle_mute(App *app)
{
    double volume;

    if (!ensure_player(app))
        return;

    volume = app->muted ? app->volume : 0.0;
    if (player_set_volume(app->player, volume) != 0) {
        set_message(app, "无法调节音量：%s", player_error(app->player));
        return;
    }
    app->muted = !app->muted;
}

static void app_update_playback(App *app)
{
    const PlayerInfo *info;
    double now;

    if (!app->player_ready || app->player == NULL)
        return;

    now = now_seconds();
    if (now < app->next_poll)
        return;
    app->next_poll = now + 0.15;

    if (player_poll(app->player) != 0) {
        char error[256];

        snprintf(error, sizeof(error), "%s", player_error(app->player));
        player_destroy(app->player);
        app->player = NULL;
        app->player_ready = false;
        app->track_loaded = false;
        set_message(app, "播放器已退出：%s", error);
        return;
    }

    if (!app->track_loaded)
        return;

    info = player_info(app->player);
    if (info == NULL)
        return;

    if (!info->idle) {
        app->saw_active = true;
    } else if (app->saw_active || now - app->load_time > 1.0) {
        app->track_loaded = false;
        app->saw_active = false;
        app_advance_auto(app);
    }
}

static void move_selection(App *app, int direction)
{
    size_t count = app->playlist.count;

    if (count == 0)
        return;

    if (direction < 0) {
        if (app->selected == 0)
            app->selected = count - 1;
        else
            app->selected--;
    } else {
        if (app->selected + 1 >= count)
            app->selected = 0;
        else
            app->selected++;
    }
}

static void move_selection_page(App *app, int direction, int page_size)
{
    long selected;
    long count = (long)app->playlist.count;

    if (count == 0)
        return;

    selected = (long)app->selected + (long)direction * page_size;
    if (selected < 0)
        selected = 0;
    if (selected >= count)
        selected = count - 1;
    app->selected = (size_t)selected;
}

static int playlist_page_height(const Tui *tui)
{
    int height = tui->rows - 10;

    return height > 0 ? height : 1;
}

static void set_input_hint(App *app, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(app->input_hint, sizeof(app->input_hint), format, args);
    va_end(args);
}

static void begin_add_input(App *app)
{
    app->adding = true;
    app->input_length = 0;
    app->input_path[0] = '\0';
    app->input_hint[0] = '\0';
}

static void append_input(App *app, const char *text, size_t length)
{
    if (length == 0)
        return;
    if (app->input_length + length >= sizeof(app->input_path)) {
        set_input_hint(app, "路径过长，无法继续输入");
        return;
    }
    memcpy(app->input_path + app->input_length, text, length);
    app->input_length += length;
    app->input_path[app->input_length] = '\0';
    app->input_hint[0] = '\0';
}

static void backspace_input(App *app)
{
    size_t position;

    if (app->input_length == 0)
        return;

    position = app->input_length - 1;
    while (position > 0 &&
           ((unsigned char)app->input_path[position] & 0xc0) == 0x80)
        position--;

    app->input_length = position;
    app->input_path[position] = '\0';
    app->input_hint[0] = '\0';
}

static bool make_input_path(const App *app, char *output, size_t output_size)
{
    const char *start = app->input_path;
    const char *end = app->input_path + app->input_length;
    const char *home;
    size_t length;
    bool expand_home;

    while (start < end && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;

    length = (size_t)(end - start);
    if (length == 0)
        return false;

    expand_home = start[0] == '~' && (length == 1 || start[1] == '/');
    home = expand_home ? getenv("HOME") : NULL;
    if (home == NULL || home[0] == '\0')
        expand_home = false;

    if (expand_home) {
        size_t home_length = strlen(home);
        size_t tail_length = length - 1;

        if (home_length + tail_length + 1 > output_size)
            return false;
        memcpy(output, home, home_length);
        memcpy(output + home_length, start + 1, tail_length);
        output[home_length + tail_length] = '\0';
    } else {
        if (length + 1 > output_size)
            return false;
        memcpy(output, start, length);
        output[length] = '\0';
    }
    return true;
}

static long find_track_index(const Playlist *playlist, const char *path)
{
    size_t i;

    for (i = 0; i < playlist->count; i++) {
        if (strcmp(playlist->items[i].path, path) == 0)
            return (long)i;
    }
    return -1;
}

static bool persist_app(App *app, char *error, size_t error_size)
{
    const char *selected_path = NULL;

    if (error != NULL && error_size > 0)
        error[0] = '\0';

    app->persisted.volume = app->volume;
    app->persisted.repeat_mode = (int)app->repeat_mode;
    app->persisted.shuffle = app->shuffle;

    if (app->playlist.count > 0 && app->selected < app->playlist.count)
        selected_path = app->playlist.items[app->selected].path;

    if (!state_set_selected(&app->persisted, selected_path)) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "内存不足");
        return false;
    }
    return state_save(&app->persisted, error, error_size);
}

static void add_input_path(App *app)
{
    char path[1200];
    char error[512];
    char state_error[512];
    char *paths[1];
    char *saved_current = NULL;
    char *saved_selected = NULL;
    size_t old_count = app->playlist.count;
    size_t added;

    if (!make_input_path(app, path, sizeof(path))) {
        set_input_hint(app, "路径不能为空或路径过长");
        return;
    }

    if (app->current >= 0 && (size_t)app->current < old_count)
        saved_current = strdup(app->playlist.items[app->current].path);
    if (app->selected < old_count)
        saved_selected = strdup(app->playlist.items[app->selected].path);

    paths[0] = path;
    if (playlist_add(&app->playlist, paths, 1, error, sizeof(error)) != 0) {
        free(saved_current);
        free(saved_selected);
        app->adding = false;
        set_message(app, "添加失败：%s", error);
        return;
    }

    added = app->playlist.count - old_count;
    if (saved_current != NULL) {
        long index = find_track_index(&app->playlist, saved_current);

        if (index >= 0)
            app->current = index;
        free(saved_current);
    }
    if (saved_selected != NULL) {
        long index = find_track_index(&app->playlist, saved_selected);

        if (index >= 0)
            app->selected = index;
        free(saved_selected);
    }

    app->adding = false;
    if (added > 0) {
        bool saved;

        if (old_count == 0)
            app->selected = 0;
        state_error[0] = '\0';
        if (!state_add_source(&app->persisted, path)) {
            snprintf(state_error, sizeof(state_error), "内存不足");
            saved = false;
        } else {
            saved = persist_app(app, state_error, sizeof(state_error));
        }

        if (saved) {
            set_message(app, "已添加 %zu 首曲目", added);
        } else {
            set_message(app, "已添加 %zu 首曲目，但无法保存状态：%s",
                        added, state_error[0] != '\0' ? state_error : "未知错误");
        }
    } else {
        set_message(app, "未添加曲目（曲目可能已存在，或路径中没有音频文件）");
    }
}

static void handle_key(App *app, Tui *tui, int key)
{
    if (app->adding) {
        switch (key) {
        case TUI_KEY_ENTER:
            add_input_path(app);
            break;
        case TUI_KEY_ESC:
            app->adding = false;
            set_message(app, "已取消添加");
            break;
        case TUI_KEY_CTRL_C:
            app->adding = false;
            set_message(app, "已取消添加");
            break;
        case TUI_KEY_CTRL_D:
        case TUI_KEY_EOF:
            app->quit = true;
            break;
        case TUI_KEY_BACKSPACE:
            backspace_input(app);
            break;
        default:
            if (key >= 32 && key <= 255) {
                char byte = (char)key;

                append_input(app, &byte, 1);
            }
            break;
        }
        return;
    }

    switch (key) {
    case 'q':
    case 'Q':
    case TUI_KEY_CTRL_C:
    case TUI_KEY_CTRL_D:
    case TUI_KEY_EOF:
        app->quit = true;
        break;
    case TUI_KEY_UP:
    case 'k':
        move_selection(app, -1);
        break;
    case TUI_KEY_DOWN:
    case 'j':
        move_selection(app, 1);
        break;
    case TUI_KEY_PGUP:
        move_selection_page(app, -1, playlist_page_height(tui));
        break;
    case TUI_KEY_PGDN:
        move_selection_page(app, 1, playlist_page_height(tui));
        break;
    case TUI_KEY_HOME:
    case 'g':
        app->selected = 0;
        break;
    case TUI_KEY_END:
    case 'G':
        if (app->playlist.count > 0)
            app->selected = app->playlist.count - 1;
        break;
    case TUI_KEY_ENTER:
        if (app->playlist.count > 0)
            (void)app_play(app, app->selected);
        break;
    case ' ':
        app_toggle_pause(app);
        break;
    case 'n':
        app_next(app, false);
        break;
    case 'p':
        app_previous(app);
        break;
    case TUI_KEY_LEFT:
    case 'h':
        app_seek(app, -5.0);
        break;
    case TUI_KEY_RIGHT:
    case 'l':
        app_seek(app, 5.0);
        break;
    case '+':
    case '=':
        app_change_volume(app, 5.0);
        break;
    case '-':
    case '_':
        app_change_volume(app, -5.0);
        break;
    case 'm':
        app_toggle_mute(app);
        break;
    case 'r':
        if (app->repeat_mode == REPEAT_OFF)
            app->repeat_mode = REPEAT_ALL;
        else if (app->repeat_mode == REPEAT_ALL)
            app->repeat_mode = REPEAT_ONE;
        else
            app->repeat_mode = REPEAT_OFF;
        break;
    case 's':
        app->shuffle = !app->shuffle;
        break;
    case 'a':
        begin_add_input(app);
        break;
    default:
        break;
    }
}

static void draw_header(const App *app, Tui *tui)
{
    char header[160];
    const char *backend;

    backend = app->player != NULL
                  ? player_backend_name(player_active_backend(app->player))
                  : "未启动";
    snprintf(header, sizeof(header), " 本地音乐播放器 [%s]  |  %zu 首",
             backend, app->playlist.count);
    tui_draw_text(tui, 0, 0, tui->cols, header,
                  TUI_BOLD TUI_CYAN, false);
}

static void draw_now_playing(const App *app, Tui *tui, int row)
{
    const PlayerInfo *info = player_info(app->player);
    char title[512];
    const char *style = TUI_DIM;

    if (app->current >= 0 &&
        (size_t)app->current < app->playlist.count) {
        const Track *track = &app->playlist.items[(size_t)app->current];

        snprintf(title, sizeof(title), "[%ld/%zu] %s",
                 app->current + 1, app->playlist.count, track->name);
        style = TUI_BOLD;
        if (app->track_loaded && info != NULL && !info->idle) {
            if (info->paused)
                style = TUI_BOLD TUI_YELLOW;
            else
                style = TUI_BOLD TUI_GREEN;
        }
    } else {
        snprintf(title, sizeof(title),
                 "未选择曲目 - 按 Enter 播放，按 a 添加");
    }

    tui_draw_text(tui, row, 2, tui->cols - 2, title, style, false);
}

static void draw_progress(const App *app, Tui *tui, int row)
{
    const PlayerInfo *info = player_info(app->player);
    char position_text[32];
    char duration_text[32];
    char time_text[80];
    char bar[4096];
    double position = 0.0;
    double duration = 0.0;
    int time_width;
    int bar_width;
    int filled;
    int i;

    if (app->current < 0) {
        snprintf(position_text, sizeof(position_text), "--:--");
        snprintf(duration_text, sizeof(duration_text), "--:--");
    } else {
        if (app->player_ready && info != NULL) {
            position = info->time_pos;
            duration = info->duration;
        }
        format_time(position, position_text, sizeof(position_text));
        format_time(duration, duration_text, sizeof(duration_text));
    }

    snprintf(time_text, sizeof(time_text), "%s / %s",
             position_text, duration_text);
    time_width = (int)strlen(time_text);
    bar_width = tui->cols - time_width - 5;
    if (bar_width < 10)
        bar_width = 10;
    if (bar_width > (int)sizeof(bar) - 1)
        bar_width = (int)sizeof(bar) - 1;

    if (duration > 0.0) {
        double ratio = position / duration;

        if (ratio < 0.0)
            ratio = 0.0;
        if (ratio > 1.0)
            ratio = 1.0;
        filled = (int)(ratio * (double)bar_width);
    } else {
        filled = 0;
    }
    if (filled > bar_width)
        filled = bar_width;

    for (i = 0; i < bar_width; i++) {
        if (i < filled)
            bar[i] = '=';
        else if (i == filled && filled < bar_width)
            bar[i] = '>';
        else
            bar[i] = '-';
    }
    bar[bar_width] = '\0';

    tui_draw_text(tui, row, 2, bar_width, bar, TUI_CYAN, false);
    tui_draw_text(tui, row, 2 + bar_width + 1, tui->cols - (2 + bar_width + 1),
                  time_text, TUI_DIM, false);
}

static void draw_status(const App *app, Tui *tui, int row)
{
    const PlayerInfo *info = player_info(app->player);
    char volume_text[32];
    char status[256];
    const char *state;
    const char *style = TUI_DIM;

    if (!app->player_ready || app->player == NULL) {
        state = "不可用";
        style = TUI_BOLD TUI_RED;
    } else if (app->current < 0) {
        state = "就绪";
    } else if (app->track_loaded && info != NULL && !info->idle) {
        if (info->paused) {
            state = "已暂停";
            style = TUI_BOLD TUI_YELLOW;
        } else {
            state = "播放中";
            style = TUI_BOLD TUI_GREEN;
        }
    } else {
        state = "已停止";
    }

    if (app->muted)
        snprintf(volume_text, sizeof(volume_text), "已静音");
    else
        snprintf(volume_text, sizeof(volume_text), "%.0f%%", app->volume);

    snprintf(status, sizeof(status),
             "   %s  |  音量 %s  |  循环 %s  |  随机 %s",
             state, volume_text, repeat_name(app->repeat_mode),
             app->shuffle ? "开" : "关");
    tui_draw_text(tui, row, 0, tui->cols, status, style, false);
}

static void draw_playlist(App *app, Tui *tui)
{
    int top = 8;
    int bottom = tui->rows - 3;
    int height = bottom - top + 1;
    int row;
    size_t count = app->playlist.count;

    if (height <= 0)
        return;

    if (count == 0) {
        tui_draw_text(tui, top, 2, tui->cols - 2,
                      "播放列表为空，按 a 添加文件或文件夹", TUI_DIM, false);
        return;
    }

    if (app->selected < app->scroll)
        app->scroll = app->selected;
    if (app->selected >= app->scroll + (size_t)height)
        app->scroll = app->selected - (size_t)height + 1;
    if (count > 0 && app->scroll >= count)
        app->scroll = count - 1;
    if (height >= (int)count)
        app->scroll = 0;

    for (row = 0; row < height; row++) {
        size_t index = app->scroll + (size_t)row;
        char line[512];
        const char *style = "";
        bool fill_style = false;

        if (index >= count) {
            tui_draw_text(tui, top + row, 0, tui->cols, "", NULL, false);
            continue;
        }

        snprintf(line, sizeof(line), "%c %c %4zu  %s",
                 index == app->selected ? '>' : ' ',
                 (long)index == app->current ? '*' : ' ',
                 index + 1, app->playlist.items[index].name);

        if (index == app->selected) {
            style = TUI_BOLD TUI_REVERSE;
            fill_style = true;
        } else if ((long)index == app->current) {
            style = TUI_GREEN;
        }

        tui_draw_text(tui, top + row, 0, tui->cols, line, style, fill_style);
    }
}

static size_t utf8_sequence_length(unsigned char byte)
{
    if (byte < 0x80)
        return 1;
    if ((byte & 0xe0) == 0xc0)
        return 2;
    if ((byte & 0xf0) == 0xe0)
        return 3;
    if ((byte & 0xf8) == 0xf0)
        return 4;
    return 1;
}

static void make_tail(const char *text, int max_columns, char *output,
                      size_t output_size)
{
    size_t length = strlen(text);
    size_t position = 0;

    if (max_columns <= 0) {
        if (output_size > 0)
            output[0] = '\0';
        return;
    }

    if (tui_text_width(text) <= max_columns) {
        snprintf(output, output_size, "%s", text);
        return;
    }

    while (position < length) {
        size_t step = utf8_sequence_length((unsigned char)text[position]);

        if (step > length - position)
            step = length - position;
        if (position + step >= length)
            break;
        if (tui_text_width(text + position + step) <= max_columns) {
            position += step;
            break;
        }
        position += step;
    }

    snprintf(output, output_size, "%s", text + position);
}

static void draw_add_prompt(App *app, Tui *tui)
{
    static const char prefix[] = " 添加路径: ";
    char tail[1200];
    char line[1300];
    int prefix_width = tui_text_width(prefix);
    int path_columns = tui->cols - prefix_width - 2;

    make_tail(app->input_path, path_columns, tail, sizeof(tail));
    snprintf(line, sizeof(line), "%s%s_", prefix, tail);
    tui_draw_text(tui, tui->rows - 1, 0, tui->cols, line,
                  TUI_BOLD TUI_CYAN, false);
}

static void draw_add_hint(App *app, Tui *tui)
{
    const char *hint;

    if (app->input_hint[0] != '\0') {
        hint = app->input_hint;
        tui_draw_text(tui, tui->rows - 2, 0, tui->cols, hint,
                      TUI_BOLD TUI_RED, false);
    } else {
        hint = " 输入文件或文件夹路径，Enter 添加，Esc 取消；支持 ~/ 开头";
        tui_draw_text(tui, tui->rows - 2, 0, tui->cols, hint,
                      TUI_DIM, false);
    }
}

static void draw_footer(App *app, Tui *tui)
{
    const char *help;
    double now = now_seconds();

    if (app->message[0] != '\0' && now < app->message_until) {
        tui_draw_text(tui, tui->rows - 1, 0, tui->cols, app->message,
                      TUI_BOLD TUI_YELLOW, false);
        return;
    }

    if (tui->cols >= 110) {
        help = " j/k 移动  Enter 播放  空格 暂停  n/p 上/下一首  "
               "h/l 快退/快进  +/- 音量  m 静音  r 循环  s 随机  a 添加  q 退出";
    } else {
        help = " j/k 移动  Enter 播放  空格 暂停  n/p 切歌  "
               "h/l 快进  +/- 音量  a 添加  q 退出";
    }
    tui_draw_text(tui, tui->rows - 1, 0, tui->cols, help, TUI_DIM, false);
}

static void render(App *app, Tui *tui)
{
    bool clear_screen = tui->size_changed;

    tui->size_changed = false;
    tui_begin_frame(tui, clear_screen);

    if (tui->rows < 12 || tui->cols < 40) {
        char message[160];

        snprintf(message, sizeof(message),
                 " 终端过小：%dx%d（最小 40x12）",
                 tui->cols, tui->rows);
        tui_draw_text(tui, 0, 0, tui->cols, message, TUI_BOLD TUI_RED, false);
        tui_draw_text(tui, 1, 0, tui->cols,
                      " 请调整终端大小，或按 q 退出。", TUI_DIM, false);
        tui_flush();
        return;
    }

    draw_header(app, tui);
    tui_draw_hline(tui, 1, tui->cols, '-', TUI_DIM);
    tui_draw_text(tui, 2, 0, tui->cols, " 正在播放", TUI_DIM, false);
    draw_now_playing(app, tui, 3);
    draw_progress(app, tui, 4);
    draw_status(app, tui, 5);
    tui_draw_hline(tui, 6, tui->cols, '-', TUI_DIM);

    {
        char header[128];

        snprintf(header, sizeof(header), " 播放列表  %zu 首",
                 app->playlist.count);
        tui_draw_text(tui, 7, 0, tui->cols, header,
                      TUI_BOLD TUI_CYAN, false);
    }

    draw_playlist(app, tui);
    if (app->adding) {
        draw_add_hint(app, tui);
        draw_add_prompt(app, tui);
    } else {
        tui_draw_hline(tui, tui->rows - 2, tui->cols, '-', TUI_DIM);
        draw_footer(app, tui);
    }
    tui_flush();
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "用法：%s [选项]... [路径]...\n"
            "\n"
            "WSL/Linux 本地音乐播放器 TUI。路径可以是音频文件或目录（递归扫描）。\n"
            "不提供路径时恢复上次保存的播放列表；可在程序中按 a 添加文件或文件夹。\n"
            "\n"
            "选项：\n"
            "  -h, --help        显示帮助\n"
            "  -v, --version     显示版本\n"
            "  --backend=NAME    播放后端：mpv（默认）、auto、ffmpeg\n"
            "\n"
            "也可用环境变量 MUSICPLAYERTUI_BACKEND 指定后端。\n"
            "默认使用 mpv；auto 优先使用 FFmpeg（编译且 PulseAudio 可用时），否则回退 mpv。\n"
            "播放依赖 mpv，或 FFmpeg + PulseAudio。\n",
            program);
}

static void install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_resize_signal;
    sigaction(SIGWINCH, &action, NULL);

    action.sa_handler = on_stop_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char **argv)
{
    Playlist playlist;
    PersistedState persisted;
    App app;
    Tui tui;
    char error[512];
    char state_error[512];
    char **paths;
    PlayerBackend backend = PLAYER_BACKEND_MPV;
    const char *backend_env;
    int path_count = 0;
    int first_path = argc;
    int i;
    bool running = true;

    srand((unsigned int)(time(NULL) ^ (long)getpid()));

    backend_env = getenv("MUSICPLAYERTUI_BACKEND");
    if (backend_env != NULL && backend_env[0] != '\0' &&
        !player_backend_from_string(backend_env, &backend)) {
        fprintf(stderr,
                "%s：无效的 MUSICPLAYERTUI_BACKEND「%s」（可选 auto、mpv、ffmpeg）\n",
                argv[0], backend_env);
        return 2;
    }

    for (i = 1; i < argc; i++) {
        const char *backend_name = NULL;

        if (strcmp(argv[i], "--") == 0) {
            first_path = i + 1;
            break;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("musicplayertui 1.3.1\n");
            return 0;
        }
        if (strcmp(argv[i], "--backend") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s：--backend 需要参数\n", argv[0]);
                print_usage(stderr, argv[0]);
                return 2;
            }
            backend_name = argv[++i];
        } else if (strncmp(argv[i], "--backend=", 10) == 0) {
            backend_name = argv[i] + 10;
        }

        if (backend_name != NULL) {
            if (!player_backend_from_string(backend_name, &backend)) {
                fprintf(stderr,
                        "%s：无效的播放后端「%s」（可选 auto、mpv、ffmpeg）\n",
                        argv[0], backend_name);
                return 2;
            }
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "%s：未知选项「%s」\n", argv[0], argv[i]);
            print_usage(stderr, argv[0]);
            return 2;
        }
        first_path = i;
        break;
    }

    path_count = argc - first_path;
    paths = path_count > 0 ? &argv[first_path] : NULL;

    state_init(&persisted);
    state_error[0] = '\0';
    (void)state_load(&persisted, state_error, sizeof(state_error));

    if (path_count > 0) {
        if (playlist_scan(&playlist, paths, (size_t)path_count,
                          error, sizeof(error)) != 0) {
            fprintf(stderr, "%s：无法读取音乐库：%s\n", argv[0], error);
            playlist_free(&playlist);
            state_free(&persisted);
            return 1;
        }
        if (playlist.count > 0 &&
            !state_replace_sources(&persisted, paths, (size_t)path_count)) {
            fprintf(stderr, "%s：保存音乐库路径失败：内存不足\n", argv[0]);
            playlist_free(&playlist);
            state_free(&persisted);
            return 1;
        }
    } else if (persisted.source_count > 0) {
        if (playlist_scan(&playlist, persisted.sources,
                          persisted.source_count,
                          error, sizeof(error)) != 0) {
            fprintf(stderr, "%s：无法读取音乐库：%s\n", argv[0], error);
            playlist_free(&playlist);
            state_free(&persisted);
            return 1;
        }
    } else {
        memset(&playlist, 0, sizeof(playlist));
        error[0] = '\0';
    }

    memset(&app, 0, sizeof(app));
    app.playlist = playlist;
    app.persisted = persisted;
    app.backend = backend;
    app.current = -1;
    app.volume = persisted.volume;
    app.repeat_mode = (RepeatMode)persisted.repeat_mode;
    app.shuffle = persisted.shuffle;
    if (app.playlist.count > 0) {
        app.selected = 0;
        if (persisted.selected_path != NULL) {
            long index = find_track_index(&app.playlist,
                                          persisted.selected_path);

            if (index >= 0)
                app.selected = (size_t)index;
        }
    }

    if (!tui_init(&tui)) {
        playlist_free(&app.playlist);
        state_free(&app.persisted);
        return 1;
    }

    install_signal_handlers();
    {
        bool player_ok = ensure_player(&app);
        const char *startup_error = error[0] != '\0' ? error :
                                    (state_error[0] != '\0' ? state_error : NULL);

        if (player_ok && startup_error != NULL)
            set_message(&app, "%s", startup_error);
    }

    while (running) {
        int key;

        if (stop_requested || app.quit)
            break;

        if (resize_requested) {
            resize_requested = 0;
            tui_update_size(&tui);
        }

        app_update_playback(&app);
        render(&app, &tui);

        key = tui_read_key(&tui, 100);
        if (stop_requested)
            break;
        if (key == TUI_KEY_NONE)
            continue;

        handle_key(&app, &tui, key);
        if (app.quit)
            running = false;
    }

    tui_shutdown(&tui);
    player_destroy(app.player);
    {
        char save_error[512];

        if (!persist_app(&app, save_error, sizeof(save_error)))
            fprintf(stderr, "%s：无法保存状态：%s\n", argv[0], save_error);
    }
    playlist_free(&app.playlist);
    state_free(&app.persisted);
    return 0;
}
