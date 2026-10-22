#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * In-process locks for every CSV "table".
 *
 * WHY: fcntl() record locks belong to the *process*, not the thread. All client
 * threads live in one server process, so fcntl() alone never blocks one thread
 * against another. A pthread read-write lock per file gives real mutual
 * exclusion between threads (many readers OR one writer).
 *
 * LOCK ORDER (always acquire in this order to avoid deadlock):
 *   LOANS < CUSTOMERS < EMPLOYEES < MANAGERS < ADMINS < TRANSACTIONS < FEEDBACK
 * ------------------------------------------------------------------------- */
typedef enum {
    DB_LOANS = 0,
    DB_CUSTOMERS,
    DB_EMPLOYEES,
    DB_MANAGERS,
    DB_ADMINS,
    DB_TRANSACTIONS,
    DB_FEEDBACK,
    DB_COUNT
} DbFile;

void db_rdlock(DbFile f);
void db_wrlock(DbFile f);
void db_unlock(DbFile f);

/* Socket I/O (EINTR-safe, SIGPIPE-safe, line based) */
void send_message(int sock, const char *msg);
int  read_input(int sock, char *buffer, int size);

/* fcntl advisory locks (protect against *other processes*) */
int lock_file(int fd, int lock_type);
int unlock_file(int fd);

/* Safe "rewrite a CSV through a temp file" helper.
 * Caller must already hold the matching db_wrlock(). */
typedef struct {
    int   fd;
    FILE *in;
    FILE *out;
    const char *path;
    const char *tmp;
} CsvRewrite;

int  csv_rewrite_begin(CsvRewrite *rw, const char *path, const char *tmp_path);
int  csv_rewrite_commit(CsvRewrite *rw);   /* 0 on success (atomic rename) */
void csv_rewrite_abort(CsvRewrite *rw);    /* discard temp file            */

/* Growable string buffer: build output while holding a lock, send it later */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
int  sb_appendf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_free(StrBuf *sb);

/* Misc helpers */
int  get_next_id_csv(const char *csv_path);
void trim_string(char *str);
void safe_copy(char *dst, const char *src, size_t size);
int  is_valid_field(const char *s);           /* non-empty, < FIELD_MAX, no commas */
void sanitize_free_text(char *s);             /* commas -> ';' for free text      */
int  parse_amount(const char *s, double *out, int allow_zero);
int  parse_int(const char *s, int *out);
void current_timestamp(char *buf, size_t size);
void log_transaction(int cid, const char *type, double amt, double balance_after);

#endif
