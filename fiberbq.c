/* bounded queue using condition variables */

#include <unistd.h>
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>

#include "libfiber.h"
#include "fiberq.h"

#define copyint(from, to) do {*to = *from;} while(0)
FIBERQ_PROTOTYPE_STATIC(fiberbq, int, copyint, FIBER_TIMEOUT_WAITFOREVER, 128);


static fiberbq_t bq;

#define NUM_PRODUCERS   8
#define NUM_CONSUMERS   8

// producer is fast
void *producer(void *arg)
{
    fiberbq_t * the_q = (fiberbq_t *)(&bq);
    int index = (int)(int64_t)(arg);
    while (true) {
        int i = rand() % 26;

        int val = 'A' + i;
        fiberbq_push(the_q, &val);
        printf("producer %2d: sent %c\n", index, val);

        int timo = rand() % 2500 + 500;
        fiber_usleep(timo * 1000);
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
        printf("consumer %2d: received %c\n", index, val);
        int timo = rand() % 3000 + 500;
        fiber_usleep(timo * 1000);
    }
    return NULL;
}

bool initializeTasks(void * args)
{
    for (int64_t i = 0; i < NUM_PRODUCERS; ++i)
        fiber_create(&producer, (void *)i, NULL, 8192);

    for (int64_t i = 0; i < NUM_CONSUMERS; ++i)
        fiber_create(&consumer, (void *)i, NULL, 8192);

    return true;
}

int main(){
    FiberGlobalStartup();

    fiberbq_init(&bq);

    /* run another thread */
    fibthread_args_t args = {
      .init_func = initializeTasks,
      .args = (void *)(&bq),
    };
    pthread_scheduler(&args);

    return (0);
}
