/*
 * localchat - ncurses chat client for the localchat daemon.
 *
 * Single-threaded; uses poll() over stdin and the daemon socket and renders
 * via ncurses/ncursesw for UTF-8 / wide-character support. Messages are
 * length-prefixed (4-byte big-endian) on the wire.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE_EXTENDED 1

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define VERSION             "1.1.0"
#if defined(__linux__)
#  define DEFAULT_SOCKET_PATH "/run/localchat/socket"
#  define LEGACY_SOCKET_PATH  "/run/localchat.sock"
#  define HAVE_SYSTEMD_COMMANDS 1
#  define HAVE_LEGACY_SOCKET   1
#else
#  define DEFAULT_SOCKET_PATH "/tmp/localchat.sock"
#  define LEGACY_SOCKET_PATH  "/tmp/localchat.sock"
#  define HAVE_SYSTEMD_COMMANDS 0
#  define HAVE_LEGACY_SOCKET   0
#endif
#define INSTALL_URL         "https://localchat.michjzuman.com/install.sh"
#define REMOTE_CLIENT_URL   "https://raw.githubusercontent.com/michjzuman/localchat/main/localchat.c"
#define MAX_MSG_LEN         8192
#define MAX_BODY_LEN        4096
#define LEN_PREFIX_BYTES    4
#define INPUT_WMAX          1024
#define USERNAME_MAX        64
#define LOG_CAP             1024
#define BUBBLE_WRAP_COLS    64

static int            sock_fd = -1;
static WINDOW        *status_win = NULL;
static WINDOW        *chat_win  = NULL;
static WINDOW        *input_win = NULL;
static WINDOW        *chat_draw_win = NULL;
static char           local_username[USERNAME_MAX];
static const char    *socket_path = DEFAULT_SOCKET_PATH;

static int            term_h = 0;
static int            term_w = 0;
static int            chat_h = 0;
static int            colors_enabled = 0;
static int            suppress_replay_duplicates = 0;
static time_t         next_reconnect_at = 0;

static unsigned char  recv_buf[LEN_PREFIX_BYTES + MAX_MSG_LEN + 16];
static size_t         recv_len = 0;

static wchar_t        input_buf[INPUT_WMAX + 1];
static int            input_len    = 0;
static int            input_cursor = 0;

/* History log for re-render on resize and scrollback. */
static char          *log_lines[LOG_CAP];
static int            log_count = 0;
static int            scroll_offset = 0;

static volatile sig_atomic_t exit_requested   = 0;
static volatile sig_atomic_t resize_pending   = 0;

typedef struct {
    int         is_chat;
    int         own;
    const char *name;
    const char *msg;
} ParsedLine;

enum {
    CP_OWN = 1,
    CP_OWN_DIM,
    CP_OTHER,
    CP_OTHER_DIM,
    CP_SYSTEM,
    CP_INPUT,
    CP_INPUT_MARK,
    CP_SCROLL
};

enum {
    COLOR_AUTO = 0,
    COLOR_ALWAYS,
    COLOR_NEVER
};

static int color_mode = COLOR_AUTO;

/* ---------- low-level helpers ---------- */

static void on_signal(int sig) {
    if (sig == SIGWINCH) resize_pending = 1;
    else                  exit_requested = 1;
}

static int set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static int wcs_to_mb(const wchar_t *src, size_t n, char *out, size_t out_cap) {
    mbstate_t st; memset(&st, 0, sizeof(st));
    size_t total = 0;
    for (size_t i = 0; i < n && total + MB_LEN_MAX + 1 < out_cap; i++) {
        char tmp[MB_LEN_MAX];
        size_t r = wcrtomb(tmp, src[i], &st);
        if (r == (size_t)-1) {
            memset(&st, 0, sizeof(st));
            continue;
        }
        memcpy(out + total, tmp, r);
        total += r;
    }
    if (total >= out_cap) total = out_cap - 1;
    out[total] = '\0';
    return (int)total;
}

static int wcs_columns(const wchar_t *s, int n) {
    int cols = 0;
    for (int i = 0; i < n; i++) {
        int w = wcwidth(s[i]);
        if (w > 0) cols += w;
    }
    return cols;
}

static void init_color_pairs(void) {
    if (color_mode == COLOR_NEVER) return;
    if (!has_colors() || start_color() == ERR) return;
    if (COLOR_PAIRS <= CP_SCROLL) return;

    short bg = COLOR_BLACK;
    if (use_default_colors() == OK) bg = -1;

    init_pair(CP_OWN,        COLOR_GREEN,  bg);
    init_pair(CP_OWN_DIM,    COLOR_GREEN,  bg);
    init_pair(CP_OTHER,      COLOR_YELLOW, bg);
    init_pair(CP_OTHER_DIM,  COLOR_YELLOW, bg);
    init_pair(CP_SYSTEM,     COLOR_CYAN,   bg);
    init_pair(CP_INPUT,      COLOR_WHITE,  bg);
    init_pair(CP_INPUT_MARK, COLOR_GREEN,  bg);
    init_pair(CP_SCROLL,     COLOR_BLACK,  COLOR_YELLOW);
    colors_enabled = 1;
}

static void style_on(WINDOW *win, int pair, int attrs) {
    if (colors_enabled && pair > 0) wattron(win, COLOR_PAIR(pair));
    if (attrs) wattron(win, attrs);
}

static void style_off(WINDOW *win, int pair, int attrs) {
    if (attrs) wattroff(win, attrs);
    if (colors_enabled && pair > 0) wattroff(win, COLOR_PAIR(pair));
}

static int message_pair(int own) {
    return own ? CP_OWN : CP_OTHER;
}

static int border_pair(int own) {
    return own ? CP_OWN_DIM : CP_OTHER_DIM;
}

