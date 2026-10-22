#include "customer.h"
#include "auth.h"
#include "common.h"
#include "records.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Rule followed everywhere below: take the lock, touch the file, release the
 * lock, THEN talk to the client. Never block on the network with a lock held. */

static void view_balance(int sock, int cid) {
    AccountRecord r;
    char msg[100];

    db_rdlock(DB_CUSTOMERS);
    int found = find_account_by_id(ROLE_CUSTOMER, cid, &r);
    db_unlock(DB_CUSTOMERS);

    if (found) snprintf(msg, sizeof(msg), "Balance: %.2f\n", r.balance);
    else       snprintf(msg, sizeof(msg), "Error: account not found\n");
    send_message(sock, msg);
}

/* status codes for balance operations */
enum { OP_NOT_FOUND = 0, OP_OK, OP_INSUFFICIENT, OP_INACTIVE, OP_IO_ERROR,
       OP_NO_RECIPIENT, OP_RECIPIENT_INACTIVE };

static void update_balance(int sock, int cid, double amount, int is_deposit) {
    int status = OP_NOT_FOUND;
    double new_balance = 0;
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
            if (line[0]) fprintf(rw.out, "%s\n", line);  /* never drop unknown rows */
            continue;
        }
        if (r.id == cid) {
            if (!r.active) { status = OP_INACTIVE; break; }
            if (is_deposit) {
                r.balance += amount;
            } else if (r.balance < amount) {
                status = OP_INSUFFICIENT;
                break;
            } else {
                r.balance -= amount;
            }
            new_balance = r.balance;
            status = OP_OK;
        }
        write_account_line(ROLE_CUSTOMER, rw.out, &r);
    }

    if (status == OP_OK) {
        if (csv_rewrite_commit(&rw) == 0)
            log_transaction(cid, is_deposit ? "DEPOSIT" : "WITHDRAWAL", amount, new_balance);
        else
            status = OP_IO_ERROR;
    } else {
        csv_rewrite_abort(&rw);
    }
    db_unlock(DB_CUSTOMERS);

    char msg[BUFFER_SIZE];
    switch (status) {
        case OP_OK:
            snprintf(msg, sizeof(msg), "Success! New balance: %.2f\n", new_balance); break;
        case OP_INSUFFICIENT: safe_copy(msg, "Insufficient balance\n", sizeof(msg)); break;
        case OP_INACTIVE:     safe_copy(msg, "Account is deactivated\n", sizeof(msg)); break;
        case OP_IO_ERROR:     safe_copy(msg, "Error saving changes\n", sizeof(msg)); break;
        default:              safe_copy(msg, "Error: account not found\n", sizeof(msg)); break;
    }
    send_message(sock, msg);
}

