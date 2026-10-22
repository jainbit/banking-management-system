#include "admin.h"
#include "auth.h"
#include "common.h"
#include "records.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add_employee(int sock) {
    char buf[BUFFER_SIZE];
    char username[FIELD_MAX], password[FIELD_MAX];

    send_message(sock, "Enter username: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!is_valid_field(buf)) {
        send_message(sock, "Invalid username (1-49 characters, no commas)\n");
        return;
    }
    safe_copy(username, buf, sizeof(username));

    send_message(sock, "Enter password: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!is_valid_field(buf)) {
        send_message(sock, "Invalid password (1-49 characters, no commas)\n");
        return;
    }
    safe_copy(password, buf, sizeof(password));

    int new_id = -1;
    const char *err = NULL;

    db_wrlock(DB_EMPLOYEES);
    if (find_account_by_username(ROLE_EMPLOYEE, username, NULL)) {
        err = "Username already exists\n";
    } else {
        new_id = get_next_id_csv(EMPLOYEES_CSV);
        FILE *fp = fopen(EMPLOYEES_CSV, "a");
        if (!fp) {
            err = "Error opening file\n";
        } else {
            AccountRecord r = { .id = new_id };
            safe_copy(r.username, username, sizeof(r.username));
            safe_copy(r.password, password, sizeof(r.password));
            write_account_line(ROLE_EMPLOYEE, fp, &r);
            fclose(fp);
        }
    }
    db_unlock(DB_EMPLOYEES);

    if (err) { send_message(sock, err); return; }
    snprintf(buf, sizeof(buf), "Employee created! ID: %d\n", new_id);
    send_message(sock, buf);
}

/* Starts a rewrite of role's CSV that copies every row except `id`.
 * Caller holds the write lock and must commit or abort `rw`. */
static int begin_remove_row(int role, int id, CsvRewrite *rw) {
    if (csv_rewrite_begin(rw, role_csv_path(role), role_tmp_path(role)) != 0) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), rw->in)) line[0] = '\0';
    fputs(role_csv_header(role), rw->out);

    while (fgets(line, sizeof(line), rw->in)) {
        AccountRecord r;
        trim_string(line);
        if (parse_account_line(role, line, &r)) {
            if (r.id != id) write_account_line(role, rw->out, &r);
        } else if (line[0]) {
            fprintf(rw->out, "%s\n", line);
        }
    }
    return 0;
}

static int append_account(int role, const AccountRecord *r) {
    FILE *fp = fopen(role_csv_path(role), "a");
    if (!fp) return -1;
    write_account_line(role, fp, r);
    fclose(fp);
    return 0;
}

/* Pending loans assigned to an employee who is being promoted would be stuck
 * forever (the employee ID disappears). Put them back to "unassigned" so a
 * manager can re-assign them. Caller holds db_wrlock(DB_LOANS).
 * Returns number of loans changed, -1 on error. */
static int unassign_pending_loans(int eid) {
    char emp_id_str[20];
    snprintf(emp_id_str, sizeof(emp_id_str), "%d", eid);

    CsvRewrite rw;
    if (csv_rewrite_begin(&rw, LOANS_CSV, LOANS_TMP) != 0) return -1;

    char line[300];
    int changed = 0;
    if (!fgets(line, sizeof(line), rw.in)) line[0] = '\0';
    fputs(LOANS_HEADER, rw.out);

    while (fgets(line, sizeof(line), rw.in)) {
        LoanRecord l;
        trim_string(line);
        if (!parse_loan_line(line, &l)) {
            if (line[0]) fprintf(rw.out, "%s\n", line);
            continue;
        }
        if (strcmp(l.assigned, emp_id_str) == 0 && strcmp(l.status, "pending") == 0) {
            safe_copy(l.assigned, "unassigned", sizeof(l.assigned));
            changed++;
        }
        write_loan_line(rw.out, &l);
    }

    if (changed == 0) { csv_rewrite_abort(&rw); return 0; }
    return csv_rewrite_commit(&rw) == 0 ? changed : -1;
}

