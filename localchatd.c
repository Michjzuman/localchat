#define _GNU_SOURCE

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/stat.h>

#include <pwd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SOCKET_PATH "/run/localchat.sock"
#define MAX_CLIENTS 64
#define BUFFER_SIZE 1024

typedef struct {
    int fd;
    char username[64];
} Client;

static Client clients[MAX_CLIENTS];

static void broadcast_message(const char *message, int except_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 && clients[i].fd != except_fd) {
            write(clients[i].fd, message, strlen(message));
        }
    }
}

static void remove_client(int index) {
    if (clients[index].fd != -1) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[system] %s left\n", clients[index].username);
        broadcast_message(msg, clients[index].fd);

        close(clients[index].fd);
        clients[index].fd = -1;
        clients[index].username[0] = '\0';
    }
}

static const char *get_username_from_fd(int fd) {
    static char fallback[64];

    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) {
        snprintf(fallback, sizeof(fallback), "unknown");
        return fallback;
    }

    struct passwd *pw = getpwuid(cred.uid);
    if (pw == NULL) {
        snprintf(fallback, sizeof(fallback), "uid-%d", cred.uid);
        return fallback;
    }

    return pw->pw_name;
}

int main(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
    }

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return 1;
    }

    unlink(SOCKET_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    chmod(SOCKET_PATH, 0666);

    if (listen(server_fd, 16) == -1) {
        perror("listen");
        close(server_fd);
        unlink(SOCKET_PATH);
        return 1;
    }

    printf("localchatd läuft auf %s\n", SOCKET_PATH);

    while (1) {
        struct pollfd fds[MAX_CLIENTS + 1];

        fds[0].fd = server_fd;
        fds[0].events = POLLIN;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            fds[i + 1].fd = clients[i].fd;
            fds[i + 1].events = clients[i].fd == -1 ? 0 : POLLIN;
        }

        int ready = poll(fds, MAX_CLIENTS + 1, -1);
        if (ready == -1) {
            perror("poll");
            break;
        }

        if (fds[0].revents & POLLIN) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd == -1) {
                perror("accept");
                continue;
            }

            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].fd == -1) {
                    slot = i;
                    break;
                }
            }

            if (slot == -1) {
                const char *full_msg = "[system] server full\n";
                write(client_fd, full_msg, strlen(full_msg));
                close(client_fd);
                continue;
            }

            clients[slot].fd = client_fd;
            snprintf(clients[slot].username, sizeof(clients[slot].username), "%s", get_username_from_fd(client_fd));

            char msg[256];
            snprintf(msg, sizeof(msg), "[system] %s joined\n", clients[slot].username);
            broadcast_message(msg, -1);

            const char *welcome = "[system] welcome to localchat\n";
            write(client_fd, welcome, strlen(welcome));
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1) {
                continue;
            }

            if (fds[i + 1].revents & POLLIN) {
                char buffer[BUFFER_SIZE];
                ssize_t n = read(clients[i].fd, buffer, sizeof(buffer) - 1);

                if (n <= 0) {
                    remove_client(i);
                    continue;
                }

                buffer[n] = '\0';

                char msg[BUFFER_SIZE + 128];
                snprintf(msg, sizeof(msg), "[%s] %s", clients[i].username, buffer);

                broadcast_message(msg, -1);
            }
        }
    }

    close(server_fd);
    unlink(SOCKET_PATH);

    return 0;
}