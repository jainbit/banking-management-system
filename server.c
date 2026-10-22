#include "common.h"
#include "admin.h"
#include "auth.h"
#include "customer.h"
#include "employee.h"
#include "manager.h"
#include "records.h"
#include "server.h"
#include "session_manager.h"
#include "signals.h"
#include "utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Registry of connected client sockets. Needed so that on shutdown the main
 * thread can tell every client "server is going down" and shutdown() their
 * sockets, which makes each worker thread's blocking recv() return 0.
 * ------------------------------------------------------------------------- */
static int client_fds[MAX_CLIENTS];
static int active_clients = 0;
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  clients_cv  = PTHREAD_COND_INITIALIZER;
static time_t start_time;

typedef struct {
    int  sock;
    char peer[64];
} ClientArg;

static int register_client(int fd) {
    int ok = 0;
    pthread_mutex_lock(&clients_mtx);
    if (active_clients < MAX_CLIENTS) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] == -1) {
                client_fds[i] = fd;
                active_clients++;
                ok = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&clients_mtx);
    return ok;
}

/* Must be called BEFORE close(fd), so the main thread never calls shutdown()
 * on an fd number that has already been closed and reused. */
static void unregister_client(int fd) {
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] == fd) {
            client_fds[i] = -1;
            active_clients--;
            break;
        }
    }
    pthread_cond_signal(&clients_cv);       /* wake shutdown waiter */
    pthread_mutex_unlock(&clients_mtx);
}

void print_server_status(void) {
    pthread_mutex_lock(&clients_mtx);
    int clients = active_clients;
    pthread_mutex_unlock(&clients_mtx);

    long up = (long)(time(NULL) - start_time);
    printf("\n[status] PID %d | uptime %ldh %02ldm %02lds | connected clients: %d/%d | "
           "logged-in sessions: %d/%d\n",
           (int)getpid(), up / 3600, (up % 3600) / 60, up % 60,
           clients, MAX_CLIENTS, count_active_sessions(), MAX_SESSIONS);
    fflush(stdout);
}

/* ------------------------------------------------------------------------- */
/* One thread per client                                                     */
/* ------------------------------------------------------------------------- */
static void *handle_client(void *arg) {
    ClientArg *ca = (ClientArg *)arg;
    int sock = ca->sock;
    char peer[64];
    safe_copy(peer, ca->peer, sizeof(peer));
    free(ca);

    char buffer[BUFFER_SIZE];

    while (server_running()) {
        send_message(sock,
            "\n=== Banking System ===\n"
            "1. Customer\n"
            "2. Employee\n"
            "3. Manager\n"
            "4. Admin\n"
            "5. Exit\n"
            "Select role: ");

        if (!read_input(sock, buffer, BUFFER_SIZE)) break;

        int role;
        if (!parse_int(buffer, &role)) role = -1;

        if (role == 5) {
            send_message(sock, "Goodbye!\n");
            break;
        }
        if (!role_is_valid(role)) {
            send_message(sock, "Invalid role!\n");
            continue;
        }

        /* FIX: strcpy(username, buffer) copied up to 1023 bytes into a
         * 50-byte stack array -> stack buffer overflow. Now bounded. */
        char username[FIELD_MAX], password[FIELD_MAX];

        send_message(sock, "Username: ");
        if (!read_input(sock, buffer, BUFFER_SIZE)) break;
        safe_copy(username, buffer, sizeof(username));

        send_message(sock, "Password: ");
        if (!read_input(sock, buffer, BUFFER_SIZE)) break;
        safe_copy(password, buffer, sizeof(password));

        int user_id = -1;
        int rc = authenticate(role, username, password, &user_id);

        if (rc == AUTH_ALREADY_LOGGED_IN) {
            send_message(sock, "\n✗ This account is already logged in from another session.\n");
            continue;
        }
        if (rc == AUTH_INACTIVE) {
            send_message(sock, "\n✗ Account is deactivated. Please contact the bank.\n");
            continue;
        }
        if (rc != AUTH_OK) {
            send_message(sock, "\n✗ Authentication failed!\n");
            continue;
        }

        printf("[auth] %s '%s' (id=%d) logged in from %s\n",
               role_name(role), username, user_id, peer);
        send_message(sock, "\n✓ Login successful!\n");

        switch (role) {
            case ROLE_CUSTOMER: handle_customer(sock, user_id); break;
            case ROLE_EMPLOYEE: handle_employee(sock, user_id); break;
            case ROLE_MANAGER:  handle_manager(sock, user_id);  break;
            case ROLE_ADMIN:    handle_admin(sock, user_id);    break;
        }

        /* Reached on normal logout, on client disconnect, on idle timeout
         * and on server shutdown - so the session slot is always freed. */
        logout_user(role, user_id);
        printf("[auth] %s '%s' (id=%d) logged out\n", role_name(role), username, user_id);
        send_message(sock, "\nLogged out.\n");
    }

    printf("[-] Client %s disconnected\n", peer);
    unregister_client(sock);
    close(sock);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Graceful shutdown                                                         */
/* ------------------------------------------------------------------------- */
static void graceful_shutdown(int listen_fd) {
    close(listen_fd);                       /* stop accepting new clients */

    pthread_mutex_lock(&clients_mtx);
    int notified = active_clients;
    const char *bye = "\n\n*** Server is shutting down. You have been logged out. ***\n";
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] != -1) {
            /* MSG_DONTWAIT: never block here on a slow client */
            ssize_t ignored = send(client_fds[i], bye, strlen(bye), MSG_DONTWAIT);
            (void)ignored;
            /* Makes the thread's blocked recv() return 0 -> it unwinds,
             * logs the user out and exits by itself. */
            shutdown(client_fds[i], SHUT_RDWR);
        }
    }
    printf("[shutdown] Notified %d connected client(s). Waiting up to %d s for threads...\n",
           notified, SHUTDOWN_GRACE_SEC);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += SHUTDOWN_GRACE_SEC;

    while (active_clients > 0) {
        if (pthread_cond_timedwait(&clients_cv, &clients_mtx, &deadline) == ETIMEDOUT)
            break;
    }
    int remaining = active_clients;
    pthread_mutex_unlock(&clients_mtx);

    if (remaining > 0)
        printf("[shutdown] %d thread(s) still busy after timeout; exiting anyway.\n", remaining);
    else
        printf("[shutdown] All client threads finished.\n");

    destroy_session_manager();
    printf("[shutdown] Server stopped cleanly. Bye!\n");
}