static void transfer_funds(int sock, int cid) {
    char buf[BUFFER_SIZE];
    int recipient_id;
    double amount;

    send_message(sock, "Enter recipient ID: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &recipient_id) || recipient_id <= 0) {
        send_message(sock, "Invalid recipient ID\n");
        return;
    }

    send_message(sock, "Enter amount: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_amount(buf, &amount, 0) || recipient_id == cid) {
        send_message(sock, "Invalid transfer\n");
        return;
    }

    int sender_found = 0, recipient_found = 0, status = OP_OK;
    double sender_balance = 0, recipient_balance = 0;
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

    while (status == OP_OK && fgets(line, sizeof(line), rw.in)) {
        AccountRecord r;
        trim_string(line);
        if (!parse_account_line(ROLE_CUSTOMER, line, &r)) {
            if (line[0]) fprintf(rw.out, "%s\n", line);
            continue;
        }
        if (r.id == cid) {
            sender_found = 1;
            if (!r.active)               status = OP_INACTIVE;
            else if (r.balance < amount) status = OP_INSUFFICIENT;
            else { r.balance -= amount; sender_balance = r.balance; }
        } else if (r.id == recipient_id) {
            recipient_found = 1;
            if (!r.active) status = OP_RECIPIENT_INACTIVE;
            else { r.balance += amount; recipient_balance = r.balance; }
        }
        write_account_line(ROLE_CUSTOMER, rw.out, &r);
    }

    if (status == OP_OK && !sender_found)    status = OP_NOT_FOUND;
    if (status == OP_OK && !recipient_found) status = OP_NO_RECIPIENT;

    if (status == OP_OK) {
        if (csv_rewrite_commit(&rw) == 0) {
            /* Both rows are updated in ONE atomic rename -> money can never be
             * debited without being credited. */
            log_transaction(cid, "TRANSFER_OUT", amount, sender_balance);
            log_transaction(recipient_id, "TRANSFER_IN", amount, recipient_balance);
        } else {
            status = OP_IO_ERROR;
        }
    } else {
        csv_rewrite_abort(&rw);
    }
    db_unlock(DB_CUSTOMERS);

    switch (status) {
        case OP_OK:
            snprintf(buf, sizeof(buf), "Transfer successful! Balance: %.2f\n", sender_balance);
            send_message(sock, buf);
            break;
        case OP_INSUFFICIENT:       send_message(sock, "Insufficient balance\n"); break;
        case OP_INACTIVE:           send_message(sock, "Your account is deactivated\n"); break;
        case OP_RECIPIENT_INACTIVE: send_message(sock, "Recipient account is deactivated\n"); break;
        case OP_NO_RECIPIENT:       send_message(sock, "Recipient account not found\n"); break;
        case OP_IO_ERROR:           send_message(sock, "Error saving changes\n"); break;
        default:                    send_message(sock, "Invalid accounts\n"); break;
    }
}

/* Accepts Personal/Home/Education (any case) or just P/H/E. */
static int normalize_loan_type(const char *in, char *out, size_t n) {
    static const char *types[] = { "Personal", "Home", "Education" };
    for (size_t i = 0; i < 3; i++) {
        if (strcasecmp(in, types[i]) == 0 ||
            (strlen(in) == 1 && (in[0] | 0x20) == (types[i][0] | 0x20))) {
            safe_copy(out, types[i], n);
            return 1;
        }
    }
    return 0;
}

static void apply_loan(int sock, int cid) {
    char buf[BUFFER_SIZE];
    double amount;
    int tenure;
    char loan_type[20];

    send_message(sock, "=== Welcome to Loan Portal ===\n");

    send_message(sock, "Enter loan amount: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_amount(buf, &amount, 0)) {
        send_message(sock, "Invalid amount\n");
        return;
    }

    send_message(sock, "Enter loan type (Personal/Home/Education): ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!normalize_loan_type(buf, loan_type, sizeof(loan_type))) {
        send_message(sock, "Invalid loan type\n");
        return;
    }

    send_message(sock, "Enter tenure (years, 1-30): ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (!parse_int(buf, &tenure) || tenure <= 0 || tenure > 30) {
        send_message(sock, "Invalid tenure\n");
        return;
    }

    int loan_id = -1;

    /* id generation + append under ONE write lock, otherwise two customers
     * applying at the same moment could get the same loan id. */
    db_wrlock(DB_LOANS);
    loan_id = get_next_id_csv(LOANS_CSV);
    FILE *fp = fopen(LOANS_CSV, "a");
    if (fp) {
        LoanRecord l = { .loan_id = loan_id, .customer_id = cid, .amount = amount,
                         .tenure = tenure };
        safe_copy(l.type, loan_type, sizeof(l.type));
        safe_copy(l.status, "pending", sizeof(l.status));
        safe_copy(l.assigned, "unassigned", sizeof(l.assigned));
        write_loan_line(fp, &l);
        fclose(fp);
    } else {
        loan_id = -1;
    }
    db_unlock(DB_LOANS);

    if (loan_id < 0) {
        send_message(sock, "Error submitting loan\n");
        return;
    }
    snprintf(buf, sizeof(buf), "Loan application successful! Loan ID: %d\n", loan_id);
    send_message(sock, buf);
}

