#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#include <pwd.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>

#define SOCKET_PATH "/run/localchat.sock"
#define BUFFER_SIZE 1024
#define USERNAME_SIZE 64

static int sock_fd;
static WINDOW *chat_win;
static WINDOW *input_win;
static pthread_mutex_t win_mutex = PTHREAD_MUTEX_INITIALIZER;
static char local_username[USERNAME_SIZE];

static void repeat_str(WINDOW *win, const char *str, int count) {
    for (int i = 0; i < count; i++) {
        waddstr(win, str);
    }
}

static void print_spaces(WINDOW *win, int count) {
    for (int i = 0; i < count; i++) {
        waddch(win, ' ');
    }
}

static int text_columns(const char *text) {
    return (int)strlen(text);
}

static int max_message_columns(void) {
    int height, width;
    getmaxyx(chat_win, height, width);
    (void)height;

    if (width > 12) {
        return width - 8;
    }
    if (width > 6) {
        return width - 5;
    }
    return 1;
}

static void trim_line(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
}

static void load_local_username(void) {
    struct passwd *pw = getpwuid(getuid());
    if (pw != NULL && pw->pw_name != NULL) {
        snprintf(local_username, sizeof(local_username), "%s", pw->pw_name);
        return;
    }

    snprintf(local_username, sizeof(local_username), "uid-%ld", (long)getuid());
}

static void render_system_message(const char *message) {
    int height, width;
    getmaxyx(chat_win, height, width);
    (void)height;

    int line_width = text_columns(message);
    int left_padding = (width - line_width) / 2;
    if (left_padding < 0) {
        left_padding = 0;
    }

    wattron(chat_win, A_DIM);
    print_spaces(chat_win, left_padding);
    wprintw(chat_win, "%s\n", message);
    wattroff(chat_win, A_DIM);
}

static void render_one_bubble(const char *name, const char *message, int own_message) {
    int height, width;
    getmaxyx(chat_win, height, width);
    (void)height;

    int message_width = text_columns(message);
    int bubble_width = message_width + 5;

    if (own_message) {
        int name_width = text_columns(name) + 3;
        print_spaces(chat_win, width > name_width ? width - name_width : 0);
        wprintw(chat_win, "%s   \n", name);

        print_spaces(chat_win, width > bubble_width ? width - bubble_width : 0);
        waddstr(chat_win, "╭─");
        repeat_str(chat_win, "─", message_width);
        waddstr(chat_win, "─╮ \n");

        print_spaces(chat_win, width > bubble_width ? width - bubble_width : 0);
        wprintw(chat_win, "│ %s │❯\n", message);

        print_spaces(chat_win, width > bubble_width ? width - bubble_width : 0);
        waddstr(chat_win, "╰─");
        repeat_str(chat_win, "─", message_width);
        waddstr(chat_win, "─╯ \n");
        return;
    }

    wprintw(chat_win, "  %s\n", name);
    waddstr(chat_win, " ╭─");
    repeat_str(chat_win, "─", message_width);
    waddstr(chat_win, "─╮\n");
    wprintw(chat_win, "❮│ %s │\n", message);
    waddstr(chat_win, " ╰─");
    repeat_str(chat_win, "─", message_width);
    waddstr(chat_win, "─╯\n");
}

static void render_bubble(const char *name, const char *message, int own_message) {
    int max_columns = max_message_columns();
    size_t message_len = strlen(message);

    if (message_len == 0) {
        render_one_bubble(name, "", own_message);
        return;
    }

    for (size_t offset = 0; offset < message_len; offset += (size_t)max_columns) {
        size_t chunk_len = message_len - offset;
        if (chunk_len > (size_t)max_columns) {
            chunk_len = (size_t)max_columns;
        }

        char chunk[BUFFER_SIZE];
        if (chunk_len >= sizeof(chunk)) {
            chunk_len = sizeof(chunk) - 1;
        }
        memcpy(chunk, message + offset, chunk_len);
        chunk[chunk_len] = '\0';

        render_one_bubble(name, chunk, own_message);
    }
}

static void render_chat_message(const char *raw_message) {
    char line[BUFFER_SIZE];
    snprintf(line, sizeof(line), "%s", raw_message);
    trim_line(line);

    if (line[0] == '\0') {
        return;
    }

    if (strncmp(line, "[system]", 8) == 0) {
        render_system_message(line);
        return;
    }

    if (line[0] == '[') {
        char *name_end = strchr(line + 1, ']');
        if (name_end != NULL) {
            *name_end = '\0';
            const char *name = line + 1;
            const char *message = name_end + 1;
            if (*message == ' ') {
                message++;
            }

            render_bubble(name, message, strcmp(name, local_username) == 0);
            return;
        }
    }

    render_system_message(line);
}