/* ------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    int port = PORT;
    if (argc > 1 && (!parse_int(argv[1], &port) || port < 1 || port > 65535)) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);       /* line-buffered log output */
    start_time = time(NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) client_fds[i] = -1;

    /* Self-pipe: the signal thread writes 1 byte here to wake poll(). */
    int wake_pipe[2];
    if (pipe(wake_pipe) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    /* Must happen before ANY other thread is created. */
    if (signals_init(wake_pipe[1]) != 0) return EXIT_FAILURE;

    mkdir(DATA_DIR, 0755);

    /* The socket is bound BEFORE touching shared memory: if the port is
     * taken (server already running) we must exit without disturbing the
     * running server's session table. */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        if (errno == EADDRINUSE)
            fprintf(stderr, "Port %d is already in use. Find the process with: lsof -i :%d\n",
                    port, port);
        close(listen_fd);
        return EXIT_FAILURE;
    }
    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (init_session_manager() != 0) {
        fprintf(stderr, "Could not initialise session manager. "
                        "If no server is running, try: make reset-ipc\n");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on port %d (PID %d)\n", port, (int)getpid());
    printf("  Ctrl+C / kill %d       -> graceful shutdown\n", (int)getpid());
    printf("  kill -USR1 %d          -> print server status\n", (int)getpid());

    struct pollfd pfds[2];
    pfds[0].fd = listen_fd;    pfds[0].events = POLLIN;
    pfds[1].fd = wake_pipe[0]; pfds[1].events = POLLIN;

    while (server_running()) {
        int r = poll(pfds, 2, -1);
        if (r == -1) {
            if (errno == EINTR) continue;           /* interrupted: retry */
            perror("poll");
            break;
        }

        if (pfds[1].revents & POLLIN) break;        /* shutdown requested */

        if (pfds[0].revents & POLLIN) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd == -1) {
                if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) continue;
                perror("accept");
                continue;
            }

            /* Idle timeout: recv() fails with EAGAIN after N seconds of
             * silence. (alarm()/SIGALRM can't be used per-thread: signals
             * are delivered to the process, not to a particular client.) */
            struct timeval tv = { CLIENT_IDLE_TIMEOUT_SEC, 0 };
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
            int one = 1;                            /* macOS: no SIGPIPE on this socket */
            setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

            if (!register_client(cfd)) {
                send_message(cfd, "Server is busy, please try again later.\n");
                close(cfd);
                continue;
            }

            ClientArg *ca = malloc(sizeof(ClientArg));
            if (!ca) {
                unregister_client(cfd);
                close(cfd);
                continue;
            }
            ca->sock = cfd;
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
            snprintf(ca->peer, sizeof(ca->peer), "%s:%d", ip, ntohs(caddr.sin_port));
            char peer_copy[64];                     /* ca is freed by the thread */
            safe_copy(peer_copy, ca->peer, sizeof(peer_copy));

            pthread_t tid;
            int rc = pthread_create(&tid, NULL, handle_client, ca);
            if (rc != 0) {
                fprintf(stderr, "pthread_create: %s\n", strerror(rc));
                unregister_client(cfd);
                close(cfd);
                free(ca);
                continue;
            }
            pthread_detach(tid);
            printf("[+] Client %s connected (fd=%d)\n", peer_copy, cfd);
        }
    }

    graceful_shutdown(listen_fd);
    /* The wake pipe is intentionally NOT closed: the signal thread may still
     * be running, and closing an fd another thread might write to is a race
     * (the fd number could be reused). The OS closes it at exit. */
    return EXIT_SUCCESS;
}
