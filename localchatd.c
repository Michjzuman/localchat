/*
 * localchatd - localchat server daemon
 *
 * Listens on a UNIX domain socket, identifies clients via SO_PEERCRED, and
 * forwards length-prefixed text messages between connected users.
 *
 * Wire format: 4-byte big-endian payload length, followed by payload bytes.
 * Server -> client payloads are formatted as "[username] body" or
 * "[system] body" (no trailing newline).
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if !defined(__linux__)
#  error "localchatd requires Linux (SO_PEERCRED, struct ucred)."
#endif

#define VERSION             "1.1.0"
#define DEFAULT_SOCKET_PATH "/run/localchat/socket"
#define LEGACY_SOCKET_PATH  "/run/localchat.sock"
#define MAX_CLIENTS         64
#define MAX_MSG_LEN         8192
#define MAX_BODY_LEN        4096
#define LEN_PREFIX_BYTES    4
#define USERNAME_MAX        32
#define IN_BUF_CAP          (LEN_PREFIX_BYTES + MAX_MSG_LEN)
#define OUT_BUF_CAP         (64 * 1024)

typedef struct {
    int            fd;
    char           username[USERNAME_MAX];
    unsigned char  in_buf[IN_BUF_CAP];
    size_t         in_len;
    unsigned char *out_buf;
    size_t         out_len;
    size_t         out_cap;
} Client;

static Client clients[MAX_CLIENTS];
static int    wakeup_pipe[2] = {-1, -1};
static volatile sig_atomic_t shutdown_requested = 0;

static const char *socket_path = DEFAULT_SOCKET_PATH;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static void on_signal(int sig) {
    (void)sig;
    shutdown_requested = 1;
    if (wakeup_pipe[1] >= 0) {
        char b = 1;
        ssize_t r;
        do { r = write(wakeup_pipe[1], &b, 1); } while (r < 0 && errno == EINTR);
        (void)r;
    }
}

static int set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static int set_cloexec(int fd) {
    int f = fcntl(fd, F_GETFD, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

static void sanitize_text(char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') continue;
        if (c < 0x20 || c == 0x7f) s[i] = ' ';
    }
}

static void sanitize_username(char *s) {
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || c == '[' || c == ']' || c == ' ') {
            s[i] = '_';
        }
    }
}

static void resolve_username(int fd, char *out, size_t cap) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) {
        snprintf(out, cap, "unknown");
    } else {
        struct passwd *pw = getpwuid(cred.uid);
        if (pw && pw->pw_name && pw->pw_name[0]) {
            snprintf(out, cap, "%s", pw->pw_name);
        } else {
            snprintf(out, cap, "uid-%u", (unsigned)cred.uid);
        }
    }
    sanitize_username(out);
}

static int queue_bytes(Client *c, const void *data, size_t n) {
    if (n == 0) return 0;
    if (c->out_len + n > OUT_BUF_CAP) return -1;
    if (c->out_cap < c->out_len + n) {
        size_t cap = c->out_cap ? c->out_cap : 256;
        while (cap < c->out_len + n) cap *= 2;
        if (cap > OUT_BUF_CAP) cap = OUT_BUF_CAP;
        unsigned char *nb = realloc(c->out_buf, cap);
        if (!nb) return -1;
        c->out_buf = nb;
        c->out_cap = cap;
    }
    memcpy(c->out_buf + c->out_len, data, n);
    c->out_len += n;
    return 0;
}

static int queue_msg(Client *c, const char *text) {
    size_t len = strlen(text);
    if (len > MAX_MSG_LEN) len = MAX_MSG_LEN;
    uint32_t netlen = htonl((uint32_t)len);
    if (queue_bytes(c, &netlen, sizeof(netlen)) != 0) return -1;
    return queue_bytes(c, text, len);
}

static void disconnect_client(int idx, const char *reason);

static void broadcast_text(const char *text, int except_idx) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == -1 || i == except_idx) continue;
        if (queue_msg(&clients[i], text) != 0) {
            disconnect_client(i, "send buffer full");
        }
    }
}

static void disconnect_client(int idx, const char *reason) {
    Client *c = &clients[idx];
    if (c->fd == -1) return;

    char left[USERNAME_MAX];
    snprintf(left, sizeof(left), "%s", c->username);

    close(c->fd);
    c->fd = -1;
    free(c->out_buf);
    c->out_buf = NULL;
    c->out_len = 0;
    c->out_cap = 0;
    c->in_len  = 0;
    c->username[0] = '\0';

    if (left[0]) {
        char msg[USERNAME_MAX + 64];
        if (reason) {
            snprintf(msg, sizeof(msg), "[system] %s left (%s)", left, reason);
        } else {
            snprintf(msg, sizeof(msg), "[system] %s left", left);
        }
        log_msg("client disconnected: %s%s%s%s",
                left,
                reason ? " (" : "",
                reason ? reason : "",
                reason ? ")"  : "");
        broadcast_text(msg, idx);
    }
}

static int process_in_buffer(int idx) {
    Client *c = &clients[idx];
    while (1) {
        if (c->in_len < LEN_PREFIX_BYTES) return 0;
        uint32_t netlen;
        memcpy(&netlen, c->in_buf, LEN_PREFIX_BYTES);
        uint32_t mlen = ntohl(netlen);
        if (mlen > MAX_BODY_LEN) {
            disconnect_client(idx, "oversized message");
            return -1;
        }
        if (c->in_len < LEN_PREFIX_BYTES + mlen) return 0;

        char body[MAX_BODY_LEN + 1];
        memcpy(body, c->in_buf + LEN_PREFIX_BYTES, mlen);
        body[mlen] = '\0';

        size_t consumed = LEN_PREFIX_BYTES + mlen;
        memmove(c->in_buf, c->in_buf + consumed, c->in_len - consumed);
        c->in_len -= consumed;

        sanitize_text(body, mlen);
        size_t blen = mlen;
        while (blen > 0 && (body[blen - 1] == ' ' || body[blen - 1] == '\n' ||
                            body[blen - 1] == '\r' || body[blen - 1] == '\t')) {
            body[--blen] = '\0';
        }
        size_t bstart = 0;
        while (bstart < blen && (body[bstart] == ' ' || body[bstart] == '\t')) {
            bstart++;
        }
        if (blen - bstart == 0) continue;

        char out[USERNAME_MAX + MAX_BODY_LEN + 8];
        snprintf(out, sizeof(out), "[%s] %s", c->username, body + bstart);
        broadcast_text(out, -1);
    }
}

static int handle_read(int idx) {
    Client *c = &clients[idx];
    while (1) {
        if (c->in_len >= IN_BUF_CAP) {
            disconnect_client(idx, "buffer overflow");
            return -1;
        }
        ssize_t n = read(c->fd, c->in_buf + c->in_len, IN_BUF_CAP - c->in_len);
        if (n > 0) {
            c->in_len += (size_t)n;
            if (process_in_buffer(idx) != 0) return -1;
            continue;
        }
        if (n == 0) {
            disconnect_client(idx, NULL);
            return -1;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        disconnect_client(idx, strerror(errno));
        return -1;
    }
}

static int handle_write(int idx) {
    Client *c = &clients[idx];
    while (c->out_len > 0) {
        ssize_t n = write(c->fd, c->out_buf, c->out_len);
        if (n > 0) {
            memmove(c->out_buf, c->out_buf + n, c->out_len - (size_t)n);
            c->out_len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        disconnect_client(idx, n < 0 ? strerror(errno) : "write returned 0");
        return -1;
    }
    return 0;
}

static void send_immediately(int fd, const char *text) {
    size_t len = strlen(text);
    if (len > MAX_MSG_LEN) len = MAX_MSG_LEN;
    uint32_t netlen = htonl((uint32_t)len);
    unsigned char buf[LEN_PREFIX_BYTES + 256];
    if (len + LEN_PREFIX_BYTES > sizeof(buf)) return;
    memcpy(buf, &netlen, LEN_PREFIX_BYTES);
    memcpy(buf + LEN_PREFIX_BYTES, text, len);
    size_t total = LEN_PREFIX_BYTES + len;
    size_t sent = 0;
    int spins = 0;
    while (sent < total) {
        ssize_t n = write(fd, buf + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && spins++ < 50) {
                struct pollfd p = { .fd = fd, .events = POLLOUT };
                if (poll(&p, 1, 100) <= 0) return;
                continue;
            }
            return;
        }
        if (n == 0) return;
        sent += (size_t)n;
    }
}

static void accept_one(int server_fd) {
    while (1) {
        int cfd = accept(server_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            log_msg("accept: %s", strerror(errno));
            return;
        }
        set_nonblock(cfd);
        set_cloexec(cfd);

        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1) { slot = i; break; }
        }
        if (slot < 0) {
            send_immediately(cfd, "[system] server full, try again later");
            close(cfd);
            continue;
        }

        Client *c = &clients[slot];
        memset(c, 0, sizeof(*c));
        c->fd = cfd;
        resolve_username(cfd, c->username, sizeof(c->username));

        queue_msg(c, "[system] welcome to localchat");

        char join[USERNAME_MAX + 32];
        snprintf(join, sizeof(join), "[system] %s joined", c->username);
        broadcast_text(join, slot);

        log_msg("client connected: %s (slot %d)", c->username, slot);
    }
}

static int setup_socket(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    set_cloexec(fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        log_msg("socket path too long: %s", socket_path);
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, socket_path);

    if (unlink(socket_path) == -1 && errno != ENOENT) {
        perror("unlink");
        close(fd);
        return -1;
    }

    /* Set umask so the socket is created with permissions 0666 atomically. */
    mode_t old_mask = umask(0111);
    int br = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    int berr = errno;
    umask(old_mask);
    if (br == -1) {
        errno = berr;
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) == -1) {
        perror("listen");
        close(fd);
        unlink(socket_path);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void usage(FILE *out) {
    fprintf(out,
        "localchatd %s\n"
        "Usage: localchatd [OPTIONS]\n"
        "  -s, --socket PATH   socket path (default: %s)\n"
        "  -h, --help          show this help\n"
        "      --version       show version\n",
        VERSION, DEFAULT_SOCKET_PATH);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("%s\n", VERSION);
            return 0;
        } else if ((strcmp(a, "-s") == 0 || strcmp(a, "--socket") == 0) && i + 1 < argc) {
            socket_path = argv[++i];
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    if (pipe(wakeup_pipe) == -1) { perror("pipe"); return 1; }
    set_nonblock(wakeup_pipe[0]);
    set_nonblock(wakeup_pipe[1]);
    set_cloexec(wakeup_pipe[0]);
    set_cloexec(wakeup_pipe[1]);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int server_fd = setup_socket();
    if (server_fd < 0) return 1;

    log_msg("localchatd %s listening on %s", VERSION, socket_path);

    while (!shutdown_requested) {
        struct pollfd fds[MAX_CLIENTS + 2];
        int slot_to_idx[MAX_CLIENTS + 2];
        nfds_t n = 0;

        fds[n].fd = server_fd;     fds[n].events = POLLIN; slot_to_idx[n] = -1; n++;
        fds[n].fd = wakeup_pipe[0]; fds[n].events = POLLIN; slot_to_idx[n] = -1; n++;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1) continue;
            fds[n].fd = clients[i].fd;
            fds[n].events = POLLIN | (clients[i].out_len > 0 ? POLLOUT : 0);
            slot_to_idx[n] = i;
            n++;
        }

        int r = poll(fds, n, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (fds[0].revents & POLLIN) accept_one(server_fd);
        if (fds[1].revents & POLLIN) {
            char drain[64];
            while (read(wakeup_pipe[0], drain, sizeof(drain)) > 0) { /* drain */ }
        }

        for (nfds_t k = 2; k < n; k++) {
            int idx = slot_to_idx[k];
            if (idx < 0 || clients[idx].fd == -1) continue;

            short re = fds[k].revents;

            if (re & (POLLERR | POLLNVAL)) {
                disconnect_client(idx, "socket error");
                continue;
            }
            if (re & (POLLIN | POLLHUP)) {
                if (handle_read(idx) != 0) continue;
            }
            if (clients[idx].fd != -1 && (re & POLLOUT)) {
                if (handle_write(idx) != 0) continue;
            }
        }
    }

    log_msg("shutting down");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            queue_msg(&clients[i], "[system] server shutting down");
            handle_write(i);
            disconnect_client(i, "server shutdown");
        }
    }
    close(server_fd);
    unlink(socket_path);
    close(wakeup_pipe[0]);
    close(wakeup_pipe[1]);
    return 0;
}
