#include "employee.h"
#include "auth.h"
#include "common.h"
#include "records.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add_customer(int sock) {
    char buf[BUFFER_SIZE];
    char username[FIELD_MAX], password[FIELD_MAX];
    double balance;

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

    send_message(sock, "Initial balance: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_amount(buf, &balance, 1)) {
        send_message(sock, "Invalid balance\n");
        return;
    }

    int new_id = -1;
    const char *err = NULL;

    db_wrlock(DB_CUSTOMERS);
    if (find_account_by_username(ROLE_CUSTOMER, username, NULL)) {
        err = "Username already exists\n";
    } else {
        new_id = get_next_id_csv(CUSTOMERS_CSV);
        FILE *fp = fopen(CUSTOMERS_CSV, "a");
        if (!fp) {
            err = "Error opening file\n";
        } else {
            AccountRecord r = { .id = new_id, .balance = balance, .active = 1 };
            safe_copy(r.username, username, sizeof(r.username));
            safe_copy(r.password, password, sizeof(r.password));
            write_account_line(ROLE_CUSTOMER, fp, &r);
            fclose(fp);
        }
    }
    db_unlock(DB_CUSTOMERS);

    if (err) { send_message(sock, err); return; }
    if (balance > 0) log_transaction(new_id, "OPENING_DEPOSIT", balance, balance);

    snprintf(buf, sizeof(buf), "Customer created! ID: %d\n", new_id);
    send_message(sock, buf);
}

static void view_assigned_loans(int sock, int eid) {
    char emp_id_str[20];
    snprintf(emp_id_str, sizeof(emp_id_str), "%d", eid);

    StrBuf out;
    sb_init(&out);
    int count = 0;
    sb_appendf(&out, "\n=== Assigned Loans ===\n");

    db_rdlock(DB_LOANS);
    FILE *fp = fopen(LOANS_CSV, "r");
    if (fp) {
        char line[300];
        if (fgets(line, sizeof(line), fp)) {
            while (fgets(line, sizeof(line), fp)) {
                LoanRecord l;
                trim_string(line);
                if (parse_loan_line(line, &l) && strcmp(l.assigned, emp_id_str) == 0) {
                    sb_appendf(&out, "Loan ID: %d | Customer: %d | Amount: %.2f | Type: %s | "
                                     "Tenure: %dy | Status: %s\n",
                               l.loan_id, l.customer_id, l.amount, l.type, l.tenure, l.status);
                    count++;
                }
            }
        }
        fclose(fp);
    }
    db_unlock(DB_LOANS);

    if (count == 0) sb_appendf(&out, "No loans assigned to you\n");
    if (out.data) send_message(sock, out.data);
    sb_free(&out);
}

/* Caller holds db_wrlock(DB_CUSTOMERS). Returns 0 on success. */
static int credit_customer(int cid, double amount, double *new_balance) {
    CsvRewrite rw;
    if (csv_rewrite_begin(&rw, CUSTOMERS_CSV, role_tmp_path(ROLE_CUSTOMER)) != 0) return -1;

    char line[256];
    int found = 0;
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
            r.balance += amount;
            *new_balance = r.balance;
            found = 1;
        }
        write_account_line(ROLE_CUSTOMER, rw.out, &r);
    }

    if (!found) { csv_rewrite_abort(&rw); return -1; }
    return csv_rewrite_commit(&rw);
}

/* FIXES vs. old version:
 *  - loans.csv and customers.csv were rewritten with NO locking at all
 *  - an already approved loan could be approved again -> money credited twice
 *  - a "DEBUG: ..." line was sent to the client
 *  - if crediting failed, the loan stayed "approved" anyway
 * Now: lock loans -> customers (lock order), build the new loans file, credit
 * the customer, and only if that worked rename the new loans file in. */