static void render_received_text(char *buffer) {
    char *line = buffer;

    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next = '\0';
        }

        render_chat_message(line);

        if (next == NULL) {
            break;
        }
        line = next + 1;
    }
}

static void draw_input_box(const char *input, int cursor_index) {
    int height, width;
    getmaxyx(input_win, height, width);
    (void)height;

    werase(input_win);

    if (width < 6) {
        box(input_win, 0, 0);
        wrefresh(input_win);
        return;
    }

    mvwaddstr(input_win, 0, 0, "╭");
    repeat_str(input_win, "─", width - 2);
    waddstr(input_win, "╮");

    mvwaddstr(input_win, 1, 0, "│");
    print_spaces(input_win, width - 2);
    mvwaddstr(input_win, 1, width - 3, "⏎");
    mvwaddstr(input_win, 1, width - 1, "│");

    mvwaddstr(input_win, 2, 0, "╰");
    repeat_str(input_win, "─", width - 2);
    waddstr(input_win, "╯");

    int input_width = width - 5;
    if (input_width < 1) {
        input_width = 1;
    }

    int offset = cursor_index > input_width ? cursor_index - input_width : 0;
    mvwprintw(input_win, 1, 2, "%.*s", input_width, input + offset);

    int cursor_x = 2 + cursor_index - offset;
    if (cursor_x > width - 4) {
        cursor_x = width - 4;
    }
    wmove(input_win, 1, cursor_x);
    wrefresh(input_win);
}

static void *receive_loop(void *arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];
    while (1) {
        ssize_t n = read(sock_fd, buffer, BUFFER_SIZE - 1);
        if (n <= 0) {
            pthread_mutex_lock(&win_mutex);
            render_system_message("[system] disconnected");
            wrefresh(chat_win);
            pthread_mutex_unlock(&win_mutex);
            endwin();
            exit(0);
        }
        buffer[n] = '\0';
        pthread_mutex_lock(&win_mutex);
        render_received_text(buffer);
        wrefresh(chat_win);
        pthread_mutex_unlock(&win_mutex);
    }
    return NULL;
}

int main(void) {
    setlocale(LC_ALL, "");
    load_local_username();

    // Connect to the server
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sock_fd);
        return 1;
    }

    // Initialize ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    int height, width;
    getmaxyx(stdscr, height, width);

    int chat_height = height - 3;
    chat_win = newwin(chat_height, width, 0, 0);
    input_win = newwin(3, width, chat_height, 0);
    if (chat_win == NULL || input_win == NULL) {
        endwin();
        close(sock_fd);
        fprintf(stderr, "failed to create ncurses windows\n");
        return 1;
    }

    scrollok(chat_win, TRUE);
    keypad(input_win, TRUE);

    // Print welcome message
    pthread_mutex_lock(&win_mutex);
    render_system_message("[system] connected to localchat");
    wrefresh(chat_win);
    pthread_mutex_unlock(&win_mutex);

    // Start receive thread
    pthread_t thread;
    if (pthread_create(&thread, NULL, receive_loop, NULL) != 0) {
        perror("pthread_create");
        endwin();
        return 1;
    }

    // Input loop
    char input[BUFFER_SIZE];
    input[0] = '\0';
    int idx = 0;
    int ch;
    while (1) {
        // Draw input box and current input
        pthread_mutex_lock(&win_mutex);
        draw_input_box(input, idx);
        pthread_mutex_unlock(&win_mutex);

        ch = wgetch(input_win);
        if (ch == '\n' || ch == '\r') {
            input[idx] = '\0';
            if (idx > 0) {
                if (write(sock_fd, input, strlen(input)) == -1) {
                    pthread_mutex_lock(&win_mutex);
                    render_system_message("[system] failed to send message");
                    wrefresh(chat_win);
                    pthread_mutex_unlock(&win_mutex);
                }
            }
            idx = 0;
            input[0] = '\0';
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (idx > 0) {
                idx--;
                input[idx] = '\0';
            }
        } else if (ch >= 32 && ch < 127 && idx < BUFFER_SIZE - 1) {
            input[idx++] = (char)ch;
            input[idx] = '\0';
        }
    }

    // Cleanup (unreachable in this loop)
    endwin();
    close(sock_fd);
    return 0;
}