static int max_message_columns(void) {
    int h, w;
    getmaxyx(chat_win, h, w);
    (void)h;
    int cols;
    if (w > 12) cols = w - 8;
    else if (w > 6) cols = w - 5;
    else cols = 1;
    if (cols > BUBBLE_WRAP_COLS) cols = BUBBLE_WRAP_COLS;
    return cols;
}

static void render_status(void) {
    if (!status_win) return;
    int h, w; getmaxyx(status_win, h, w); (void)h;
    werase(status_win);
    if (w <= 0) return;

    const char *state = sock_fd >= 0 ? "connected" : "reconnecting";
    char line[512];
    snprintf(line, sizeof(line), " localchat | %s | %s | %s",
             local_username, state, socket_path);
    int pair = sock_fd >= 0 ? CP_INPUT_MARK : CP_SYSTEM;
    style_on(status_win, pair, A_BOLD);
    mvwaddnstr(status_win, 0, 0, " localchat ", w);
    style_off(status_win, pair, A_BOLD);

    if (w > 11) {
        style_on(status_win, CP_INPUT, A_DIM);
        mvwaddnstr(status_win, 0, 11, line + 11, w - 11);
        style_off(status_win, CP_INPUT, A_DIM);
    }
    wnoutrefresh(status_win);
}

static void load_local_username(void) {
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name && pw->pw_name[0]) {
        snprintf(local_username, sizeof(local_username), "%s", pw->pw_name);
    } else {
        snprintf(local_username, sizeof(local_username), "uid-%ld", (long)getuid());
    }
}

/* ---------- rendering ---------- */

static void put_spaces(WINDOW *win, int n) {
    for (int i = 0; i < n; i++) waddch(win, ' ');
}

static void put_repeat(WINDOW *win, const char *s, int n) {
    for (int i = 0; i < n; i++) waddstr(win, s);
}

static int mb_columns(const char *s) {
    mbstate_t st; memset(&st, 0, sizeof(st));
    int cols = 0;
    while (*s) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, s, MB_CUR_MAX, &st);
        if (r == (size_t)-1 || r == (size_t)-2) {
            memset(&st, 0, sizeof(st));
            s++;
            cols++;
            continue;
        }
        if (r == 0) break;
        int cw = wcwidth(wc);
        if (cw > 0) cols += cw;
        s += r;
    }
    return cols;
}

static const char *display_system_text(const char *text) {
    if (strncmp(text, "[system]", 8) != 0) return text;
    text += 8;
    if (*text == ' ') text++;
    return text;
}

static WINDOW *chat_target(void) {
    return chat_draw_win ? chat_draw_win : chat_win;
}

static void render_system(const char *text) {
    text = display_system_text(text);
    int h, w; getmaxyx(chat_win, h, w); (void)h;
    WINDOW *win = chat_target();
    int cols = mb_columns(text);
    int pad  = (w - cols) / 2;
    if (pad < 0) pad = 0;
    style_on(win, CP_SYSTEM, A_DIM);
    put_spaces(win, pad);
    waddstr(win, text);
    waddch(win, '\n');
    style_off(win, CP_SYSTEM, A_DIM);
}

static int next_message_chunk(const char **cursor, int max_cols,
                              const char **chunk_start, size_t *chunk_len,
                              int *chunk_cols) {
    mbstate_t st; memset(&st, 0, sizeof(st));
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    const char *start = p;
    const char *end = p;
    const char *last_break = NULL;
    const char *last_break_trim = NULL;
    int last_break_cols = 0;
    int cols = 0;

    if (*p == '\0') return 0;
    while (*p) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, p, MB_CUR_MAX, &st);
        if (r == (size_t)-1 || r == (size_t)-2) {
            memset(&st, 0, sizeof(st));
            p++;
            end = p;
            cols++;
            if (cols >= max_cols) break;
            continue;
        }
        if (r == 0) break;
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        if (cols + cw > max_cols && end > start) {
            if (last_break && last_break_trim > start) {
                end = last_break_trim;
                cols = last_break_cols;
                p = last_break;
            }
            break;
        }
        if (iswspace(wc) && end > start) {
            last_break = p + r;
            last_break_trim = p;
            last_break_cols = cols;
        }
        cols += cw;
        p += r;
        end = p;
        if (cols >= max_cols) break;
    }

    if (end == start) return 0;
    *cursor = p;
    *chunk_start = start;
    *chunk_len = (size_t)(end - start);
    *chunk_cols = cols;
    return 1;
}

static int wrapped_message_columns(const char *message, int max_cols) {
    if (message[0] == '\0') return 0;

    const char *p = message;
    int widest = 0;
    while (*p) {
        const char *chunk_start;
        size_t chunk_len;
        int chunk_cols;
        if (!next_message_chunk(&p, max_cols, &chunk_start, &chunk_len, &chunk_cols)) break;
        (void)chunk_start;
        (void)chunk_len;
        if (chunk_cols > widest) widest = chunk_cols;
    }
    return widest;
}

static void render_bubble_line(const char *body, int body_cols, int line_cols,
                               int own, int left_pad, int show_tail) {
    WINDOW *win = chat_target();
    int fill = body_cols - line_cols;
    if (fill < 0) fill = 0;

    if (own) {
        put_spaces(win, left_pad);
        style_on(win, border_pair(own), A_DIM);
        waddstr(win, "│ ");
        style_off(win, border_pair(own), A_DIM);

        style_on(win, message_pair(own), 0);
        waddstr(win, body);
        put_spaces(win, fill);
        style_off(win, message_pair(own), 0);

        style_on(win, border_pair(own), A_DIM);
        waddstr(win, " │");
        style_off(win, border_pair(own), A_DIM);
        if (show_tail) {
            style_on(win, message_pair(own), A_BOLD);
            waddstr(win, "❯");
            style_off(win, message_pair(own), A_BOLD);
            waddch(win, '\n');
        } else {
            waddstr(win, " \n");
        }
    } else {
        if (show_tail) {
            style_on(win, message_pair(own), A_BOLD);
            waddstr(win, "❮");
            style_off(win, message_pair(own), A_BOLD);
        } else {
            waddstr(win, " ");
        }
        style_on(win, border_pair(own), A_DIM);
        waddstr(win, "│ ");
        style_off(win, border_pair(own), A_DIM);

        style_on(win, message_pair(own), 0);
        waddstr(win, body);
        put_spaces(win, fill);
        style_off(win, message_pair(own), 0);

        style_on(win, border_pair(own), A_DIM);
        waddstr(win, " │");
        style_off(win, border_pair(own), A_DIM);
        waddch(win, '\n');
    }
}