static void check_loan_status(int sock, int cid) {
    StrBuf out;
    sb_init(&out);
    int count = 0;

    sb_appendf(&out, "\n=== Your Loan Applications ===\n"
                     "Loan ID | Amount       | Type       | Tenure | Status    | Assigned To\n"
                     "---------------------------------------------------------------------\n");

    db_rdlock(DB_LOANS);
    FILE *fp = fopen(LOANS_CSV, "r");
    if (fp) {
        char line[300];
        if (fgets(line, sizeof(line), fp)) {
            while (fgets(line, sizeof(line), fp)) {
                LoanRecord l;
                trim_string(line);
                if (parse_loan_line(line, &l) && l.customer_id == cid) {
                    sb_appendf(&out, "%7d | %12.2f | %-10s | %6d | %-9s | %-12s\n",
                               l.loan_id, l.amount, l.type, l.tenure, l.status, l.assigned);
                    count++;
                }
            }
        }
        fclose(fp);
    }
    db_unlock(DB_LOANS);

    if (count == 0) sb_appendf(&out, "No loan applications found\n");
    if (out.data) send_message(sock, out.data);
    sb_free(&out);
}

static void add_feedback(int sock, int cid) {
    char buf[BUFFER_SIZE];

    send_message(sock, "Enter feedback message: ");
    if (!read_input(sock, buf, BUFFER_SIZE)) return;
    if (buf[0] == '\0') {
        send_message(sock, "Feedback cannot be empty\n");
        return;
    }

    FeedbackRecord f;
    memset(&f, 0, sizeof(f));
    f.customer_id = cid;
    safe_copy(f.message, buf, sizeof(f.message));   /* commas sanitised on write */

    db_wrlock(DB_FEEDBACK);
    f.id = get_next_id_csv(FEEDBACK_CSV);
    FILE *fp = fopen(FEEDBACK_CSV, "a");
    if (fp) {
        write_feedback_line(fp, &f);
        fclose(fp);
    }
    db_unlock(DB_FEEDBACK);

    send_message(sock, fp ? "Feedback submitted successfully\n" : "Error saving feedback\n");
}

static void view_transactions(int sock, int cid) {
    StrBuf out;
    sb_init(&out);
    int count = 0;

    sb_appendf(&out, "\n=== Transaction History ===\n");

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

    if (count == 0) sb_appendf(&out, "No transactions found\n");
    if (out.data) send_message(sock, out.data);
    sb_free(&out);
}

void handle_customer(int sock, int cid) {
    char buffer[BUFFER_SIZE];

    for (;;) {
        send_message(sock,
            "\n=== Customer Menu ===\n"
            "1. View Account Balance\n"
            "2. Deposit Money\n"
            "3. Withdraw Money\n"
            "4. Transfer Funds\n"
            "5. Apply for a Loan\n"
            "6. Check Loan Status\n"
            "7. Change Password\n"
            "8. Add Feedback\n"
            "9. View Transaction History\n"
            "10. Logout\n"
            "Choice: ");

        if (!read_input(sock, buffer, BUFFER_SIZE)) return;   /* disconnect */
        int choice;
        if (!parse_int(buffer, &choice)) choice = -1;

        double amount;
        switch (choice) {
            case 1: view_balance(sock, cid); break;
            case 2:
            case 3:
                send_message(sock, "Enter amount: ");
                if (!read_input(sock, buffer, BUFFER_SIZE)) return;
                /* FIX: negative / zero / non-numeric amounts used to be
                 * accepted -> "deposit -500" was a withdrawal that skipped
                 * the balance check. */
                if (!parse_amount(buffer, &amount, 0)) {
                    send_message(sock, "Invalid amount (must be a positive number)\n");
                    break;
                }
                update_balance(sock, cid, amount, choice == 2);
                break;
            case 4: transfer_funds(sock, cid); break;
            case 5: apply_loan(sock, cid); break;
            case 6: check_loan_status(sock, cid); break;
            case 7: change_password(sock, ROLE_CUSTOMER, cid); break;
            case 8: add_feedback(sock, cid); break;
            case 9: view_transactions(sock, cid); break;
            case 10: return;
            default: send_message(sock, "Invalid choice\n");
        }
    }
}