static void process_loan(int sock, int eid) {
    char buf[BUFFER_SIZE];
    int lid, approve;

    send_message(sock, "Enter loan ID: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &lid) || lid <= 0) { send_message(sock, "Invalid loan ID\n"); return; }

    send_message(sock, "Approve (1) or Reject (0): ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &approve) || (approve != 0 && approve != 1)) {
        send_message(sock, "Please enter 1 or 0\n");
        return;
    }

    char emp_id_str[20];
    snprintf(emp_id_str, sizeof(emp_id_str), "%d", eid);

    enum { NOT_FOUND, NOT_YOURS, NOT_PENDING, DONE, CREDIT_FAIL, IO_FAIL } status = NOT_FOUND;
    LoanRecord target;
    double new_balance = 0;
    CsvRewrite rw;

    db_wrlock(DB_LOANS);
    if (csv_rewrite_begin(&rw, LOANS_CSV, LOANS_TMP) != 0) {
        db_unlock(DB_LOANS);
        send_message(sock, "Error opening loans file\n");
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
            if (strcmp(l.assigned, emp_id_str) != 0)   status = NOT_YOURS;
            else if (strcmp(l.status, "pending") != 0) status = NOT_PENDING;
            else {
                safe_copy(l.status, approve ? "approved" : "rejected", sizeof(l.status));
                target = l;
                status = DONE;
            }
        }
        write_loan_line(rw.out, &l);
    }

    if (status != DONE) {
        csv_rewrite_abort(&rw);
    } else if (approve) {
        db_wrlock(DB_CUSTOMERS);
        if (credit_customer(target.customer_id, target.amount, &new_balance) != 0) {
            status = CREDIT_FAIL;
            csv_rewrite_abort(&rw);                 /* keep loan pending */
        } else if (csv_rewrite_commit(&rw) != 0) {
            status = IO_FAIL;
        } else {
            log_transaction(target.customer_id, "LOAN_CREDIT", target.amount, new_balance);
        }
        db_unlock(DB_CUSTOMERS);
    } else if (csv_rewrite_commit(&rw) != 0) {
        status = IO_FAIL;
    }
    db_unlock(DB_LOANS);

    switch (status) {
        case DONE:
            if (approve) {
                snprintf(buf, sizeof(buf),
                         "Loan approved! Amount %.2f credited to customer %d's account\n",
                         target.amount, target.customer_id);
                send_message(sock, buf);
            } else {
                send_message(sock, "Loan rejected\n");
            }
            break;
        case NOT_YOURS:   send_message(sock, "This loan is not assigned to you\n"); break;
        case NOT_PENDING: send_message(sock, "This loan has already been processed\n"); break;
        case CREDIT_FAIL: send_message(sock, "Could not credit customer account; loan left pending\n"); break;
        case IO_FAIL:     send_message(sock, "Error saving loan file\n"); break;
        default:          send_message(sock, "Loan not found\n"); break;
    }
}

static void view_customer_transactions(int sock) {
    char buf[BUFFER_SIZE];
    int cid;

    send_message(sock, "Enter customer ID: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &cid) || cid <= 0) { send_message(sock, "Invalid ID\n"); return; }

    StrBuf out;
    sb_init(&out);
    int count = 0;
    sb_appendf(&out, "\n=== Customer Transaction History (Passbook) ===\n");

    db_rdlock(DB_TRANSACTIONS);
    FILE *fp = fopen(TRANSACTIONS_CSV, "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            while (fgets(line, sizeof(line), fp)) {
                int customer_id;
                char type[20], timestamp[50];
                double amount, balance_after;
                trim_string(line);
                if (sscanf(line, "%d,%19[^,],%lf,%lf,%49[^\n]",
                           &customer_id, type, &amount, &balance_after, timestamp) == 5 &&
                    customer_id == cid) {
                    sb_appendf(&out, "%s: %.2f | Balance: %.2f | %s\n",
                               type, amount, balance_after, timestamp);
                    count++;
                }
            }
        }
        fclose(fp);
    }
    db_unlock(DB_TRANSACTIONS);

    if (count == 0) sb_appendf(&out, "No transactions found for this customer\n");
    if (out.data) send_message(sock, out.data);
    sb_free(&out);
}

void handle_employee(int sock, int eid) {
    char buffer[BUFFER_SIZE];

    for (;;) {
        send_message(sock,
            "\n=== Employee Menu ===\n"
            "1. Add New Customer\n"
            "2. Modify Customer Details\n"
            "3. Process Loan Applications\n"
            "4. View Assigned Loan Applications\n"
            "5. View Customer Transactions (Passbook)\n"
            "6. Change Password\n"
            "7. Logout\n"
            "Choice: ");

        if (!read_input(sock, buffer, BUFFER_SIZE)) return;
        int choice;
        if (!parse_int(buffer, &choice)) choice = -1;

        switch (choice) {
            case 1: add_customer(sock); break;
            case 2: modify_account_details(sock, ROLE_CUSTOMER); break;
            case 3: process_loan(sock, eid); break;
            case 4: view_assigned_loans(sock, eid); break;
            case 5: view_customer_transactions(sock); break;
            case 6: change_password(sock, ROLE_EMPLOYEE, eid); break;
            case 7: return;
            default: send_message(sock, "Invalid choice\n");
        }
    }
}
