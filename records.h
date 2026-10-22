#ifndef RECORDS_H
#define RECORDS_H

/* ---------------------------------------------------------------------------
 * One place that knows the CSV layout of every data file.
 * Before this, the same sscanf()/fprintf() format strings were copy-pasted
 * in ~15 functions across customer.c, employee.c, manager.c and admin.c.
 * ------------------------------------------------------------------------- */

#include "common.h"
#include "utils.h"
#include <stdio.h>

#define CUSTOMERS_CSV     DATA_DIR "/customers.csv"
#define EMPLOYEES_CSV     DATA_DIR "/employees.csv"
#define MANAGERS_CSV      DATA_DIR "/managers.csv"
#define ADMINS_CSV        DATA_DIR "/admins.csv"
#define LOANS_CSV         DATA_DIR "/loans.csv"
#define LOANS_TMP         DATA_DIR "/loans.tmp"
#define FEEDBACK_CSV      DATA_DIR "/feedback.csv"
#define FEEDBACK_TMP      DATA_DIR "/feedback.tmp"
#define TRANSACTIONS_CSV  DATA_DIR "/transactions.csv"

#define LOANS_HEADER      "loan_id,customer_id,amount,loan_type,tenure,status,assigned_employee\n"
#define FEEDBACK_HEADER   "feedback_id,customer_id,message,reviewed\n"

/* Account row: customers have balance/active, staff rows only id/user/pass */
typedef struct {
    int    id;
    char   username[FIELD_MAX];
    char   password[FIELD_MAX];
    double balance;
    int    active;
} AccountRecord;

typedef struct {
    int    loan_id;
    int    customer_id;
    double amount;
    char   type[20];
    int    tenure;
    char   status[20];       /* pending / approved / rejected            */
    char   assigned[20];     /* employee id as text, or "unassigned"      */
} LoanRecord;

typedef struct {
    int  id;
    int  customer_id;
    char message[256];
    int  reviewed;
} FeedbackRecord;

int         role_is_valid(int role);
const char *role_name(int role);
DbFile      role_db(int role);
const char *role_csv_path(int role);
const char *role_tmp_path(int role);
const char *role_csv_header(int role);

int  parse_account_line(int role, const char *line, AccountRecord *r);
void write_account_line(int role, FILE *out, const AccountRecord *r);

/* Caller must hold db_rdlock/db_wrlock(role_db(role)). Return 1 if found. */
int  find_account_by_id(int role, int id, AccountRecord *out);
int  find_account_by_username(int role, const char *username, AccountRecord *out);

int  parse_loan_line(const char *line, LoanRecord *l);
void write_loan_line(FILE *out, const LoanRecord *l);

int  parse_feedback_line(const char *line, FeedbackRecord *f);
void write_feedback_line(FILE *out, const FeedbackRecord *f);

#endif
