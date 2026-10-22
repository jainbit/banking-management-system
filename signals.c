#include "signals.h"
#include "server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* atomic_int instead of a plain int: it is read by many threads and written
 * by the signal thread. (volatile sig_atomic_t is only enough for a classic
 * single-threaded signal handler.) */
static atomic_int g_running = 1;
static int g_wake_fd = -1;
static sigset_t g_sigset;

static void *signal_thread(void *arg) {
    (void)arg;
    int shutdown_requested = 0;

    for (;;) {
        int sig = 0;
        int rc = sigwait(&g_sigset, &sig);     /* sleeps until a signal arrives */
        if (rc != 0) {
            if (rc == EINTR) continue;
            fprintf(stderr, "[signal] sigwait failed: %s\n", strerror(rc));
            continue;
        }

        switch (sig) {
        case SIGUSR1:
            print_server_status();
            break;

        case SIGINT:
        case SIGTERM:
        case SIGQUIT:
        case SIGHUP:
            if (!shutdown_requested) {
                shutdown_requested = 1;
                printf("\n[signal] Caught %s (signal %d). Starting graceful shutdown...\n"
                       "[signal] Press Ctrl+C again to force an immediate exit.\n",
                       strsignal(sig), sig);
                fflush(stdout);
                atomic_store(&g_running, 0);

                /* Wake up poll() in main(). write() is retried on EINTR. */
                ssize_t w;
                do { w = write(g_wake_fd, "x", 1); } while (w == -1 && errno == EINTR);
            } else {
                fprintf(stderr, "\n[signal] Second %s received - forcing exit now.\n",
                        strsignal(sig));
                _exit(EXIT_FAILURE);   /* async-safe, skips atexit handlers */
            }
            break;

        default:
            break;
        }
    }
    return NULL;
}

int signals_init(int wake_fd) {
    g_wake_fd = wake_fd;

    /* 1) Ignore SIGPIPE for the whole process. Default action of SIGPIPE is
     *    to TERMINATE - so one client closing its terminal while the server
     *    was writing to it used to kill the server and every other client. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        return -1;
    }

    /* 2) Block the "control" signals in this (main) thread. Threads created
     *    afterwards inherit this mask, so no worker thread is ever interrupted
     *    asynchronously in the middle of a file update. */
    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    sigaddset(&g_sigset, SIGQUIT);
    sigaddset(&g_sigset, SIGHUP);
    sigaddset(&g_sigset, SIGUSR1);

    int rc = pthread_sigmask(SIG_BLOCK, &g_sigset, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(rc));
        return -1;
    }

    /* 3) One thread that collects them synchronously. */
    pthread_t tid;
    rc = pthread_create(&tid, NULL, signal_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_create(signal thread): %s\n", strerror(rc));
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

int server_running(void) {
    return atomic_load(&g_running);
}
