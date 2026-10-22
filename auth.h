#ifndef AUTH_H
#define AUTH_H

/* authenticate() result codes */
#define AUTH_OK                 1
#define AUTH_FAIL               0   /* wrong username / password          */
#define AUTH_ALREADY_LOGGED_IN -1   /* same account active in another session */
#define AUTH_INACTIVE          -2   /* customer account deactivated       */

int  authenticate(int role, const char *username, const char *password, int *user_id);
void logout_user(int role, int user_id);

/* Shared credential operations (were duplicated in 4 files before) */
void change_password(int sock, int role, int id);
void modify_account_details(int sock, int role);   /* customers / employees */

#endif
