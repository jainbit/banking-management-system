#include "manager.h"
#include "auth.h"
#include "common.h"
#include "records.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void activate_account(int sock) {
    char buf[BUFFER_SIZE];
    int cid, activate;

    send_message(sock, "Enter customer ID: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &cid) || cid <= 0) { send_message(sock, "Invalid ID\n"); return; }

    send_message(sock, "Activate (1) or Deactivate (0): ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &activate) || (activate != 0 && activate != 1)) {
        send_message(sock, "Please enter 1 or 0\n");
        return;
    }

    int found = 0, ok = 1;
    CsvRewrite rw;

    db_wrlock(DB_CUSTOMERS);
    if (csv_rewrite_begin(&rw, CUSTOMERS_CSV, role_tmp_path(ROLE_CUSTOMER)) != 0) {
        db_unlock(DB_CUSTOMERS);
        send_message(sock, "Error opening file\n");
        return;
    }

    char line[256];
    if (!fgets(line, sizeof(line), rw.in)) line[0] = '\0';
    fputs(role_csv_header(ROLE_CUSTOMER), rw.out);

    while (fgets(line, sizeof(line), rw.in)) {
        AccountRecord r;
        trim_string(line);
        if (!parse_account_line(ROLE_CUSTOMER, line, &r)) {
            if (line[0]) fprintf(rw.out, "%s\n", line);
            continue;
        }
        if (r.id == cid) {
            r.active = activate;
            found = 1;
        }
        write_account_line(ROLE_CUSTOMER, rw.out, &r);
    }

    if (found) ok = (csv_rewrite_commit(&rw) == 0);
    else csv_rewrite_abort(&rw);
    db_unlock(DB_CUSTOMERS);

    if (!ok)        send_message(sock, "Error saving changes\n");
    else if (found) send_message(sock, activate ? "Account activated\n" : "Account deactivated\n");
    else            send_message(sock, "Account not found\n");
}

static void assign_loan(int sock) {
    char buf[BUFFER_SIZE];
    int lid, eid;

    send_message(sock, "Enter loan ID: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &lid) || lid <= 0) { send_message(sock, "Invalid loan ID\n"); return; }

    send_message(sock, "Enter employee ID: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &eid) || eid <= 0) { send_message(sock, "Invalid employee ID\n"); return; }

    /* FIX: loans could be assigned to employee IDs that don't exist. */
    db_rdlock(DB_EMPLOYEES);
    int emp_exists = find_account_by_id(ROLE_EMPLOYEE, eid, NULL);
    db_unlock(DB_EMPLOYEES);
    if (!emp_exists) {
        send_message(sock, "Employee not found\n");
        return;
    }

    enum { NOT_FOUND, NOT_PENDING, DONE, IO_FAIL } status = NOT_FOUND;
    CsvRewrite rw;

    db_wrlock(DB_LOANS);
    if (csv_rewrite_begin(&rw, LOANS_CSV, LOANS_TMP) != 0) {
        db_unlock(DB_LOANS);
        send_message(sock, "Error opening file\n");
        return;
    }

    char line[300];
    if (!fgets(line, sizeof(line), rw.in)) line[0] = '\0';
    fputs(LOANS_HEADER, rw.out);

    while (fgets(line, sizeof(line), rw.in)) {
        LoanRecord l;
        trim_string(line);
        if (!parse_loan_line(line, &l)) {
            if (line[0]) fprintf(rw.out, "%s\n", line);
            continue;
        }
        if (l.loan_id == lid) {
            if (strcmp(l.status, "pending") != 0) {
                status = NOT_PENDING;
            } else {
                snprintf(l.assigned, sizeof(l.assigned), "%d", eid);
                status = DONE;
            }
        }
        write_loan_line(rw.out, &l);
    }

    if (status == DONE) { if (csv_rewrite_commit(&rw) != 0) status = IO_FAIL; }
    else csv_rewrite_abort(&rw);
    db_unlock(DB_LOANS);

    switch (status) {
        case DONE:        send_message(sock, "Loan assigned\n"); break;
        case NOT_PENDING: send_message(sock, "Loan has already been processed\n"); break;
        case IO_FAIL:     send_message(sock, "Error saving changes\n"); break;
        default:          send_message(sock, "Loan not found\n"); break;
    }
}

static void view_feedback(int sock) {
    StrBuf out;
    sb_init(&out);
    int count = 0, unreviewed = 0;
    sb_appendf(&out, "\n=== Customer Feedback ===\n");

    db_rdlock(DB_FEEDBACK);
    FILE *fp = fopen(FEEDBACK_CSV, "r");
    if (fp) {
        char line[512];
        if (fgets(line, sizeof(line), fp)) {
            while (fgets(line, sizeof(line), fp)) {
                FeedbackRecord f;
                trim_string(line);
                if (parse_feedback_line(line, &f)) {
                    sb_appendf(&out, "ID: %d | Customer: %d | Message: %s | Reviewed: %s\n",
                               f.id, f.customer_id, f.message, f.reviewed ? "Yes" : "No");
                    count++;
                    if (!f.reviewed) unreviewed++;
                }
            }
        }
        fclose(fp);
    }
    db_unlock(DB_FEEDBACK);

    if (count == 0) sb_appendf(&out, "No feedback available\n");
    if (out.data) send_message(sock, out.data);
    sb_free(&out);

    if (unreviewed == 0) return;

    /* NEW: the 'reviewed' column existed but nothing ever set it. */
    char buf[BUFFER_SIZE];
    send_message(sock, "Mark all feedback as reviewed? (y/n): ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (buf[0] != 'y' && buf[0] != 'Y') return;

    CsvRewrite rw;
    int ok = 0;
    db_wrlock(DB_FEEDBACK);
    if (csv_rewrite_begin(&rw, FEEDBACK_CSV, FEEDBACK_TMP) == 0) {
        char line[512];
        if (!fgets(line, sizeof(line), rw.in)) line[0] = '\0';
        fputs(FEEDBACK_HEADER, rw.out);
        while (fgets(line, sizeof(line), rw.in)) {
            FeedbackRecord f;
            trim_string(line);
            if (!parse_feedback_line(line, &f)) {
                if (line[0]) fprintf(rw.out, "%s\n", line);
                continue;
            }
            f.reviewed = 1;
            write_feedback_line(rw.out, &f);
        }
        ok = (csv_rewrite_commit(&rw) == 0);
    }
    db_unlock(DB_FEEDBACK);

    send_message(sock, ok ? "All feedback marked as reviewed\n" : "Error updating feedback\n");
}

void handle_manager(int sock, int mid) {
    char buffer[BUFFER_SIZE];

    for (;;) {
        send_message(sock,
            "\n=== Manager Menu ===\n"
            "1. Activate/Deactivate Customer Accounts\n"
            "2. Assign Loan Application Processes to Employees\n"
            "3. Review Customer Feedback\n"
            "4. Change Password\n"
            "5. Logout\n"
            "Choice: ");

        if (!read_input(sock, buffer, BUFFER_SIZE)) return;
        int choice;
        if (!parse_int(buffer, &choice)) choice = -1;

        switch (choice) {
            case 1: activate_account(sock); break;
            case 2: assign_loan(sock); break;
            case 3: view_feedback(sock); break;
            case 4: change_password(sock, ROLE_MANAGER, mid); break;
            case 5: return;
            default: send_message(sock, "Invalid choice\n");
        }
    }
}