static void render_bubble(const char *name, const char *message, int own,
                          int show_name, int show_tail) {
    int h, w; getmaxyx(chat_win, h, w); (void)h;
    WINDOW *win = chat_target();
    int name_cols = mb_columns(name);
    int max_cols  = max_message_columns();
    if (max_cols < 1) max_cols = 1;
    if (own) {
        int own_max_cols = w > 6 ? w - 6 : 1;
        if (max_cols > own_max_cols) max_cols = own_max_cols;
    }

    int body_cols = wrapped_message_columns(message, max_cols);
    int bubble_cols = body_cols + 5; /* "│ body │❯" / "❮│ body │" both = body+5 */
    int left_pad = 0;

    if (own) {
        int right_edge = w > 1 ? w - 1 : w;
        if (show_name) {
            int name_pad = right_edge - (name_cols + 3);
            if (name_pad < 0) name_pad = 0;
            put_spaces(win, name_pad);
            style_on(win, message_pair(own), A_BOLD);
            wprintw(win, "%s   \n", name);
            style_off(win, message_pair(own), A_BOLD);
        }

        left_pad = right_edge - bubble_cols;
        if (left_pad < 0) left_pad = 0;

        put_spaces(win, left_pad);
        style_on(win, border_pair(own), A_DIM);
        waddstr(win, "╭─");
        put_repeat(win, "─", body_cols);
        waddstr(win, "─╮ \n");
        style_off(win, border_pair(own), A_DIM);
    } else {
        if (show_name) {
            style_on(win, message_pair(own), A_BOLD);
            wprintw(win, "  %s\n", name);
            style_off(win, message_pair(own), A_BOLD);
        }
        style_on(win, border_pair(own), A_DIM);
        waddstr(win, " ╭─");
        put_repeat(win, "─", body_cols);
        waddstr(win, "─╮\n");
        style_off(win, border_pair(own), A_DIM);
    }

    if (message[0] == '\0') {
        render_bubble_line("", body_cols, 0, own, left_pad, show_tail);
    } else {
        const char *p = message;
        while (*p) {
            const char *chunk_start;
            size_t chunk_len;
            int chunk_cols;
            if (!next_message_chunk(&p, max_cols, &chunk_start, &chunk_len, &chunk_cols)) break;

            char chunk[MAX_BODY_LEN + 8];
            if (chunk_len >= sizeof(chunk)) chunk_len = sizeof(chunk) - 1;
            memcpy(chunk, chunk_start, chunk_len);
            chunk[chunk_len] = '\0';

            render_bubble_line(chunk, body_cols, chunk_cols, own, left_pad,
                               show_tail && *p == '\0');
        }
    }

    if (own) {
        put_spaces(win, left_pad);
        style_on(win, border_pair(own), A_DIM);
        waddstr(win, "╰─");
        put_repeat(win, "─", body_cols);
        waddstr(win, "─╯ \n");
        style_off(win, border_pair(own), A_DIM);
    } else {
        style_on(win, border_pair(own), A_DIM);
        waddstr(win, " ╰─");
        put_repeat(win, "─", body_cols);
        waddstr(win, "─╯\n");
        style_off(win, border_pair(own), A_DIM);
    }
}

static ParsedLine parse_chat_line(char *line) {
    ParsedLine p = { .is_chat = 0, .own = 0, .name = NULL, .msg = NULL };
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) return p;

    if (strncmp(line, "[system]", 8) == 0) {
        return p;
    }
    if (line[0] == '[') {
        char *end = strchr(line + 1, ']');
        if (end) {
            *end = '\0';
            p.is_chat = 1;
            p.name = line + 1;
            p.msg  = end + 1;
            if (*p.msg == ' ') p.msg++;
            p.own = strcmp(p.name, local_username) == 0;
        }
    }
    return p;
}

static void log_add(const char *raw) {
    int slot = log_count % LOG_CAP;
    free(log_lines[slot]);
    log_lines[slot] = strdup(raw);
    log_count++;
}

static int log_contains(const char *raw) {
    int oldest = log_count > LOG_CAP ? log_count - LOG_CAP : 0;
    for (int i = oldest; i < log_count; i++) {
        const char *line = log_lines[i % LOG_CAP];
        if (line && strcmp(line, raw) == 0) return 1;
    }
    return 0;
}

static int oldest_log_index(void) {
    return log_count > LOG_CAP ? log_count - LOG_CAP : 0;
}

static int wrapped_message_line_count(const char *message, int max_cols) {
    if (message[0] == '\0') return 1;

    const char *p = message;
    int lines = 0;
    while (*p) {
        const char *chunk_start;
        size_t chunk_len;
        int chunk_cols;
        if (!next_message_chunk(&p, max_cols, &chunk_start, &chunk_len, &chunk_cols)) break;
        (void)chunk_start;
        (void)chunk_len;
        (void)chunk_cols;
        lines++;
    }
    return lines > 0 ? lines : 1;
}

static int bubble_line_count(const ParsedLine *line, int show_name) {
    int max_cols = max_message_columns();
    if (max_cols < 1) max_cols = 1;
    if (line->own) {
        int h, w; getmaxyx(chat_win, h, w); (void)h;
        int own_max_cols = w > 6 ? w - 6 : 1;
        if (max_cols > own_max_cols) max_cols = own_max_cols;
    }
    return (show_name ? 1 : 0) + 2 + wrapped_message_line_count(line->msg, max_cols);
}

