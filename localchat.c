#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>

#define SOCKET_PATH "/run/localchat.sock"
#define BUFFER_SIZE 1024

static int sock_fd;
static WINDOW *chat_win;
static WINDOW *input_win;
static pthread_mutex_t win_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *receive_loop(void *arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];
    while (1) {
        ssize_t n = read(sock_fd, buffer, BUFFER_SIZE - 1);
        if (n <= 0) {
            pthread_mutex_lock(&win_mutex);
            wprintw(chat_win, "\n[system] disconnected\n");
            wrefresh(chat_win);
            pthread_mutex_unlock(&win_mutex);
            endwin();
            exit(0);
        }
        buffer[n] = '\0';
        pthread_mutex_lock(&win_mutex);
        wprintw(chat_win, "%s\n", buffer);
        wrefresh(chat_win);
        pthread_mutex_unlock(&win_mutex);
    }
    return NULL;
}

int main(void) {
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
    scrollok(chat_win, TRUE);

    // Print welcome message
    pthread_mutex_lock(&win_mutex);
    wprintw(chat_win, "[system] connected to localchat\n");
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
        werase(input_win);
        box(input_win, 0, 0);
        mvwprintw(input_win, 1, 2, "%s", input);
        wmove(input_win, 1, 2 + idx);
        wrefresh(input_win);

        ch = wgetch(input_win);
        if (ch == '\n' || ch == '\r') {
            input[idx] = '\0';
            if (idx > 0) {
                write(sock_fd, input, strlen(input));
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
