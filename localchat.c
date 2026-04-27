#include <sys/socket.h>
#include <sys/un.h>

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCKET_PATH "/run/localchat.sock"
#define BUFFER_SIZE 1024

static int sock_fd;

static void *receive_loop(void *arg) {
    (void)arg;

    char buffer[BUFFER_SIZE];

    while (1) {
        ssize_t n = read(sock_fd, buffer, sizeof(buffer) - 1);

        if (n <= 0) {
            printf("\n[system] disconnected\n");
            exit(0);
        }

        buffer[n] = '\0';
        printf("%s", buffer);
        fflush(stdout);
    }

    return NULL;
}

int main(void) {
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

    pthread_t thread;
    if (pthread_create(&thread, NULL, receive_loop, NULL) != 0) {
        perror("pthread_create");
        close(sock_fd);
        return 1;
    }

    char input[BUFFER_SIZE];

    while (fgets(input, sizeof(input), stdin) != NULL) {
        if (strcmp(input, "/quit\n") == 0) {
            break;
        }

        write(sock_fd, input, strlen(input));
    }

    close(sock_fd);

    return 0;
}