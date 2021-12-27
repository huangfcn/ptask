/* Modified from https://cboard.cprogramming.com/c-programming/112999-reader-writer-problem-using-semaphores.html

   use the pthread flag with gcc to compile this code
   ~$ gcc -pthread reader_writer.c -o reader_writer
*/
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "task.h"

#define MAXTHREAD 8  /* define # readers */


int  access_database(int);     /* prototypes */
void non_access_database();
void write_database();

void* reader(void*);
void* writer(void*);

fiber_mutex_t mutex;    /* controls access to nreaders */
fiber_sem_t   db;       /* controls access to db */
fiber_sem_t   q;        /* establish queue */

fiber_t readers[MAXTHREAD], writerTh;
int64_t ids[MAXTHREAD]; /*initialize readers and initialize mutex, */

volatile int dbase[MAXTHREAD];
volatile int nreaders = 0;

bool initializeTasks(void* args) {

    fiber_mutex_init(&mutex);
    fiber_sem_init  (&db, 1);
    fiber_sem_init  (&q,  1);

    for (int index = 0; index < MAXTHREAD; index++)
    {
        ids[index] = index + 1;  /* create readers and error check */
        readers[index] = fiber_create(reader, &ids[index], NULL, 8192);
    }

    writerTh = fiber_create(writer, NULL, NULL, 8192);
    return true;
}

int main(){
    FiberGlobalStartup();

    /* run another thread */
    fibthread_args_t args = {
      .threadStartup = initializeTasks,
      .threadCleanup = NULL,
      .args = (void *)(0),
    };
    pthread_scheduler(&args);

    return (0);
}

void* reader(void* arg)   /* readers function to read */
{
    int64_t index = *(int64_t *)arg;
    while (1) {
        fiber_sem_wait(&q);
        fiber_mutex_lock(&mutex);
        nreaders++;
        if (nreaders == 1) fiber_sem_wait(&db);
        fiber_mutex_unlock(&mutex);

        int value = access_database(index - 1);
        fiber_sem_post(&q);
        printf("Fiber %ld read %d\n", index - 1, value);
        
        int msec = rand() % (100 * index) + 100;
        fiber_usleep(msec * 1000);

        fiber_mutex_lock(&mutex);
        nreaders--;
        if (nreaders == 0) fiber_sem_post(&db);
        fiber_mutex_unlock(&mutex);
        //non_access_database();
    }
    return 0;
};

void* writer(void* arg)      /* writer's function to write */
{
    while (1) {
        //non_access_database();
        fiber_sem_wait(&q);
        fiber_sem_wait(&db);

        write_database();
        fiber_sem_post(&q);
        printf("Writer is now writing...Number of readers: %d\n", nreaders);
        int msec = rand() % 300 + 50;
        fiber_usleep(msec * 1000);
        fiber_sem_post(&db);
    }
    return 0;
}

int access_database(int index)
{
    return dbase[index];
}


void non_access_database()
{
    return;
}

void write_database() {
    for (int i = 0; i < MAXTHREAD; i++) {
        dbase[i] = rand() % 1000;
        printf("...writing: dbase[%d] = %d\n", i, dbase[i]);
    }
    return;
}
