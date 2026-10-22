#include "utils.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Linux: MSG_NOSIGNAL stops send() from raising SIGPIPE.
 * macOS has no MSG_NOSIGNAL; there we use SO_NOSIGPIPE on the socket and
 * also ignore SIGPIPE process-wide (see signals.c). */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* ------------------------------------------------------------------------- */
/* Per-file read/write locks                                                 */
/* ------------------------------------------------------------------------- */
static pthread_rwlock_t db_locks[DB_COUNT] = {
    PTHREAD_RWLOCK_INITIALIZER, PTHREAD_RWLOCK_INITIALIZER,
    PTHREAD_RWLOCK_INITIALIZER, PTHREAD_RWLOCK_INITIALIZER,
    PTHREAD_RWLOCK_INITIALIZER, PTHREAD_RWLOCK_INITIALIZER,
    PTHREAD_RWLOCK_INITIALIZER
};

void db_rdlock(DbFile f) { pthread_rwlock_rdlock(&db_locks[f]); }
void db_wrlock(DbFile f) { pthread_rwlock_wrlock(&db_locks[f]); }
void db_unlock(DbFile f) { pthread_rwlock_unlock(&db_locks[f]); }

/* ------------------------------------------------------------------------- */
/* Socket I/O                                                                */
/* ------------------------------------------------------------------------- */

/* Sends the whole message. Handles partial writes and EINTR. If the peer has
 * gone away, send() fails with EPIPE (instead of killing the process with
 * SIGPIPE) and we simply return. */
void send_message(int sock, const char *msg) {
    size_t len = strlen(msg);
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(sock, msg + sent, len - sent, SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR) continue;   /* interrupted by a signal: retry */
            return;                         /* EPIPE / ECONNRESET: client gone */
        }
        sent += (size_t)n;
    }
}

/* Reads one line (terminated by '\n') from the socket.
 *  - returns 1 on success (buffer holds the trimmed line, always NUL-terminated)
 *  - returns 0 if the client disconnected, the server is shutting down
 *    (socket was shutdown()), or the idle timeout (SO_RCVTIMEO) expired.
 * Reading byte-by-byte guarantees that two lines typed quickly are never
 * merged into one read and that we never overflow `buffer`. */
int read_input(int sock, char *buffer, int size) {
    int len = 0;

    if (size <= 0) return 0;
    buffer[0] = '\0';

    for (;;) {
        char c;
        ssize_t n = recv(sock, &c, 1, 0);

        if (n == 0) return 0;                       /* orderly EOF */
        if (n < 0) {
            if (errno == EINTR) continue;           /* signal interrupted recv */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                send_message(sock,
                    "\n\n*** Session timed out due to inactivity. Disconnecting. ***\n");
            }
            return 0;
        }

        if (c == '\n') break;
        if (c == '\r') continue;                    /* telnet / nc send CRLF */
        if (len < size - 1) buffer[len++] = c;      /* silently drop overflow */
    }

    buffer[len] = '\0';
    trim_string(buffer);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* fcntl advisory locking                                                    */
/* ------------------------------------------------------------------------- */
int lock_file(int fd, int lock_type) {
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = (short)lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;                                 /* whole file */

    while (fcntl(fd, F_SETLKW, &lock) == -1) {
        if (errno == EINTR) continue;               /* wait was interrupted */
        perror("Error acquiring lock");
        return -1;
    }
    return 0;
}

