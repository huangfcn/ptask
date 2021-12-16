#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "libfiber.h"

///////////////////////////////////////////////////////////////////////////////
/* RWLOCK implementation with mutex & CV                                     */
///////////////////////////////////////////////////////////////////////////////
typedef struct rwlock_t {
    fiber_mutex_t mutex;
    fiber_cond_t  unlocked;

    bool writer;
    int  readers;
} rwlock_t;

int rwlock_init(rwlock_t * locker){
    fiber_mutex_init(&(locker->mutex));
    fiber_cond_init(&(locker->unlocked));

    locker->writer = false;
    locker->readers = 0;
    return 0;
}

int rwlock_rdlock(rwlock_t * locker) {
    fiber_mutex_lock(&(locker->mutex));
    while (locker->writer)
        fiber_cond_wait(&(locker->unlocked), &(locker->mutex));
    locker->readers++;
    fiber_mutex_unlock(&(locker->mutex));

    return (0);
}

int rwlock_rdunlock(rwlock_t * locker) {
    fiber_mutex_lock(&(locker->mutex));
    locker->readers--;
    if (locker->readers == 0)
        fiber_cond_broadcast(&(locker->unlocked));
    fiber_mutex_unlock(&(locker->mutex));

    return 0;
}

int rwlock_wrlock(rwlock_t * locker) {
    fiber_mutex_lock(&(locker->mutex));
    while (locker->writer || locker->readers)
        fiber_cond_wait(&(locker->unlocked), &(locker->mutex));
    locker->writer = true;
    fiber_mutex_unlock(&(locker->mutex));

    return 0;
}

int rwlock_wrunlock(rwlock_t * locker) {
    fiber_mutex_lock(&(locker->mutex));
    locker->writer = false;
    fiber_cond_broadcast(&(locker->unlocked));
    fiber_mutex_unlock(&(locker->mutex));

    return 0;
}

int rwlock_destroy(rwlock_t * locker){
    fiber_mutex_destroy(&(locker->mutex));
    fiber_cond_destroy(&(locker->unlocked));

    return 0;
}
///////////////////////////////////////////////////////////////////////////////

#define MAXTHREAD 8  /* define # readers */

void* reader(void*);
void* writer(void*);

struct _params_t {
    rwlock_t rwlock;
    int64_t  ids[MAXTHREAD];
    int      dbase[MAXTHREAD];
} params;

bool initializeTasks(void* args) {

    for (int index = 0; index < MAXTHREAD; index++)
    {
        params.ids[index] = index + 1;  /* create readers and error check */
        fiber_create(reader, &(params.ids[index]), NULL, 8192);
    }

    fiber_create(writer, NULL, NULL, 8192);
    return true;
}

int main(){
    FiberGlobalStartup();
    rwlock_init(&(params.rwlock));

    /* run another thread */
    fibthread_args_t args = {
      .init_func = initializeTasks,
      .args = (void *)(0),
    };
    pthread_scheduler(&args);

    return (0);
}

void* reader(void* arg)   /* readers function to read */
{
    int64_t index = *(int64_t *)arg;

    rwlock_t * the_lock = &(params.rwlock);
    while (1) {
        rwlock_rdlock(the_lock);

        int value = params.dbase[index-1];
        printf("Fiber %ld read %d\n", index - 1, value);


        rwlock_rdunlock(the_lock);
        
        int msec = rand() % (300 * index) + 300;
        fiber_usleep(msec * 1000);
    }
    return 0;
};

void* writer(void* arg)      /* writer's function to write */
{
    rwlock_t * the_lock = &(params.rwlock);
    while (1) {
        rwlock_wrlock(the_lock);
    
        for (int i = 0; i < MAXTHREAD; i++) {
            params.dbase[i] = rand() % 1000;
            printf("...writing: dbase[%d] = %d\n", i, params.dbase[i]);
        }

        rwlock_wrunlock(the_lock);

        int msec = rand() % (300 * 4) + 300;
        fiber_usleep(msec * 1000);
    }
    return 0;
}