static int rendered_log_rows(int idx) {
    const char *raw = log_lines[idx % LOG_CAP];
    if (!raw) return 0;

    char tmp[MAX_MSG_LEN + 32];
    snprintf(tmp, sizeof(tmp), "%s", raw);
    ParsedLine cur = parse_chat_line(tmp);
    if (!cur.is_chat) {
        int h, w; getmaxyx(chat_win, h, w); (void)h;
        int cols = mb_columns(display_system_text(tmp));
        if (w <= 0) return 1;
        return cols > 0 ? (cols + w - 1) / w : 1;
    }

    int show_name = 1;
    int oldest = oldest_log_index();
    if (idx > oldest) {
        const char *prev_raw = log_lines[(idx - 1) % LOG_CAP];
        if (prev_raw) {
            char prev_tmp[MAX_MSG_LEN + 32];
            snprintf(prev_tmp, sizeof(prev_tmp), "%s", prev_raw);
            ParsedLine prev = parse_chat_line(prev_tmp);
            if (prev.is_chat && strcmp(prev.name, cur.name) == 0) show_name = 0;
        }
    }

    return bubble_line_count(&cur, show_name);
}

static int total_rendered_rows(void) {
    int oldest = oldest_log_index();
    int rows = 0;
    for (int i = oldest; i < log_count; i++) {
        rows += rendered_log_rows(i);
    }
    return rows;
}

static int max_scroll_offset(void) {
    int rows = total_rendered_rows();
    if (rows <= chat_h) return 0;

    int reserved_rows = chat_h > 1 ? 1 : 0;
    int visible_rows = chat_h - reserved_rows;
    if (visible_rows < 1) visible_rows = 1;
    return rows - visible_rows;
}

static void clamp_scroll_offset(void) {
    int max_scroll = max_scroll_offset();
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
}

static void rerender_chat(void) {
    werase(chat_win);
    clamp_scroll_offset();

    int total_rows = total_rendered_rows();
    if (total_rows <= 0) return;

    int reserved_rows = scroll_offset > 0 && chat_h > 1 ? 1 : 0;
    int visible_rows = chat_h - reserved_rows;
    if (visible_rows < 1) visible_rows = 1;

    int start_row = total_rows - visible_rows - scroll_offset;
    if (start_row < 0) start_row = 0;
    if (start_row > total_rows - 1) start_row = total_rows - 1;

    int pad_rows = total_rows + 1;
    if (pad_rows < start_row + visible_rows + 1) {
        pad_rows = start_row + visible_rows + 1;
    }
    int pad_cols = term_w > 0 ? term_w : 1;
    WINDOW *pad = newpad(pad_rows, pad_cols);
    if (!pad) return;
    scrollok(pad, FALSE);
    werase(pad);

    chat_draw_win = pad;
    int oldest = oldest_log_index();
    for (int i = oldest; i < log_count; i++) {
        const char *raw = log_lines[i % LOG_CAP];
        if (!raw) continue;
        char tmp[MAX_MSG_LEN + 32];
        snprintf(tmp, sizeof(tmp), "%s", raw);
        ParsedLine cur = parse_chat_line(tmp);
        if (!cur.is_chat) {
            render_system(tmp);
            continue;
        }

        int show_name = 1;
        int show_tail = 1;

        if (i > oldest) {
            const char *prev_raw = log_lines[(i - 1) % LOG_CAP];
            if (prev_raw) {
                char prev_tmp[MAX_MSG_LEN + 32];
                snprintf(prev_tmp, sizeof(prev_tmp), "%s", prev_raw);
                ParsedLine prev = parse_chat_line(prev_tmp);
                if (prev.is_chat && strcmp(prev.name, cur.name) == 0) show_name = 0;
            }
        }
        if (i + 1 < log_count) {
            const char *next_raw = log_lines[(i + 1) % LOG_CAP];
            if (next_raw) {
                char next_tmp[MAX_MSG_LEN + 32];
                snprintf(next_tmp, sizeof(next_tmp), "%s", next_raw);
                ParsedLine next = parse_chat_line(next_tmp);
                if (next.is_chat && strcmp(next.name, cur.name) == 0) show_tail = 0;
            }
        }

        render_bubble(cur.name, cur.msg, cur.own, show_name, show_tail);
    }
    chat_draw_win = NULL;

    if (scroll_offset > 0) {
        if (colors_enabled) style_on(chat_win, CP_SCROLL, A_BOLD);
        else wattron(chat_win, A_REVERSE);
        mvwprintw(chat_win, 0, 0, " -- scrollback (%d rows) -- ", scroll_offset);
        if (colors_enabled) style_off(chat_win, CP_SCROLL, A_BOLD);
        else wattroff(chat_win, A_REVERSE);
    }

    int dest_top = reserved_rows;
    int dest_bottom = dest_top + visible_rows - 1;
    if (dest_bottom >= chat_h) dest_bottom = chat_h - 1;
    if (dest_top <= dest_bottom && term_w > 0) {
        copywin(pad, chat_win, start_row, 0, dest_top, 0,
                dest_bottom, term_w - 1, 0);
    }
    delwin(pad);
}

