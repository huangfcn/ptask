/* bounded queue (ring buffer queue) */

#include <unistd.h>
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>

#include "libfiber.h"
#include "fiberq.h"

#define copyint(from, to) do {*to = *from;} while(0)
FIBERQ_PROTOTYPE_STATIC(fiberbq, int, copyint, FIBER_TIMEOUT_INFINITE, 8);

static fiberbq_t bq;

#define NUM_PRODUCERS   (8)
#define NUM_CONSUMERS   (8)

// producer is fast
void *producer(void *arg)
{
    fiberbq_t * the_q = (fiberbq_t *)(&bq);
    int index = (int)(int64_t)(arg);
    while (true) {
        int i = rand() % 26;

        int val = 'A' + i;
        fiberbq_push(the_q, &val);
        // if ((index & 8191) == 0)
            printf("producer %2d: sent %c\n", index, val);

        int timo = rand() % 2500 + 500;
        fiber_usleep(timo * 1000);
        // fiber_sched_yield();
    }
    return NULL;
}

// consumer is slow
void *consumer(void *arg)
{
    fiberbq_t * the_q = (fiberbq_t *)(&bq);
    int index = (int)(int64_t)(arg);
    while (true) {
        int val;
        fiberbq_pop(the_q, &val);
        // if ((index & 8191) == 4095)
            printf("consumer %2d: received %c\n", index, val);
        int timo = rand() % 2500 + 500;
        fiber_usleep(timo * 1000);
        // fiber_sched_yield();
    }
    return NULL;
}

/* create a fully loaded thread, it will push loading to other threads */
bool initializeTasks(void * args)
{
    int n = (NUM_PRODUCERS < NUM_CONSUMERS) ? (NUM_PRODUCERS) : (NUM_CONSUMERS);
    for (int64_t i = 0; i < n; ++i){
        fiber_create(&producer, (void *)i, NULL, 8192);
        fiber_create(&consumer, (void *)i, NULL, 8192);
    }

    for (int64_t i = n; i < NUM_PRODUCERS; ++i)
        fiber_create(&producer, (void *)i, NULL, 8192);

    for (int64_t i = n; i < NUM_CONSUMERS; ++i)
        fiber_create(&consumer, (void *)i, NULL, 8192);

    return true;
}

/* create idle thread to accept loading from other threads */
bool initializeTasks2(void * args)
{
    // for (int64_t i = 0; i < NUM_PRODUCERS; ++i)
    //     fiber_create(&producer, (void *)i, NULL, 8192);

    // for (int64_t i = 0; i < NUM_CONSUMERS; ++i)
    //     fiber_create(&consumer, (void *)i, NULL, 8192);

    return true;
}

int main(int argc, char ** argv){
    FiberGlobalStartup();

    fiberbq_init(&bq);

    pthread_t tid;
    /* create some service threads and wait it running */
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);

    fibthread_args_t args = {
      .threadStartup = initializeTasks,
      .threadCleanup = NULL,
      .args = (void *)(&bq),
    };

    pthread_scheduler(&args);

    return (0);
}
