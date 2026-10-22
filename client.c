#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* Set by the signal handler, read by the main loop.
 * volatile sig_atomic_t is the ONLY type the C standard guarantees can be
 * safely written from an asynchronous signal handler. */
static volatile sig_atomic_t g_signal = 0;

/* Async-signal-safe handler: only records which signal arrived.
 * No printf / malloc / close here - those are not async-signal-safe. */
static void on_signal(int sig) {
    g_signal = sig;
}

static int install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART on purpose: we WANT poll() to return -1/EINTR so the main
     * loop notices g_signal immediately. */
    sa.sa_flags = 0;

    int sigs[] = { SIGINT, SIGTERM, SIGQUIT, SIGHUP };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (sigaction(sigs[i], &sa, NULL) == -1) {
            perror("sigaction");
            return -1;
        }
    }

    /* Server vanished while we write -> get EPIPE instead of being killed. */
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGPIPE, &ign, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        return -1;
    }
    return 0;
}

static int send_all(int sock, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_signal) return -1;
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : PORT;
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Usage: %s [server-ip] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (install_signal_handlers() != 0) return EXIT_FAILURE;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;                                    /* macOS */
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP address: %s\n", host);
        close(sock);
        return EXIT_FAILURE;
    }

    while (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == EINTR && !g_signal) continue;
        perror("Connection failed");
        fprintf(stderr, "Is the server running on %s:%d ?  (start it with ./server)\n",
                host, port);
        close(sock);
        return EXIT_FAILURE;
    }

    printf("Connected to server %s:%d  (Ctrl+C to quit)\n\n", host, port);
    setvbuf(stdout, NULL, _IONBF, 0);

    char buffer[BUFFER_SIZE];
    char input[BUFFER_SIZE];
    struct pollfd fds[2];
    fds[0].fd = sock;          fds[0].events = POLLIN;
    fds[1].fd = STDIN_FILENO;  fds[1].events = POLLIN;

    const char *reason = NULL;

    while (!g_signal) {
        int r = poll(fds, 2, -1);              /* block until data or signal */
        if (r < 0) {
            if (errno == EINTR) continue;      /* loop condition checks g_signal */
            perror("poll");
            break;
        }

        /* Data (or hang-up) from the server */
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (n == 0) { reason = "Server closed the connection."; break; }
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                break;
            }
            buffer[n] = '\0';
            fputs(buffer, stdout);
        }

        /* A line typed by the user */
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            if (!fgets(input, sizeof(input), stdin)) {
                if (g_signal) break;           /* fgets interrupted by Ctrl+C */
                reason = "End of input (Ctrl+D).";
                break;
            }
            /* Always send the newline: the server reads line-by-line.
             * (Old code stripped it, and an empty line made write() send
             * 0 bytes, which the client treated as an error and quit.) */
            size_t len = strlen(input);
            if (len == 0 || input[len - 1] != '\n') {
                if (len < sizeof(input) - 1) { input[len++] = '\n'; input[len] = '\0'; }
            }
            if (send_all(sock, input, len) != 0) {
                if (!g_signal) reason = "Lost connection to server.";
                break;
            }
        }
    }

    if (g_signal) {
        /* Safe to use printf here - we're back in normal program flow. */
        printf("\n[client] Caught %s. Closing connection gracefully...\n",
               strsignal(g_signal));
    } else if (reason) {
        printf("\n[client] %s\n", reason);
    }

    /* Closing the socket sends FIN -> the server's recv() returns 0 -> the
     * server thread logs this user out and frees the session slot. */
    shutdown(sock, SHUT_RDWR);
    close(sock);
    printf("Disconnected\n");
    return EXIT_SUCCESS;
}
