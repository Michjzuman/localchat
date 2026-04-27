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
#include <signal.h>
#include <arpa/inet.h>

#define SOCKET_PATH "/run/localchat.sock"
#define MAX_CLIENTS 64
#define BUFFER_SIZE 1024

typedef struct {
    int fd;
    char username[64];
    char recv_buf[BUFFER_SIZE];
    size_t recv_len;
} Client;

static Client clients[MAX_CLIENTS];

static volatile sig_atomic_t keep_running = 1;

static void handle_shutdown(int sig) {
    (void)sig;
    keep_running = 0;
}

// Helper to send a length‑prefixed message to a client socket
static ssize_t safe_write(int fd, const void *buf, size_t count); // forward declaration
static int send_message_fd(int fd, const char *message) {
    size_t len = strlen(message);
    uint32_t netlen = htonl((uint32_t)len);
    if (safe_write(fd, &netlen, sizeof(netlen)) == -1) {
        return -1;
    }
    if (safe_write(fd, message, len) == -1) {
        return -1;
    }
    return 0;
}


static ssize_t safe_write(int fd, const void *buf, size_t count) {
    size_t total = 0;
    const char *ptr = (const char *)buf;
    while (total < count) {
        ssize_t n = write(fd, ptr + total, count - total);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1; // error
        }
        total += n;
    }
    return (ssize_t)total;
}

static void broadcast_message(const char *message, int except_fd) {
    size_t len = strlen(message);
    int needs_newline = (len == 0 || message[len - 1] != '\n');
    // Prepare payload (include newline if needed)
    char tmp[BUFFER_SIZE + 2];
    const char *payload = message;
    size_t payload_len = len;
    if (needs_newline) {
        if (len + 1 < sizeof(tmp)) {
            memcpy(tmp, message, len);
            tmp[len] = '\n';
            payload = tmp;
            payload_len = len + 1;
        }
        // else fallback: send without newline
    }
    uint32_t netlen = htonl((uint32_t)payload_len);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 && clients[i].fd != except_fd) {
            // send length prefix
            if (safe_write(clients[i].fd, &netlen, sizeof(netlen)) == -1) {
                continue; // error, client will be handled later
            }
            if (safe_write(clients[i].fd, payload, payload_len) == -1) {
                continue;
            }
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
#if defined(__linux__)
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) {
        static char fallback[64];
        snprintf(fallback, sizeof(fallback), "unknown");
        return fallback;
    }

    struct passwd *pw = getpwuid(cred.uid);
    if (pw == NULL) {
        static char fallback[64];
        snprintf(fallback, sizeof(fallback), "uid-%d", cred.uid);
        return fallback;
    }
    return pw->pw_name;
#else
    (void)fd; // unused parameter on non‑Linux platforms
    return "unknown";
#endif
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

    // Install signal handlers for graceful shutdown and ignore SIGPIPE
    signal(SIGTERM, handle_shutdown);
    signal(SIGINT, handle_shutdown);
    signal(SIGPIPE, SIG_IGN);

    while (keep_running) {
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
                send_message_fd(client_fd, full_msg);
                close(client_fd);
                continue;
            }

            clients[slot].fd = client_fd;
            snprintf(clients[slot].username, sizeof(clients[slot].username), "%s", get_username_from_fd(client_fd));

            char msg[256];
            snprintf(msg, sizeof(msg), "[system] %s joined\n", clients[slot].username);
            broadcast_message(msg, -1);

            const char *welcome = "[system] welcome to localchat\n";
            send_message_fd(client_fd, welcome);
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1) {
                continue;
            }

            if (fds[i + 1].revents & POLLIN) {
                // Read a length‑prefixed message from the client
                char buffer[BUFFER_SIZE];
                uint32_t netlen;
                ssize_t n = read(clients[i].fd, &netlen, sizeof(netlen));
                if (n <= 0) {
                    remove_client(i);
                    continue;
                }
                while (n < (ssize_t)sizeof(netlen)) {
                    ssize_t m = read(clients[i].fd, ((char *)&netlen) + n, sizeof(netlen) - n);
                    if (m <= 0) {
                        remove_client(i);
                        continue;
                    }
                    n += m;
                }
                uint32_t msg_len = ntohl(netlen);
                // Read the message payload
                if (msg_len > BUFFER_SIZE - 1) {
                    // Truncate to buffer size and discard the rest
                    size_t to_read = BUFFER_SIZE - 1;
                    size_t total = 0;
                    while (total < to_read) {
                        ssize_t m = read(clients[i].fd, buffer + total, to_read - total);
                        if (m <= 0) {
                            remove_client(i);
                            goto after_read; // break out
                        }
                        total += m;
                    }
                    // discard remaining bytes
                    size_t remaining = msg_len - to_read;
                    char discard[1024];
                    while (remaining > 0) {
                        ssize_t m = read(clients[i].fd, discard, remaining > sizeof(discard) ? sizeof(discard) : remaining);
                        if (m <= 0) {
                            remove_client(i);
                            break;
                        }
                        remaining -= m;
                    }
                    buffer[to_read] = '\0';
                } else {
                    size_t total = 0;
                    while (total < msg_len) {
                        ssize_t m = read(clients[i].fd, buffer + total, msg_len - total);
                        if (m <= 0) {
                            remove_client(i);
                            goto after_read;
                        }
                        total += m;
                    }
                    buffer[msg_len] = '\0';
                }
                // Build the formatted chat message and broadcast it
                {
                    char msg[BUFFER_SIZE + 128];
                    snprintf(msg, sizeof(msg), "[%s] %s", clients[i].username, buffer);
                    broadcast_message(msg, -1);
                }
                after_read: ;
            } else if (fds[i + 1].revents & (POLLHUP | POLLERR)) {
                // Connection closed or error; remove client
                remove_client(i);
                continue;
            }
    }
    }
    close(server_fd);
    unlink(SOCKET_PATH);

    return 0;
}
