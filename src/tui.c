#include "tui.h"

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

static void set_fallback_size(Tui *tui)
{
    tui->rows = 24;
    tui->cols = 80;
}

static void read_size(Tui *tui, int *rows, int *cols)
{
    struct winsize ws;

    *rows = tui->rows;
    *cols = tui->cols;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = (int)ws.ws_row;
        *cols = (int)ws.ws_col;
    }
}

bool tui_init(Tui *tui)
{
    struct termios raw;

    memset(tui, 0, sizeof(*tui));
    setlocale(LC_ALL, "");

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "musicplayertui：标准输入和输出必须是终端\n");
        return false;
    }

    if (tcgetattr(STDIN_FILENO, &tui->saved) != 0)
        return false;
    tui->saved_valid = true;

    raw = tui->saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return false;

    set_fallback_size(tui);
    read_size(tui, &tui->rows, &tui->cols);
    tui->size_changed = true;
    tui->active = true;
    tui->input_len = 0;

    fputs("\033[?1049h\033[?25l\033[?7l", stdout);
    fflush(stdout);
    return true;
}

void tui_shutdown(Tui *tui)
{
    if (!tui->active)
        return;

    fputs(TUI_RESET "\033[?7h\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    if (tui->saved_valid)
        tcsetattr(STDIN_FILENO, TCSANOW, &tui->saved);
    tui->active = false;
}

void tui_update_size(Tui *tui)
{
    int rows, cols;

    read_size(tui, &rows, &cols);
    if (rows != tui->rows || cols != tui->cols) {
        tui->rows = rows;
        tui->cols = cols;
        tui->size_changed = true;
    }
}

void tui_begin_frame(Tui *tui, bool clear_screen)
{
    (void)tui;
    if (clear_screen)
        fputs("\033[2J", stdout);
    fputs("\033[H", stdout);
}

void tui_flush(void)
{
    fflush(stdout);
}

void tui_move(Tui *tui, int row, int col)
{
    if (row < 0 || row >= tui->rows || col < 0 || col >= tui->cols)
        return;
    printf("\033[%d;%dH", row + 1, col + 1);
}

static int read_more(Tui *tui, int timeout_ms)
{
    struct pollfd pfd;
    int rc;
    ssize_t n;

    if (tui->input_len >= sizeof(tui->input))
        return 0;

    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0)
        return 0;

    n = read(STDIN_FILENO, tui->input + tui->input_len,
             sizeof(tui->input) - tui->input_len);
    if (n > 0) {
        tui->input_len += (size_t)n;
        return 1;
    }
    if (n == 0)
        return -1;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return 0;
    return -1;
}

static void consume_input(Tui *tui, size_t count)
{
    if (count >= tui->input_len) {
        tui->input_len = 0;
        return;
    }
    memmove(tui->input, tui->input + count, tui->input_len - count);
    tui->input_len -= count;
}

static int parse_escape(Tui *tui)
{
    size_t final_pos = 0;
    int attempts;
    int key = TUI_KEY_NONE;

    for (attempts = 0; attempts < 3 && final_pos == 0; attempts++) {
        size_t i;

        for (i = 2; i < tui->input_len; i++) {
            if (tui->input[i] >= 0x40 && tui->input[i] <= 0x7e) {
                final_pos = i;
                break;
            }
        }
        if (final_pos != 0)
            break;
        if (read_more(tui, 15) <= 0)
            break;
    }

    if (final_pos == 0) {
        consume_input(tui, 1);
        return TUI_KEY_ESC;
    }

    switch (tui->input[final_pos]) {
    case 'A':
        key = TUI_KEY_UP;
        break;
    case 'B':
        key = TUI_KEY_DOWN;
        break;
    case 'C':
        key = TUI_KEY_RIGHT;
        break;
    case 'D':
        key = TUI_KEY_LEFT;
        break;
    case 'H':
        key = TUI_KEY_HOME;
        break;
    case 'F':
        key = TUI_KEY_END;
        break;
    case '~': {
        int number = 0;
        size_t i;

        for (i = 2; i < final_pos; i++) {
            unsigned char c = tui->input[i];
            if (c < '0' || c > '9')
                break;
            number = number * 10 + (c - '0');
        }
        switch (number) {
        case 1:
        case 7:
            key = TUI_KEY_HOME;
            break;
        case 4:
        case 8:
            key = TUI_KEY_END;
            break;
        case 5:
            key = TUI_KEY_PGUP;
            break;
        case 6:
            key = TUI_KEY_PGDN;
            break;
        default:
            key = TUI_KEY_NONE;
            break;
        }
        break;
    }
    default:
        key = TUI_KEY_NONE;
        break;
    }

    consume_input(tui, final_pos + 1);
    return key;
}

