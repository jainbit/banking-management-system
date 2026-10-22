#include "auth.h"
#include "common.h"
#include "records.h"
#include "session_manager.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

/* NOTE: the old version printed every username AND plaintext password it read
 * to the server console ("DEBUG: Read line ..."). Those debug prints were
 * removed - logging passwords is a serious security problem. */
int authenticate(int role, const char *username, const char *password, int *user_id) {
    if (!role_is_valid(role)) return AUTH_FAIL;

    AccountRecord rec;
    int found;

    db_rdlock(role_db(role));
    found = find_account_by_username(role, username, &rec);
    db_unlock(role_db(role));

    if (!found || strcmp(rec.password, password) != 0) return AUTH_FAIL;
    if (role == ROLE_CUSTOMER && !rec.active) return AUTH_INACTIVE;

    if (!check_and_register_login(role, rec.id)) return AUTH_ALREADY_LOGGED_IN;

    *user_id = rec.id;
    return AUTH_OK;
}

void logout_user(int role, int user_id) {
    register_logout(role, user_id);
}

/* Works for all four roles (customers.csv has 5 columns, the others 3). */
void change_password(int sock, int role, int id) {
    char buf[BUFFER_SIZE];
    char old_pass[FIELD_MAX], new_pass[FIELD_MAX];

    send_message(sock, "Enter current password: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    safe_copy(old_pass, buf, sizeof(old_pass));

    send_message(sock, "Enter new password: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!is_valid_field(buf)) {
        send_message(sock, "Invalid password (1-49 characters, no commas)\n");
        return;
    }
    safe_copy(new_pass, buf, sizeof(new_pass));

    DbFile db = role_db(role);
    CsvRewrite rw;
    int found = 0, ok;

    db_wrlock(db);
    if (csv_rewrite_begin(&rw, role_csv_path(role), role_tmp_path(role)) != 0) {
        db_unlock(db);
        send_message(sock, "Error opening file\n");
        return;
    }

    char line[256];
    if (!fgets(line, sizeof(line), rw.in)) line[0] = '\0';   /* header */
    fputs(role_csv_header(role), rw.out);

    while (fgets(line, sizeof(line), rw.in)) {
        AccountRecord r;
        trim_string(line);
        if (!parse_account_line(role, line, &r)) {
            if (line[0]) fprintf(rw.out, "%s\n", line);   /* keep, don't lose data */
            continue;
        }
        if (r.id == id && strcmp(r.password, old_pass) == 0) {
            safe_copy(r.password, new_pass, sizeof(r.password));
            found = 1;
        }
        write_account_line(role, rw.out, &r);
    }

    if (found) ok = (csv_rewrite_commit(&rw) == 0);
    else { csv_rewrite_abort(&rw); ok = 1; }
    db_unlock(db);

    if (!ok)          send_message(sock, "Error saving changes\n");
    else if (found)   send_message(sock, "Password changed successfully\n");
    else              send_message(sock, "Incorrect password\n");
}

/* Used by Employee (customers) and Admin (customers + employees).
 *
 * IMPORTANT FIX: the old version asked the user for the new username and
 * password *while holding the file lock*. A user who walked away at that
 * prompt blocked every other thread touching that file. Now we collect all
 * input first, and only then lock + rewrite. Network I/O never happens while
 * a lock is held. */
void modify_account_details(int sock, int role) {
    char buf[BUFFER_SIZE];
    char new_user[FIELD_MAX] = "", new_pass[FIELD_MAX] = "";
    int id;
    const char *label = role_name(role);
    DbFile db = role_db(role);

    snprintf(buf, sizeof(buf), "Enter %s ID: ", label);
    send_message(sock, buf);
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &id) || id <= 0) {
        send_message(sock, "Invalid ID\n");
        return;
    }

    db_rdlock(db);
    int exists = find_account_by_id(role, id, NULL);
    db_unlock(db);
    if (!exists) {
        snprintf(buf, sizeof(buf), "%s not found\n", label);
        send_message(sock, buf);
        return;
    }

    send_message(sock, "New username (- to skip): ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (strcmp(buf, "-") != 0) {
        if (!is_valid_field(buf)) { send_message(sock, "Invalid username\n"); return; }
        safe_copy(new_user, buf, sizeof(new_user));
    }

    send_message(sock, "New password (- to skip): ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (strcmp(buf, "-") != 0) {
        if (!is_valid_field(buf)) { send_message(sock, "Invalid password\n"); return; }
        safe_copy(new_pass, buf, sizeof(new_pass));
    }

    if (!new_user[0] && !new_pass[0]) {
        send_message(sock, "Nothing to change\n");
        return;
    }

    const char *result;
    CsvRewrite rw;

    db_wrlock(db);
    AccountRecord other;
    if (new_user[0] && find_account_by_username(role, new_user, &other) && other.id != id) {
        result = "Username already taken\n";
    } else if (csv_rewrite_begin(&rw, role_csv_path(role), role_tmp_path(role)) != 0) {
        result = "Error opening file\n";
    } else {
        char line[256];
        int found = 0;
        if (!fgets(line, sizeof(line), rw.in)) line[0] = '\0';
        fputs(role_csv_header(role), rw.out);

        while (fgets(line, sizeof(line), rw.in)) {
            AccountRecord r;
            trim_string(line);
            if (!parse_account_line(role, line, &r)) {
                if (line[0]) fprintf(rw.out, "%s\n", line);
                continue;
            }
            if (r.id == id) {
                if (new_user[0]) safe_copy(r.username, new_user, sizeof(r.username));
                if (new_pass[0]) safe_copy(r.password, new_pass, sizeof(r.password));
                found = 1;
            }
            write_account_line(role, rw.out, &r);
        }

        if (!found) { csv_rewrite_abort(&rw); result = "Record disappeared, nothing changed\n"; }
        else if (csv_rewrite_commit(&rw) != 0) result = "Error saving changes\n";
        else result = (role == ROLE_CUSTOMER) ? "Customer updated\n" : "Employee updated\n";
    }
    db_unlock(db);

    send_message(sock, result);
}