/* Moves an account from one staff file to the other.
 * FIX: the old version did this with NO locks and without checking fopen()
 * results (NULL FILE* -> crash). Locks are taken in the global order:
 * LOANS < EMPLOYEES < MANAGERS - the same for promote AND demote, so the two
 * operations can never deadlock each other. */
static void move_staff(int sock, int from_role, int to_role, int id) {
    AccountRecord rec;
    CsvRewrite rw;
    char msg[BUFFER_SIZE];
    int promoting = (from_role == ROLE_EMPLOYEE);
    int new_id = -1, loans_reset = 0;
    const char *err = NULL;

    if (promoting) db_wrlock(DB_LOANS);
    db_wrlock(DB_EMPLOYEES);
    db_wrlock(DB_MANAGERS);

    if (!find_account_by_id(from_role, id, &rec)) {
        err = promoting ? "Employee not found\n" : "Manager not found\n";
    } else if (find_account_by_username(to_role, rec.username, NULL)) {
        err = "An account with that username already exists in the target role\n";
    } else if (begin_remove_row(from_role, id, &rw) != 0) {
        err = "Error opening file\n";
    } else {
        AccountRecord moved = rec;
        new_id = get_next_id_csv(role_csv_path(to_role));
        moved.id = new_id;

        if (append_account(to_role, &moved) != 0) {
            csv_rewrite_abort(&rw);
            err = "Error writing target file\n";
        } else if (csv_rewrite_commit(&rw) != 0) {
            err = "Error saving changes\n";
        } else if (promoting) {
            loans_reset = unassign_pending_loans(id);
        }
    }

    db_unlock(DB_MANAGERS);
    db_unlock(DB_EMPLOYEES);
    if (promoting) db_unlock(DB_LOANS);

    if (err) { send_message(sock, err); return; }

    if (promoting) {
        snprintf(msg, sizeof(msg),
                 "Employee promoted to Manager successfully (new Manager ID: %d)\n", new_id);
        send_message(sock, msg);
        if (loans_reset > 0) {
            snprintf(msg, sizeof(msg),
                     "%d pending loan(s) were unassigned and need to be re-assigned\n", loans_reset);
            send_message(sock, msg);
        }
    } else {
        snprintf(msg, sizeof(msg),
                 "Manager demoted to Employee successfully (new Employee ID: %d)\n", new_id);
        send_message(sock, msg);
    }
}

static void manage_user_roles(int sock) {
    char buf[BUFFER_SIZE];
    int choice, id;

    send_message(sock,
        "\n=== Manage User Roles ===\n"
        "1. Promote Employee to Manager\n"
        "2. Demote Manager to Employee\n"
        "Choice: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &choice) || (choice != 1 && choice != 2)) {
        send_message(sock, "Invalid choice\n");
        return;
    }

    send_message(sock, choice == 1 ? "Enter employee ID to promote: "
                                   : "Enter manager ID to demote: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &id) || id <= 0) {
        send_message(sock, "Invalid ID\n");
        return;
    }

    if (choice == 1) move_staff(sock, ROLE_EMPLOYEE, ROLE_MANAGER, id);
    else             move_staff(sock, ROLE_MANAGER, ROLE_EMPLOYEE, id);
}

void handle_admin(int sock, int aid) {
    char buffer[BUFFER_SIZE];

    for (;;) {
        send_message(sock,
            "\n=== Admin Menu ===\n"
            "1. Add New Bank Employee\n"
            "2. Modify Customer Details\n"
            "3. Modify Employee Details\n"
            "4. Manage User Roles\n"
            "5. Change Password\n"
            "6. Logout\n"
            "Choice: ");

        if (!read_input(sock, buffer, BUFFER_SIZE)) return;
        int choice;
        if (!parse_int(buffer, &choice)) choice = -1;

        switch (choice) {
            case 1: add_employee(sock); break;
            case 2: modify_account_details(sock, ROLE_CUSTOMER); break;
            case 3: modify_account_details(sock, ROLE_EMPLOYEE); break;
            case 4: manage_user_roles(sock); break;
            case 5: change_password(sock, ROLE_ADMIN, aid); break;
            case 6: return;
            default: send_message(sock, "Invalid choice\n");
        }
    }
}