int tui_read_key(Tui *tui, int timeout_ms)
{
    int rc;
    unsigned char b;

    if (!tui->active)
        return TUI_KEY_EOF;

    if (tui->input_len == 0) {
        rc = read_more(tui, timeout_ms);
        if (rc < 0)
            return TUI_KEY_EOF;
        if (rc == 0)
            return TUI_KEY_NONE;
    }

    b = tui->input[0];
    if (b == 0x1b) {
        if (tui->input_len < 3)
            (void)read_more(tui, 15);
        return parse_escape(tui);
    }

    consume_input(tui, 1);
    switch (b) {
    case '\r':
    case '\n':
        return TUI_KEY_ENTER;
    case 3:
        return TUI_KEY_CTRL_C;
    case 4:
        return TUI_KEY_CTRL_D;
    case 8:
    case 127:
        return TUI_KEY_BACKSPACE;
    default:
        if (b >= 32)
            return (int)b;
        return TUI_KEY_NONE;
    }
}

static int decode_char(const char *text, size_t remaining, size_t *used)
{
    mbstate_t state;
    wchar_t wc;
    size_t n;
    int width;

    memset(&state, 0, sizeof(state));
    n = mbrtowc(&wc, text, remaining, &state);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *used = 1;
        return -1;
    }

    width = wcwidth(wc);
    if (width < 0)
        width = 1;
    *used = n;
    return width;
}

static int emit_clipped(const char *text, int max_cols)
{
    size_t len = strlen(text);
    size_t pos = 0;
    int used = 0;

    while (pos < len && used < max_cols) {
        unsigned char c = (unsigned char)text[pos];
        size_t n = 1;
        int width = 1;

        if (c < 0x20 || c == 0x7f) {
            fputc('?', stdout);
            pos++;
            used++;
            continue;
        }
        if (c >= 0x80) {
            width = decode_char(text + pos, len - pos, &n);
            if (width < 0) {
                fputc('?', stdout);
                pos++;
                used++;
                continue;
            }
            if (width > 0 && used + width > max_cols)
                break;
            fwrite(text + pos, 1, n, stdout);
            pos += n;
            used += width;
        } else {
            if (used + 1 > max_cols)
                break;
            fputc((int)c, stdout);
            pos++;
            used++;
        }
    }

    return used;
}

int tui_text_width(const char *text)
{
    size_t len = strlen(text);
    size_t pos = 0;
    int width_total = 0;

    while (pos < len) {
        unsigned char c = (unsigned char)text[pos];

        if (c < 0x20 || c == 0x7f) {
            width_total++;
            pos++;
        } else if (c >= 0x80) {
            size_t n = 1;
            int width = decode_char(text + pos, len - pos, &n);
            width_total += width < 0 ? 1 : width;
            pos += n;
        } else {
            width_total++;
            pos++;
        }
    }
    return width_total;
}

void tui_draw_text(Tui *tui, int row, int col, int width, const char *text,
                   const char *style, bool fill_style)
{
    int total, target, used;

    if (width <= 0 || row < 0 || row >= tui->rows ||
        col < 0 || col >= tui->cols)
        return;

    if (col + width > tui->cols)
        width = tui->cols - col;
    if (width <= 0)
        return;

    total = tui_text_width(text);
    target = total > width ? width - 1 : width;

    tui_move(tui, row, col);
    if (style != NULL)
        fputs(style, stdout);

    used = emit_clipped(text, target);
    if (total > width && used < width) {
        fputc('~', stdout);
        used++;
    }

    if (fill_style) {
        while (used < width) {
            fputc(' ', stdout);
            used++;
        }
        fputs(TUI_RESET, stdout);
    } else {
        fputs(TUI_RESET, stdout);
        while (used < width) {
            fputc(' ', stdout);
            used++;
        }
    }
}

void tui_draw_hline(Tui *tui, int row, int width, char ch, const char *style)
{
    int i;

    if (width <= 0 || row < 0 || row >= tui->rows)
        return;
    if (width > tui->cols)
        width = tui->cols;

    tui_move(tui, row, 0);
    if (style != NULL)
        fputs(style, stdout);
    for (i = 0; i < width; i++)
        fputc(ch, stdout);
    fputs(TUI_RESET, stdout);
}
