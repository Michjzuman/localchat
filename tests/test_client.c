/*
 * tests/test_client.c
 *
 * Minimal smoke-test client. Connects to a localchatd socket, expects a
 * "[system] welcome" message, sends a body, and verifies the broadcast echo.
 *
 * Usage: test_client SOCKET_PATH MESSAGE
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, (const char *)buf + put, n - put);
        if (r > 0) { put += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int read_msg(int fd, char *buf, size_t cap) {
    uint32_t netlen;
    if (read_full(fd, &netlen, 4) != 0) return -1;
    uint32_t mlen = ntohl(netlen);
    if (mlen >= cap) return -1;
    if (read_full(fd, buf, mlen) != 0) return -1;
    buf[mlen] = '\0';
    return (int)mlen;
}

static int send_msg(int fd, const char *text) {
    size_t len = strlen(text);
    uint32_t netlen = htonl((uint32_t)len);
    if (write_full(fd, &netlen, 4) != 0) return -1;
    if (write_full(fd, text, len) != 0) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s SOCKET MESSAGE\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const char *msg  = argv[2];

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long\n");
        return 1;
    }
    strcpy(addr.sun_path, path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect %s: %s\n", path, strerror(errno));
        return 1;
    }

    char buf[8192];
    int n = read_msg(fd, buf, sizeof(buf));
    if (n < 0) { fprintf(stderr, "no welcome\n"); return 1; }
    if (strstr(buf, "[system] welcome") == NULL) {
        fprintf(stderr, "unexpected welcome: %s\n", buf);
        return 1;
    }
    fprintf(stderr, "welcome: %s\n", buf);

    if (send_msg(fd, msg) != 0) { perror("send"); return 1; }

    /* The server may emit further frames before/after the echo (e.g. join
     * notices from prior connects). Scan up to 5 frames for the echo. */
    int found = 0;
    for (int i = 0; i < 5 && !found; i++) {
        n = read_msg(fd, buf, sizeof(buf));
        if (n < 0) break;
        fprintf(stderr, "recv:  %s\n", buf);
        if (strstr(buf, msg) != NULL && buf[0] == '[' && buf[1] != 's') {
            found = 1;
        }
    }
    close(fd);
    if (!found) { fprintf(stderr, "echo not received\n"); return 1; }
    printf("OK\n");
    return 0;
}
