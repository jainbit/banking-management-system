#include "session_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>

/* The session table lives in System V shared memory and is guarded by a
 * System V binary semaphore. It enforces "one login per user at a time". */
static SessionManager *sessions = NULL;
static int shm_id = -1;
static int sem_session = -1;

#if defined(__linux__)
/* Linux requires the caller to define union semun; macOS defines it. */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};
#endif

/* semop() can be interrupted by a signal (EINTR) - just retry. */
static void sem_lock(void) {
    struct sembuf op = {0, -1, SEM_UNDO};
    while (semop(sem_session, &op, 1) == -1 && errno == EINTR) { }
}

static void sem_unlock(void) {
    struct sembuf op = {0, 1, SEM_UNDO};
    while (semop(sem_session, &op, 1) == -1 && errno == EINTR) { }
}

int init_session_manager(void) {
    /* IPC_EXCL: fail if the segment already exists, so we can find out WHO
     * owns it instead of blindly zeroing another server's session table. */
    shm_id = shmget(SHM_KEY_SESSIONS, sizeof(SessionManager), IPC_CREAT | IPC_EXCL | 0666);

    if (shm_id == -1 && errno == EEXIST) {
        int existing = shmget(SHM_KEY_SESSIONS, 0, 0666);
        struct shmid_ds ds;

        if (existing != -1 && shmctl(existing, IPC_STAT, &ds) == 0) {
            if (ds.shm_nattch > 0) {
                /* A live process is attached -> another server is running. */
                fprintf(stderr, "Another server instance is already running "
                                "(shared memory key %d is attached by %lu process(es)).\n",
                        SHM_KEY_SESSIONS, (unsigned long)ds.shm_nattch);
                return -1;
            }
            /* Nobody attached: stale segment left behind by `kill -9`. */
            if (ds.shm_segsz == sizeof(SessionManager)) {
                shm_id = existing;
                printf("  (re-using stale shared memory left by a previous crash)\n");
            } else {
                shmctl(existing, IPC_RMID, NULL);   /* old build, wrong size */
                shm_id = shmget(SHM_KEY_SESSIONS, sizeof(SessionManager),
                                IPC_CREAT | IPC_EXCL | 0666);
            }
        }
    }
    if (shm_id == -1) {
        perror("shmget");
        return -1;
    }

    void *addr = shmat(shm_id, NULL, 0);
    if (addr == (void *)-1) {
        perror("shmat");
        return -1;
    }
    sessions = (SessionManager *)addr;
    memset(sessions, 0, sizeof(SessionManager));   /* fresh start: nobody logged in */

    sem_session = semget(SHM_KEY_SESSIONS + 1, 1, IPC_CREAT | 0666);
    if (sem_session == -1) {
        perror("semget");
        return -1;
    }

    union semun arg;
    arg.val = 1;                                     /* 1 = unlocked */
    if (semctl(sem_session, 0, SETVAL, arg) == -1) {
        perror("semctl(SETVAL)");
        return -1;
    }

    printf("✓ Session manager initialized (shm key %d, sem key %d)\n",
           SHM_KEY_SESSIONS, SHM_KEY_SESSIONS + 1);
    return 0;
}

/* Called during graceful shutdown. Without this the shared memory segment
 * and semaphore stay in the kernel after the server exits (check with
 * `ipcs -m -s`). */
void destroy_session_manager(void) {
    if (shm_id != -1) {
        shmctl(shm_id, IPC_RMID, NULL);   /* destroyed after last detach */
        shm_id = -1;
    }
    if (sem_session != -1) {
        semctl(sem_session, 0, IPC_RMID);
        sem_session = -1;
    }
    if (sessions) {
        shmdt(sessions);
        sessions = NULL;
    }
    printf("✓ Shared memory segment and semaphore removed\n");
}

int check_and_register_login(int role, int user_id) {
    if (!sessions) return 0;
    sem_lock();

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions->sessions[i].active &&
            sessions->sessions[i].role == role &&
            sessions->sessions[i].user_id == user_id) {
            sem_unlock();
            return 0;                       /* already logged in elsewhere */
        }
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions->sessions[i].active) {
            sessions->sessions[i].role = role;
            sessions->sessions[i].user_id = user_id;
            sessions->sessions[i].active = 1;
            sem_unlock();
            return 1;
        }
    }

    sem_unlock();
    return 0;                               /* table full */
}

void register_logout(int role, int user_id) {
    if (!sessions) return;
    sem_lock();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions->sessions[i].active &&
            sessions->sessions[i].role == role &&
            sessions->sessions[i].user_id == user_id) {
            sessions->sessions[i].active = 0;
            break;
        }
    }
    sem_unlock();
}

int count_active_sessions(void) {
    if (!sessions) return 0;
    int n = 0;
    sem_lock();
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions->sessions[i].active) n++;
    sem_unlock();
    return n;
}
