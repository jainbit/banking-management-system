#include "records.h"

#include <string.h>

int role_is_valid(int role) {
    return role >= ROLE_CUSTOMER && role <= ROLE_ADMIN;
}

const char *role_name(int role) {
    switch (role) {
        case ROLE_CUSTOMER: return "Customer";
        case ROLE_EMPLOYEE: return "Employee";
        case ROLE_MANAGER:  return "Manager";
        case ROLE_ADMIN:    return "Admin";
        default:            return "Unknown";
    }
}

DbFile role_db(int role) {
    switch (role) {
        case ROLE_CUSTOMER: return DB_CUSTOMERS;
        case ROLE_EMPLOYEE: return DB_EMPLOYEES;
        case ROLE_MANAGER:  return DB_MANAGERS;
        default:            return DB_ADMINS;
    }
}

/* String literals -> immutable -> thread-safe (the old get_csv_path() used a
 * shared static buffer that two threads could overwrite at the same time). */
const char *role_csv_path(int role) {
    switch (role) {
        case ROLE_CUSTOMER: return CUSTOMERS_CSV;
        case ROLE_EMPLOYEE: return EMPLOYEES_CSV;
        case ROLE_MANAGER:  return MANAGERS_CSV;
        default:            return ADMINS_CSV;
    }
}

const char *role_tmp_path(int role) {
    switch (role) {
        case ROLE_CUSTOMER: return DATA_DIR "/customers.tmp";
        case ROLE_EMPLOYEE: return DATA_DIR "/employees.tmp";
        case ROLE_MANAGER:  return DATA_DIR "/managers.tmp";
        default:            return DATA_DIR "/admins.tmp";
    }
}

const char *role_csv_header(int role) {
    return role == ROLE_CUSTOMER ? "id,username,password,balance,active\n"
                                 : "id,username,password\n";
}

int parse_account_line(int role, const char *line, AccountRecord *r) {
    memset(r, 0, sizeof(*r));

    if (role == ROLE_CUSTOMER) {
        if (sscanf(line, "%d,%49[^,],%49[^,],%lf,%d",
                   &r->id, r->username, r->password, &r->balance, &r->active) != 5)
            return 0;
    } else {
        if (sscanf(line, "%d,%49[^,],%49[^\n]", &r->id, r->username, r->password) != 3)
            return 0;
        r->active = 1;
    }
    trim_string(r->username);
    trim_string(r->password);
    return 1;
}

void write_account_line(int role, FILE *out, const AccountRecord *r) {
    if (role == ROLE_CUSTOMER)
        fprintf(out, "%d,%s,%s,%.2f,%d\n", r->id, r->username, r->password, r->balance, r->active);
    else
        fprintf(out, "%d,%s,%s\n", r->id, r->username, r->password);
}

static int find_account(int role, int id, const char *username, AccountRecord *out) {
    FILE *fp = fopen(role_csv_path(role), "r");
    if (!fp) return 0;

    char line[256];
    int found = 0;

    if (fgets(line, sizeof(line), fp)) {
        while (fgets(line, sizeof(line), fp)) {
            AccountRecord r;
            trim_string(line);
            if (!parse_account_line(role, line, &r)) continue;
            if ((username && strcmp(r.username, username) == 0) || (!username && r.id == id)) {
                if (out) *out = r;
                found = 1;
                break;
            }
        }
    }
    fclose(fp);
    return found;
}

int find_account_by_id(int role, int id, AccountRecord *out) {
    return find_account(role, id, NULL, out);
}

int find_account_by_username(int role, const char *username, AccountRecord *out) {
    return find_account(role, 0, username, out);
}

int parse_loan_line(const char *line, LoanRecord *l) {
    memset(l, 0, sizeof(*l));
    if (sscanf(line, "%d,%d,%lf,%19[^,],%d,%19[^,],%19[^\n]",
               &l->loan_id, &l->customer_id, &l->amount, l->type,
               &l->tenure, l->status, l->assigned) != 7)
        return 0;
    trim_string(l->type);
    trim_string(l->status);
    trim_string(l->assigned);
    return 1;
}

void write_loan_line(FILE *out, const LoanRecord *l) {
    fprintf(out, "%d,%d,%.2f,%s,%d,%s,%s\n", l->loan_id, l->customer_id, l->amount,
            l->type, l->tenure, l->status, l->assigned);
}

/* message may (in old data) contain commas, so: id,cid,<message...>,reviewed
 * -> the reviewed flag is whatever follows the LAST comma. */
int parse_feedback_line(const char *line, FeedbackRecord *f) {
    memset(f, 0, sizeof(*f));
    int consumed = 0;
    if (sscanf(line, "%d,%d,%n", &f->id, &f->customer_id, &consumed) < 2 || consumed == 0)
        return 0;

    const char *msg = line + consumed;
    const char *last = strrchr(msg, ',');
    if (!last) return 0;

    size_t mlen = (size_t)(last - msg);
    if (mlen >= sizeof(f->message)) mlen = sizeof(f->message) - 1;
    memcpy(f->message, msg, mlen);
    f->message[mlen] = '\0';
    trim_string(f->message);
    f->reviewed = (last[1] == '1');
    return 1;
}

void write_feedback_line(FILE *out, const FeedbackRecord *f) {
    char msg[256];
    safe_copy(msg, f->message, sizeof(msg));
    sanitize_free_text(msg);
    fprintf(out, "%d,%d,%s,%d\n", f->id, f->customer_id, msg, f->reviewed ? 1 : 0);
}
