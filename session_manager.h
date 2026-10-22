#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include "common.h"

int  init_session_manager(void);       /* 0 on success, -1 on failure          */
void destroy_session_manager(void);    /* detach + remove shm and semaphore    */
int  check_and_register_login(int role, int user_id);
void register_logout(int role, int user_id);
int  count_active_sessions(void);

#endif
