#ifndef COMMON_H
#define COMMON_H

/* ---------------------------------------------------------------------------
 * Network / server configuration
 * ------------------------------------------------------------------------- */
#define PORT                     8080   /* default port (override: ./server <port>) */
#define BUFFER_SIZE              1024
#define DATA_DIR                 "data"
#define MAX_SESSIONS             100    /* slots in the shared-memory session table   */
#define MAX_CLIENTS              100    /* simultaneous TCP connections               */
#define SHM_KEY_SESSIONS         9999   /* System V key: shm = 9999, semaphore = 10000 */

/* Interrupt / signal related tunables */
#ifndef CLIENT_IDLE_TIMEOUT_SEC
#define CLIENT_IDLE_TIMEOUT_SEC  300    /* idle client is logged out after 5 minutes  */
#endif
#define SHUTDOWN_GRACE_SEC       5      /* how long the server waits for threads      */

/* Input limits */
#define FIELD_MAX                50     /* max length (incl. '\0') of username/password */
#define MAX_AMOUNT               1000000000.0  /* sanity cap for money values        */

/* ---------------------------------------------------------------------------
 * Data types (kept from the original design / class diagram)
 * ------------------------------------------------------------------------- */
typedef struct {
    int id;
    char username[FIELD_MAX];
    char password[FIELD_MAX];
    double balance;
    int active;
} Customer;

typedef struct {
    int id;
    char username[FIELD_MAX];
    char password[FIELD_MAX];
} Employee;

typedef struct {
    int id;
    char username[FIELD_MAX];
    char password[FIELD_MAX];
} Manager;

typedef struct {
    int id;
    char username[FIELD_MAX];
    char password[FIELD_MAX];
} Admin;

typedef struct {
    int customer_id;
    char type[20];
    double amount;
    double balance_after;
    char timestamp[50];
} Transaction;

typedef struct {
    int user_id;
    int role;
    int active;
} Session;

typedef struct {
    Session sessions[MAX_SESSIONS];
} SessionManager;

#define ROLE_CUSTOMER 1
#define ROLE_EMPLOYEE 2
#define ROLE_MANAGER  3
#define ROLE_ADMIN    4

#endif