static void render_input(void) {
    int h, w; getmaxyx(input_win, h, w); (void)h;
    werase(input_win);
    if (w < 6) {
        style_on(input_win, CP_INPUT, A_DIM);
        box(input_win, 0, 0);
        style_off(input_win, CP_INPUT, A_DIM);
        wnoutrefresh(input_win);
        return;
    }
    style_on(input_win, CP_INPUT, A_DIM);
    mvwaddstr(input_win, 0, 0, "╭");
    put_repeat(input_win, "─", w - 2);
    waddstr(input_win, "╮");

    mvwaddstr(input_win, 1, 0, "│");
    put_spaces(input_win, w - 2);
    mvwaddstr(input_win, 1, w - 1, "│");

    mvwaddstr(input_win, 2, 0, "╰");
    put_repeat(input_win, "─", w - 2);
    waddstr(input_win, "╯");
    style_off(input_win, CP_INPUT, A_DIM);

    style_on(input_win, CP_INPUT_MARK, A_BOLD);
    mvwaddstr(input_win, 1, w - 3, "⏎");
    style_off(input_win, CP_INPUT_MARK, A_BOLD);

    int input_cols = w - 5;
    if (input_cols < 1) input_cols = 1;

    int cursor_col = wcs_columns(input_buf, input_cursor);
    int scroll = 0;
    if (cursor_col >= input_cols) scroll = cursor_col - input_cols + 1;

    int sw = 0, sc = 0;
    while (sw < input_len && sc < scroll) {
        int cw = wcwidth(input_buf[sw]);
        if (cw < 0) cw = 1;
        if (sc + cw > scroll) break;
        sc += cw;
        sw++;
    }

    int visible = 0;
    int wi = sw;
    while (wi < input_len) {
        int cw = wcwidth(input_buf[wi]);
        if (cw < 0) cw = 1;
        if (visible + cw > input_cols) break;
        char tmp[MB_LEN_MAX + 1];
        mbstate_t st; memset(&st, 0, sizeof(st));
        size_t n = wcrtomb(tmp, input_buf[wi], &st);
        if (n != (size_t)-1) {
            tmp[n] = '\0';
            style_on(input_win, CP_INPUT, 0);
            mvwaddstr(input_win, 1, 2 + visible, tmp);
            style_off(input_win, CP_INPUT, 0);
        }
        visible += cw;
        wi++;
    }

    int cur_x = 2 + (cursor_col - sc);
    if (cur_x > w - 4) cur_x = w - 4;
    if (cur_x < 2)     cur_x = 2;
    wmove(input_win, 1, cur_x);
    wnoutrefresh(input_win);
}

static void redraw_all(void) {
    render_status();
    wnoutrefresh(chat_win);
    render_input();
    doupdate();
}

/* ---------- I/O ---------- */

