/* bounded queue using condition variables */

#include <unistd.h>
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>

#include "libfiber.h"

////////////////////////////////////////////////////////////
/* linked based queue                                     */
////////////////////////////////////////////////////////////
struct queue_node;
typedef struct queue_node queue_node_t;

/*
 * Queue data structure.
 */
typedef struct queue {
    queue_node_t *head, *tail;
} queue_t;

struct queue_node {
    void *item;
    struct queue_node *next;
};

void queue_init(queue_t *q)
{
    q->head = q->tail = NULL;
}

int queue_is_empty(queue_t *q)
{
    return q->head == NULL;
}

void queue_enqueue(queue_t *q, void *item)
{
    queue_node_t *node;

    node = malloc(sizeof(queue_node_t));
    node->item = item;
    node->next = NULL;

    if (q->tail != NULL) {
        q->tail->next = node;
        q->tail = node;
    } else {
        q->head = q->tail = node;
    }
}

void *queue_dequeue(queue_t *q)
{
    void *item;
    queue_node_t *old_head;

    assert(!queue_is_empty(q));

    item = q->head->item;
    old_head = q->head;
    q->head = q->head->next;
    free(old_head);
    if (q->head == NULL) {
        q->tail = NULL;
    }

    return item;
}

////////////////////////////////////////////////////////////

typedef struct {
    queue_t q;
    int max;   // max number of items
    int count; // current number of items

    fiber_mutex_t lock;
    fiber_cond_t  cond;
} BoundedQueue;

void boundedqueue_init(BoundedQueue* bq, int max);
void boundedqueue_enqueue(BoundedQueue* bq, void *item);
void* boundedqueue_dequeue(BoundedQueue* bq);

// producer is fast
void *producer(void *arg)
{
    BoundedQueue *bq = arg;
    while (true) {
        int i = rand() % 26;

        char val = 'A' + i;
        char *item = malloc(sizeof(char));
        *item = val;
        boundedqueue_enqueue(bq, item);
        printf("sent %c\n", val);

        int timo = rand() % 3000 + 500;
        fiber_usleep(timo * 1000);
    }
    return NULL;
}

// consumer is slow
void *consumer(void *arg)
{
    BoundedQueue *bq = arg;
    int done = 0;
    while (!done) {
        char *item = boundedqueue_dequeue(bq);
        printf("received %c\n", *item);
        // if (*item == 'Z') {
        //  done = 1;
        // }
        free(item);

        int timo = rand() % 3000 + 500;
        fiber_usleep(timo * 1000);
    }
    return NULL;
}

bool initializeTasks(void * args)
{
    BoundedQueue *bq = (BoundedQueue *)args; // malloc(sizeof(BoundedQueue));
    boundedqueue_init(bq, 10);

    // fiber_t thread1, thread2;

    fiber_create(&producer, bq, NULL, 8192);
    fiber_create(&consumer, bq, NULL, 8192);

    // fiber_join(thread1, NULL);
    // fiber_join(thread2, NULL);
    // printf("done\n");

    return true;
}

int main(){
    FiberGlobalStartup();

    BoundedQueue bq;

    /* run another thread */
    fibthread_args_t args = {
      .init_func = initializeTasks,
      .args = (void *)(&bq),
    };
    pthread_scheduler(&args);

    return (0);
}


//////////////////////////////////////////////////////////////////////////
/* bounded queue implementation (using mutex & condition variables)     */
//////////////////////////////////////////////////////////////////////////          
void boundedqueue_init(BoundedQueue* bq, int max)
{
    queue_init(&bq->q);
    bq->max = max;
    bq->count = 0;
    fiber_mutex_init(&bq->lock);
    fiber_cond_init(&bq->cond);
}

void boundedqueue_enqueue(BoundedQueue* bq, void *item)
{
    fiber_mutex_lock(&bq->lock);

    while (bq->count >= bq->max) {
        fiber_cond_wait(&bq->cond, &bq->lock);
    }

    queue_enqueue(&bq->q, item);
    bq->count++;
    fiber_cond_broadcast(&bq->cond); // wake up consumer

    fiber_mutex_unlock(&bq->lock);
}

void* boundedqueue_dequeue(BoundedQueue* bq)
{
    fiber_mutex_lock(&bq->lock);

    while (bq->count == 0) {
        fiber_cond_wait(&bq->cond, &bq->lock);
    }

    void *result = queue_dequeue(&bq->q);
    bq->count--;
    fiber_cond_broadcast(&bq->cond); // wake up consumer

    fiber_mutex_unlock(&bq->lock);

    return result;
}