int unlock_file(int fd) {
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;

    if (fcntl(fd, F_SETLK, &lock) == -1) {
        perror("Error releasing lock");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* CSV rewrite through a temp file + atomic rename()                         */
/* ------------------------------------------------------------------------- */
int csv_rewrite_begin(CsvRewrite *rw, const char *path, const char *tmp_path) {
    memset(rw, 0, sizeof(*rw));
    rw->fd = -1;
    rw->path = path;
    rw->tmp = tmp_path;

    rw->fd = open(path, O_RDWR);
    if (rw->fd == -1) return -1;

    if (lock_file(rw->fd, F_WRLCK) == -1) {
        close(rw->fd);
        return -1;
    }

    int dfd = dup(rw->fd);
    if (dfd >= 0) {
        rw->in = fdopen(dfd, "r");
        if (!rw->in) close(dfd);
    }
    rw->out = fopen(tmp_path, "w");

    if (!rw->in || !rw->out) {
        if (rw->in) fclose(rw->in);
        if (rw->out) { fclose(rw->out); remove(tmp_path); }
        unlock_file(rw->fd);
        close(rw->fd);
        return -1;
    }
    return 0;
}

int csv_rewrite_commit(CsvRewrite *rw) {
    int ok = 1;

    fclose(rw->in);
    if (fflush(rw->out) != 0) ok = 0;
    if (ok && fsync(fileno(rw->out)) != 0) ok = 0;  /* data really on disk */
    if (fclose(rw->out) != 0) ok = 0;

    /* rename() is atomic: readers see either the old or the new file, never
     * a half-written one - even if the server is killed right here. */
    if (ok && rename(rw->tmp, rw->path) != 0) {
        perror("rename");
        ok = 0;
    }
    if (!ok) remove(rw->tmp);

    unlock_file(rw->fd);
    close(rw->fd);
    return ok ? 0 : -1;
}

void csv_rewrite_abort(CsvRewrite *rw) {
    fclose(rw->in);
    fclose(rw->out);
    remove(rw->tmp);
    unlock_file(rw->fd);
    close(rw->fd);
}

/* ------------------------------------------------------------------------- */
/* String buffer                                                             */
/* ------------------------------------------------------------------------- */
void sb_init(StrBuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

int sb_appendf(StrBuf *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (need < 0) { va_end(ap2); return -1; }

    if (sb->len + (size_t)need + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 256;
        while (ncap < sb->len + (size_t)need + 1) ncap *= 2;
        char *p = realloc(sb->data, ncap);
        if (!p) { va_end(ap2); return -1; }
        sb->data = p;
        sb->cap = ncap;
    }

    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)need;
    return 0;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb_init(sb);
}

/* ------------------------------------------------------------------------- */
/* Misc helpers                                                              */
/* ------------------------------------------------------------------------- */

/* Caller must hold the write lock of the file, otherwise two threads could
 * compute the same "next id". */
int get_next_id_csv(const char *csv_path) {
    FILE *fp = fopen(csv_path, "r");
    if (!fp) return 1;

    char line[512];
    int max_id = 0;

    if (fgets(line, sizeof(line), fp)) {            /* skip header */
        while (fgets(line, sizeof(line), fp)) {
            int id;
            if (sscanf(line, "%d,", &id) == 1 && id > max_id) max_id = id;
        }
    }

    fclose(fp);
    return max_id + 1;
}

/* Trims whitespace on both ends, in place. */
void trim_string(char *str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' ||
                       str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[--len] = '\0';
    }

    size_t start = 0;
    while (str[start] == ' ' || str[start] == '\t') start++;
    if (start > 0) memmove(str, str + start, len - start + 1);
}

/* Bounded copy that always NUL-terminates (replaces unsafe strcpy). */
void safe_copy(char *dst, const char *src, size_t size) {
    if (size == 0) return;
    snprintf(dst, size, "%s", src);
}

/* A username/password must be non-empty, fit in FIELD_MAX, and must not
 * contain characters that would corrupt the CSV format. */
int is_valid_field(const char *s) {
    size_t len = strlen(s);
    if (len == 0 || len >= FIELD_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ',' || s[i] == '\n' || s[i] == '\r') return 0;
    }
    return 1;
}

void sanitize_free_text(char *s) {
    for (; *s; s++) {
        if (*s == ',') *s = ';';
        else if (*s == '\n' || *s == '\r') *s = ' ';
    }
}

/* Strict number parsing: rejects "abc", "12abc", "nan", "inf", negatives. */
int parse_amount(const char *s, double *out, int allow_zero) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);

    if (end == s || errno != 0) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return 0;
    if (!isfinite(v)) return 0;
    if (allow_zero ? (v < 0) : (v <= 0)) return 0;
    if (v > MAX_AMOUNT) return 0;

    *out = v;
    return 1;
}

int parse_int(const char *s, int *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);

    if (end == s || errno != 0) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return 0;
    if (v < INT_MIN || v > INT_MAX) return 0;

    *out = (int)v;
    return 1;
}

/* localtime() uses a static buffer and is NOT thread-safe; localtime_r is. */
void current_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm_now);
}

void log_transaction(int cid, const char *type, double amt, double balance_after) {
    char ts[32];
    current_timestamp(ts, sizeof(ts));

    db_wrlock(DB_TRANSACTIONS);
    FILE *fp = fopen(DATA_DIR "/transactions.csv", "a");
    if (fp) {
        fprintf(fp, "%d,%s,%.2f,%.2f,%s\n", cid, type, amt, balance_after, ts);
        fclose(fp);
    }
    db_unlock(DB_TRANSACTIONS);
}