static int send_text(const char *text, size_t len) {
    if (sock_fd < 0) return -1;
    if (len == 0 || len > MAX_BODY_LEN) return -1;
    unsigned char buf[LEN_PREFIX_BYTES + MAX_BODY_LEN];
    uint32_t netlen = htonl((uint32_t)len);
    memcpy(buf, &netlen, sizeof(netlen));
    memcpy(buf + sizeof(netlen), text, len);
    size_t total = sizeof(netlen) + len;
    size_t sent  = 0;
    while (sent < total) {
        ssize_t n = write(sock_fd, buf + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Wait briefly and retry; non-blocking fd. */
                struct pollfd p = { .fd = sock_fd, .events = POLLOUT };
                if (poll(&p, 1, 1000) <= 0) return -1;
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void send_input(void) {
    if (input_len == 0) return;
    char mb[INPUT_WMAX * 4 + 1];
    int n = wcs_to_mb(input_buf, (size_t)input_len, mb, sizeof(mb));
    if (n > 0) {
        if (send_text(mb, (size_t)n) != 0) {
            log_add("[system] failed to send message");
            scroll_offset = 0;
            rerender_chat();
        }
    }
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = L'\0';
}

static int handle_socket_in(void) {
    while (1) {
        if (recv_len >= sizeof(recv_buf)) {
            /* Should not happen if MAX_MSG_LEN is honored, but guard anyway. */
            return -1;
        }
        ssize_t n = read(sock_fd, recv_buf + recv_len, sizeof(recv_buf) - recv_len);
        if (n > 0) {
            recv_len += (size_t)n;
            while (1) {
                if (recv_len < LEN_PREFIX_BYTES) break;
                uint32_t netlen;
                memcpy(&netlen, recv_buf, LEN_PREFIX_BYTES);
                uint32_t mlen = ntohl(netlen);
                if (mlen > MAX_MSG_LEN) return -1;
                if (recv_len < LEN_PREFIX_BYTES + mlen) break;
                char body[MAX_MSG_LEN + 1];
                memcpy(body, recv_buf + LEN_PREFIX_BYTES, mlen);
                body[mlen] = '\0';
                size_t consumed = LEN_PREFIX_BYTES + mlen;
                memmove(recv_buf, recv_buf + consumed, recv_len - consumed);
                recv_len -= consumed;

                int is_welcome = strcmp(body, "[system] welcome to localchat") == 0;
                if (suppress_replay_duplicates && !is_welcome && log_contains(body)) {
                    continue;
                }
                log_add(body);
                if (is_welcome) suppress_replay_duplicates = 0;
                if (scroll_offset == 0) rerender_chat();
            }
            continue;
        }
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

/* ---------- input editing ---------- */

static void insert_char(wchar_t wc) {
    if (input_len >= INPUT_WMAX) return;
    if (wc < 0x20 && wc != L'\t') return;
    int w = wcwidth(wc);
    if (w < 0) return;
    memmove(input_buf + input_cursor + 1, input_buf + input_cursor,
            (size_t)(input_len - input_cursor) * sizeof(wchar_t));
    input_buf[input_cursor] = wc;
    input_len++;
    input_cursor++;
    input_buf[input_len] = L'\0';
}

static void delete_back(void) {
    if (input_cursor == 0) return;
    memmove(input_buf + input_cursor - 1, input_buf + input_cursor,
            (size_t)(input_len - input_cursor) * sizeof(wchar_t));
    input_cursor--;
    input_len--;
    input_buf[input_len] = L'\0';
}

static void delete_forward(void) {
    if (input_cursor >= input_len) return;
    memmove(input_buf + input_cursor, input_buf + input_cursor + 1,
            (size_t)(input_len - input_cursor - 1) * sizeof(wchar_t));
    input_len--;
    input_buf[input_len] = L'\0';
}

static void delete_word_back(void) {
    int p = input_cursor;
    while (p > 0 && iswspace(input_buf[p - 1])) p--;
    while (p > 0 && !iswspace(input_buf[p - 1])) p--;
    if (p == input_cursor) return;
    memmove(input_buf + p, input_buf + input_cursor,
            (size_t)(input_len - input_cursor) * sizeof(wchar_t));
    input_len  -= input_cursor - p;
    input_cursor = p;
    input_buf[input_len] = L'\0';
}

static void scroll_by(int delta) {
    int old_offset = scroll_offset;
    scroll_offset += delta;
    clamp_scroll_offset();
    if (scroll_offset != old_offset) rerender_chat();
}

static void scroll_page_up(void) {
    int step = chat_h / 4; if (step < 1) step = 1;
    scroll_by(step);
}

static void scroll_page_down(void) {
    int step = chat_h / 4; if (step < 1) step = 1;
    scroll_by(-step);
}

static void handle_resize(void);

static void process_keys(void) {
    while (1) {
        wint_t wc;
        int r = wget_wch(input_win, &wc);
        if (r == ERR) break;

        if (r == KEY_CODE_YES) {
            switch ((int)wc) {
                case KEY_BACKSPACE: delete_back();    break;
                case KEY_DC:        delete_forward(); break;
                case KEY_LEFT:      if (input_cursor > 0)         input_cursor--; break;
                case KEY_RIGHT:     if (input_cursor < input_len) input_cursor++; break;
                case KEY_HOME:      input_cursor = 0;          break;
                case KEY_END:       input_cursor = input_len;  break;
                case KEY_UP:        scroll_by(1); break;
                case KEY_DOWN:      scroll_by(-1); break;
                case KEY_PPAGE:     scroll_page_up();   break;
                case KEY_NPAGE:     scroll_page_down(); break;
                case KEY_RESIZE:    handle_resize(); break;
                case KEY_ENTER:     send_input();    break;
                default: break;
            }
        } else {
            if (wc == L'\n' || wc == L'\r') { send_input(); }
            else if (wc == 127 || wc == L'\b') { delete_back(); }
            else if (wc == 1)  { input_cursor = 0; }                            /* Ctrl-A */
            else if (wc == 5)  { input_cursor = input_len; }                    /* Ctrl-E */
            else if (wc == 21) { input_len = 0; input_cursor = 0;
                                 input_buf[0] = L'\0'; }                        /* Ctrl-U */
            else if (wc == 11) { input_len = input_cursor;
                                 input_buf[input_len] = L'\0'; }                /* Ctrl-K */
            else if (wc == 23) { delete_word_back(); }                          /* Ctrl-W */
            else if (wc == 12) { rerender_chat(); }                             /* Ctrl-L */
            else if (wc == 4)  { exit_requested = 1; }                          /* Ctrl-D */
            else if (wc == 3)  { exit_requested = 1; }                          /* Ctrl-C */
            else { insert_char((wchar_t)wc); }
        }
    }
}

static void handle_resize(void) {
    endwin();
    refresh();
    getmaxyx(stdscr, term_h, term_w);
    chat_h = term_h - 4;
    if (chat_h < 1) chat_h = 1;
    if (status_win) delwin(status_win);
    if (chat_win) delwin(chat_win);
    if (input_win) delwin(input_win);
    status_win = newwin(1, term_w, 0, 0);
    chat_win  = newwin(chat_h, term_w, 1, 0);
    input_win = newwin(3, term_w, chat_h + 1, 0);
    if (chat_win)  scrollok(chat_win, FALSE);
    if (input_win) { keypad(input_win, TRUE); nodelay(input_win, TRUE); }
    rerender_chat();
    redraw_all();
}

/* ---------- connect ---------- */

static int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static int connect_server(int verbose) {
    int fd = connect_unix(socket_path);
    if (fd >= 0) return fd;
    int e = errno;

#if HAVE_LEGACY_SOCKET
    /* Fall back to legacy path if the user did not override -s. */
    if (strcmp(socket_path, DEFAULT_SOCKET_PATH) == 0) {
        struct stat st;
        if (stat(LEGACY_SOCKET_PATH, &st) == 0) {
            int fd2 = connect_unix(LEGACY_SOCKET_PATH);
            if (fd2 >= 0) {
                if (verbose) fprintf(stderr, "note: using legacy socket %s\n", LEGACY_SOCKET_PATH);
                return fd2;
            }
        }
    }
#endif

    if (verbose) {
        fprintf(stderr, "could not connect to %s: %s\n", socket_path, strerror(e));
#if HAVE_SYSTEMD_COMMANDS
        fprintf(stderr, "is the localchatd service running?\n");
        fprintf(stderr, "  localchat status\n");
#else
        fprintf(stderr, "start the daemon in another terminal:\n");
        fprintf(stderr, "  localchatd --socket %s\n", socket_path);
#endif
    }
    return -1;
}

static void close_server_connection(void) {
    if (sock_fd >= 0) close(sock_fd);
    sock_fd = -1;
    recv_len = 0;
}

static void mark_disconnected(void) {
    close_server_connection();
    suppress_replay_duplicates = 0;
    next_reconnect_at = time(NULL);
    log_add("[system] disconnected from server");
    scroll_offset = 0;
    rerender_chat();
}

static int try_reconnect(void) {
    if (sock_fd >= 0) return 1;
    time_t now = time(NULL);
    if (now < next_reconnect_at) return 0;

    int fd = connect_server(0);
    if (fd >= 0) {
        sock_fd = fd;
        set_nonblock(sock_fd);
        recv_len = 0;
        suppress_replay_duplicates = log_count > 0;
        next_reconnect_at = 0;
        return 1;
    }

    next_reconnect_at = now + 1;
    return 0;
}

/* ---------- management commands ---------- */

static int command_status(int rc) {
    if (rc == -1) return 1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
    return 1;
}

static int run_shell_command(const char *cmd) {
    return command_status(system(cmd));
}

static int require_root(const char *command) {
    if (geteuid() == 0) return 1;
    fprintf(stderr, "%s must be run as root (use sudo localchat %s)\n",
            command, command);
    return 0;
}

static void trim_line(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static int fetch_remote_version(char *out, size_t cap) {
    const char *cmd =
        "curl -fsSL " REMOTE_CLIENT_URL " 2>/dev/null | "
        "sed -n 's/^#define VERSION[[:space:]]*\"\\([^\"]*\\)\"/\\1/p' | "
        "head -n 1";
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    if (!fgets(out, (int)cap, fp)) out[0] = '\0';
    int rc = command_status(pclose(fp));
    trim_line(out);
    if (rc != 0 || out[0] == '\0') return -1;
    return 0;
}

static int print_update_check(const char *remote) {
    printf("installed: %s\n", VERSION);
    printf("latest:    %s\n", remote);
    if (strcmp(VERSION, remote) == 0) {
        printf("status:    up to date\n");
        return 0;
    }
    printf("status:    update available\n");
    return 1;
}

static int run_update(int check_only) {
    char remote[64];
    int have_remote = fetch_remote_version(remote, sizeof(remote)) == 0;

    if (check_only) {
        if (!have_remote) {
            fprintf(stderr, "could not check latest localchat version\n");
            return 1;
        }
        return print_update_check(remote);
    }

    if (have_remote) {
        int needs_update = print_update_check(remote);
        if (!needs_update) return 0;
    } else {
        fprintf(stderr, "warning: could not check latest localchat version; updating anyway\n");
    }

#if !HAVE_SYSTEMD_COMMANDS
    fprintf(stderr, "localchat update installs the Linux systemd service and is not supported on this platform.\n");
    fprintf(stderr, "update from a checkout instead: git pull && make && sudo make install\n");
    return 2;
#endif

    if (!require_root("update")) return 1;
    printf("updating localchat from %s\n", INSTALL_URL);
    return run_shell_command("curl -fsSL " INSTALL_URL " | bash");
}

static int run_service_command(const char *action, int needs_root) {
#if !HAVE_SYSTEMD_COMMANDS
    (void)needs_root;
    fprintf(stderr, "localchat %s uses systemd and is only available on Linux.\n", action);
    fprintf(stderr, "on this platform, run the daemon manually:\n");
    fprintf(stderr, "  localchatd --socket %s\n", socket_path);
    return 2;
#else
    char cmd[128];
    if (needs_root && !require_root(action)) return 1;
    snprintf(cmd, sizeof(cmd), "systemctl %s localchatd", action);
    return run_shell_command(cmd);
#endif
}

static int run_status(void) {
#if HAVE_SYSTEMD_COMMANDS
    return run_service_command("status", 0);
#else
    struct stat st;
    printf("localchat %s\n", VERSION);
    printf("service:   manual (systemd is Linux-only)\n");
    printf("socket:    %s\n", socket_path);

    if (stat(socket_path, &st) != 0) {
        printf("daemon:    not reachable\n");
        printf("start:     localchatd --socket %s\n", socket_path);
        return 3;
    }

    int fd = connect_unix(socket_path);
    if (fd >= 0) {
        close(fd);
        printf("daemon:    reachable\n");
        return 0;
    }

    printf("daemon:    socket exists, connect failed: %s\n", strerror(errno));
    printf("start:     localchatd --socket %s\n", socket_path);
    return 1;
#endif
}

static int run_logs(int follow) {
#if !HAVE_SYSTEMD_COMMANDS
    (void)follow;
    fprintf(stderr, "localchat logs reads journald and is only available on Linux.\n");
    fprintf(stderr, "on this platform, localchatd logs to the terminal where it is running.\n");
    return 2;
#else
    if (follow) {
        return run_shell_command("journalctl -u localchatd -f");
    }
    return run_shell_command("journalctl -u localchatd --no-pager -n 80");
#endif
}

static int run_uninstall(void) {
    if (!require_root("uninstall")) return 1;
#if HAVE_SYSTEMD_COMMANDS
    int rc;
    rc = system("systemctl stop localchatd 2>/dev/null"); (void)rc;
    rc = system("systemctl disable localchatd 2>/dev/null"); (void)rc;
    unlink("/etc/systemd/system/localchatd.service");
    unlink("/usr/local/sbin/localchatd");
    unlink("/usr/local/bin/localchat");
    unlink(DEFAULT_SOCKET_PATH);
    unlink(LEGACY_SOCKET_PATH);
    rmdir("/run/localchat");
    rc = system("systemctl daemon-reload 2>/dev/null"); (void)rc;
#else
    unlink("/usr/local/sbin/localchatd");
    unlink("/usr/local/bin/localchat");
    unlink(DEFAULT_SOCKET_PATH);
#endif
    printf("localchat has been uninstalled.\n");
    return 0;
}

static int is_command(const char *arg) {
    return strcmp(arg, "update") == 0 ||
           strcmp(arg, "status") == 0 ||
           strcmp(arg, "start") == 0 ||
           strcmp(arg, "stop") == 0 ||
           strcmp(arg, "restart") == 0 ||
           strcmp(arg, "logs") == 0 ||
           strcmp(arg, "uninstall") == 0;
}

static int parse_color_mode(const char *value) {
    if (strcmp(value, "auto") == 0) {
        color_mode = COLOR_AUTO;
        return 0;
    }
    if (strcmp(value, "always") == 0) {
        color_mode = COLOR_ALWAYS;
        return 0;
    }
    if (strcmp(value, "never") == 0) {
        color_mode = COLOR_NEVER;
        return 0;
    }
    return -1;
}

static void usage(FILE *out) {
    fprintf(out,
        "localchat %s\n"
        "Usage: localchat [OPTIONS] [COMMAND]\n"
        "\n"
        "Commands:\n"
        "  update               Download and run the Linux installer (requires root).\n"
        "  status               Show localchatd status.\n"
        "  start                Start localchatd via systemd (Linux, requires root).\n"
        "  stop                 Stop localchatd via systemd (Linux, requires root).\n"
        "  restart              Restart localchatd via systemd (Linux, requires root).\n"
        "  logs [-f]            Show recent systemd logs, or follow them (Linux).\n"
        "  uninstall            Remove localchat (requires root).\n"
        "\n"
        "Options:\n"
        "  -s, --socket PATH    Socket path (default: %s).\n"
        "      --color MODE     Color mode: auto, always, never.\n"
        "      --no-color       Disable colors.\n"
        "  -h, --help           Show this help and exit.\n"
        "      --version        Show version and exit.\n"
        "\n"
        "Keys:\n"
        "  Enter         send message\n"
        "  ←/→           move cursor       Home/End  start/end of input\n"
        "  Backspace/Del edit              Ctrl-W    delete word back\n"
        "  Ctrl-U/Ctrl-K clear / kill EOL  Ctrl-L    redraw\n"
        "  ↑/↓           scroll linewise   PgUp/PgDn scroll pagewise\n"
        "  Ctrl-C/D      quit\n"
        "\n"
        "Manual daemon:\n"
        "  localchatd --socket %s\n",
        VERSION, DEFAULT_SOCKET_PATH, DEFAULT_SOCKET_PATH);
}

int main(int argc, char **argv) {
    const char *command = NULL;
    int logs_follow = 0;
    int update_check = 0;

    if (getenv("NO_COLOR")) color_mode = COLOR_NEVER;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (command) {
            if (strcmp(command, "logs") == 0 &&
                (strcmp(a, "-f") == 0 || strcmp(a, "--follow") == 0)) {
                logs_follow = 1;
                continue;
            }
            if (strcmp(command, "update") == 0 && strcmp(a, "--check") == 0) {
                update_check = 1;
                continue;
            }
            fprintf(stderr, "unexpected argument: %s\n", a);
            usage(stderr);
            return 2;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout); return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("%s\n", VERSION); return 0;
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--socket") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing argument for %s\n", a);
                usage(stderr);
                return 2;
            }
            socket_path = argv[++i];
        } else if (strcmp(a, "--no-color") == 0) {
            color_mode = COLOR_NEVER;
        } else if (strncmp(a, "--color=", 8) == 0) {
            if (parse_color_mode(a + 8) != 0) {
                fprintf(stderr, "invalid color mode: %s\n", a + 8);
                usage(stderr);
                return 2;
            }
        } else if (strcmp(a, "--color") == 0) {
            if (i + 1 >= argc || parse_color_mode(argv[i + 1]) != 0) {
                fprintf(stderr, "invalid or missing argument for --color\n");
                usage(stderr);
                return 2;
            }
            i++;
        } else if (is_command(a)) {
            command = a;
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    if (command) {
        if (strcmp(command, "update") == 0) return run_update(update_check);
        if (strcmp(command, "status") == 0) return run_status();
        if (strcmp(command, "start") == 0) return run_service_command("start", 1);
        if (strcmp(command, "stop") == 0) return run_service_command("stop", 1);
        if (strcmp(command, "restart") == 0) return run_service_command("restart", 1);
        if (strcmp(command, "logs") == 0) return run_logs(logs_follow);
        if (strcmp(command, "uninstall") == 0) return run_uninstall();
    }

    setlocale(LC_ALL, "");
    load_local_username();

    sock_fd = connect_server(1);
    if (sock_fd < 0) return 1;
    set_nonblock(sock_fd);

    if (initscr() == NULL) {
        fprintf(stderr, "failed to initialize ncurses\n");
        close(sock_fd);
        return 1;
    }
    cbreak();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    curs_set(1);
    init_color_pairs();

    getmaxyx(stdscr, term_h, term_w);
    chat_h = term_h - 4;
    if (chat_h < 1) chat_h = 1;
    status_win = newwin(1, term_w, 0, 0);
    chat_win  = newwin(chat_h, term_w, 1, 0);
    input_win = newwin(3, term_w, chat_h + 1, 0);
    if (!status_win || !chat_win || !input_win) {
        endwin();
        close(sock_fd);
        fprintf(stderr, "failed to create ncurses windows\n");
        return 1;
    }
    scrollok(chat_win, FALSE);
    keypad(input_win, TRUE);
    nodelay(input_win, TRUE);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,   &sa, NULL);
    sigaction(SIGTERM,  &sa, NULL);
    sigaction(SIGHUP,   &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    render_system("[system] connected to localchat");
    redraw_all();

    while (!exit_requested) {
        if (resize_pending) {
            resize_pending = 0;
            handle_resize();
        }

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        int stdin_slot = -1;
        int sock_slot = -1;

        stdin_slot = (int)nfds;
        pfds[nfds].fd = STDIN_FILENO;
        pfds[nfds].events = POLLIN;
        nfds++;

        if (sock_fd >= 0) {
            sock_slot = (int)nfds;
            pfds[nfds].fd = sock_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int r = poll(pfds, nfds, sock_fd >= 0 ? -1 : 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (r == 0) {
            try_reconnect();
            redraw_all();
            continue;
        }

        if (stdin_slot >= 0 && (pfds[stdin_slot].revents & POLLIN)) process_keys();

        if (sock_slot >= 0 && (pfds[sock_slot].revents & (POLLIN | POLLHUP))) {
            if (handle_socket_in() != 0) mark_disconnected();
        }
        if (sock_slot >= 0 && sock_fd >= 0 && (pfds[sock_slot].revents & (POLLERR | POLLNVAL))) {
            mark_disconnected();
        }

        if (sock_fd < 0) try_reconnect();
        redraw_all();
    }

    endwin();
    close_server_connection();
    for (int i = 0; i < LOG_CAP; i++) free(log_lines[i]);
    return 0;
}
