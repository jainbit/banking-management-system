#ifndef SIGNALS_H
#define SIGNALS_H

/* ---------------------------------------------------------------------------
 * Server-side signal handling.
 *
 *   SIGINT  (Ctrl+C)          -> graceful shutdown
 *   SIGTERM (kill <pid>)      -> graceful shutdown
 *   SIGQUIT (Ctrl+\)          -> graceful shutdown
 *   SIGHUP  (terminal closed) -> graceful shutdown
 *   SIGUSR1 (kill -USR1 <pid>)-> print live server status, keep running
 *   SIGPIPE                   -> ignored (write to a dead client = EPIPE error,
 *                                instead of killing the whole server)
 *   2nd SIGINT/TERM during shutdown -> force immediate exit
 *
 * Design: the signals are BLOCKED in every thread and one dedicated thread
 * receives them synchronously with sigwait(). Because that thread runs normal
 * code (not an async signal handler) it can safely call printf(), take
 * mutexes, etc. It wakes the main accept loop through a pipe (self-pipe trick).
 * ------------------------------------------------------------------------- */

/* Must be called in main() BEFORE any other thread is created, so that every
 * thread inherits the blocked signal mask. `wake_fd` is the write end of a
 * pipe that main() polls together with the listening socket. */
int signals_init(int wake_fd);

/* 1 while the server should keep running, 0 once shutdown has been requested */
int server_running(void);

#endif
