#ifndef SS_PROJECT_SERVER_H
#define SS_PROJECT_SERVER_H

/* Prints connected clients / logged-in sessions / uptime.
 * Triggered by: kill -USR1 <server-pid> */
void print_server_status(void);

#endif /* SS_PROJECT_SERVER_H */
