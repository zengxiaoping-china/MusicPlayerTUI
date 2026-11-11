#ifndef TUI_H
#define TUI_H

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

enum {
    TUI_KEY_NONE = 0,
    TUI_KEY_EOF = 1000,
    TUI_KEY_UP,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    TUI_KEY_HOME,
    TUI_KEY_END,
    TUI_KEY_PGUP,
    TUI_KEY_PGDN,
    TUI_KEY_ENTER,
    TUI_KEY_ESC,
    TUI_KEY_BACKSPACE,
    TUI_KEY_CTRL_C,
    TUI_KEY_CTRL_D
};

#define TUI_RESET   "\033[0m"
#define TUI_BOLD    "\033[1m"
#define TUI_DIM     "\033[2m"
#define TUI_ITALIC  "\033[3m"
#define TUI_REVERSE "\033[7m"
#define TUI_BLACK   "\033[30m"
#define TUI_RED     "\033[31m"
#define TUI_GREEN   "\033[32m"
#define TUI_YELLOW  "\033[33m"
#define TUI_BLUE    "\033[34m"
#define TUI_MAGENTA "\033[35m"
#define TUI_CYAN    "\033[36m"
#define TUI_WHITE   "\033[37m"

typedef struct {
    int rows;
    int cols;
    bool active;
    bool size_changed;
    struct termios saved;
    bool saved_valid;
    unsigned char input[64];
    size_t input_len;
} Tui;

bool tui_init(Tui *tui);
void tui_shutdown(Tui *tui);
void tui_update_size(Tui *tui);
int tui_read_key(Tui *tui, int timeout_ms);
void tui_begin_frame(Tui *tui, bool clear_screen);
void tui_flush(void);
void tui_move(Tui *tui, int row, int col);
void tui_draw_text(Tui *tui, int row, int col, int width, const char *text,
                   const char *style, bool fill_style);
void tui_draw_hline(Tui *tui, int row, int width, char ch, const char *style);
int tui_text_width(const char *text);

#endif
