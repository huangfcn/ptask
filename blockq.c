#include <unistd.h>
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>

#include "libfiber.h"

//////////////////////////////////////////////////////////
/* blockq.h                                             */
//////////////////////////////////////////////////////////
struct blockq_t;
typedef struct blockq_t blockq_t;

blockq_t *blockq_new(size_t limit);
void   blockq_push(blockq_t * bq, void * pitem);
void * blockq_pop (blockq_t * bq);
void blockq_delete(blockq_t * bq);
//////////////////////////////////////////////////////////

#define NUM_PRODUCERS (6)
#define NUM_CONSUMERS (8)

typedef struct {
    blockq_t * bq;
    int index;
} context_t;

context_t ctxProducers[NUM_PRODUCERS], ctxConsumers[NUM_CONSUMERS];

// producer is fast
void * producer(void *arg)
{
    context_t * ctx = (context_t *)arg;
    blockq_t  * bq  = ctx->bq;
    while (true) {
        int i = rand() % 26;

        char   val  = 'A' + i;
        char * item = (void *)(int64_t)(val);
        blockq_push(bq, item);
        printf("producer %2d sent %c.\n", ctx->index, val);

        int timo = rand() % 2000 + 400;
        fiber_usleep(timo * 1000);
    }
    return NULL;
}

// consumer is slow
void *consumer(void *arg)
{
    context_t * ctx = (context_t *)arg;
    blockq_t  * bq  = ctx->bq;
    while (true) {
        char val = (int64_t)blockq_pop(bq);
        printf("consumer %2d received %c.\n", ctx->index, val);
        int timo = rand() % 3000 + 500;
        fiber_usleep(timo * 1000);
    }
    return NULL;
}

bool initializeTasks(void * args)
{
    blockq_t *bq = blockq_new(3);

    for (int i = 0; i < NUM_PRODUCERS; ++i){
        ctxProducers[i].index = i;
        ctxProducers[i].bq    = bq;
        fiber_create(&producer, &ctxProducers[i], NULL, FIBER_STACKSIZE_MIN);
    }

    for (int i = 0; i < NUM_CONSUMERS; ++i){
        ctxConsumers[i].index = i;
        ctxConsumers[i].bq    = bq;
        fiber_create(&consumer, &ctxConsumers[i], NULL, FIBER_STACKSIZE_MIN);
    }

    return true;
}

int main(){
    FiberGlobalStartup();

    /* run another thread */
    fibthread_args_t args = {
      .init_func = initializeTasks,
      .args = (void *)(0),
    };
    pthread_scheduler(&args);

    return (0);
}


////////////////////////////////////////////////////////////////
/* blockq.c                                                   */
/* blocking queue using linked list & condition variables     */
////////////////////////////////////////////////////////////////
struct queue_node;
typedef struct queue_node queue_node_t;

/*
 * Queue data structure.
 */
typedef struct queue {
    queue_node_t *head, *tail;
} queue_t;

struct queue_node {
    struct queue_node *next;
    void * item;
};

static inline void queue_init(queue_t *q)
{
    q->head = q->tail = NULL;
}

static inline int queue_is_empty(queue_t *q)
{
    return q->head == NULL;
}

static inline void queue_push(queue_t *q, void * item)
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

static inline void * queue_pop(queue_t *q)
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
struct blockq_t {
    queue_t q;
    int32_t limit;  // max number of the queue
    int32_t count;  // current number of items

    fiber_mutex_t lock;
    fiber_cond_t  empt;
    fiber_cond_t  full;    
};

blockq_t * blockq_new(size_t limit)
{
    blockq_t * bq = (blockq_t *)malloc(sizeof(blockq_t));
    if (bq == NULL){ return NULL; }

    memset(bq, 0, sizeof(blockq_t));

    queue_init(&bq->q);
    bq->limit = limit;
    bq->count = 0;

    fiber_mutex_init(&bq->lock);
    fiber_cond_init (&bq->empt);
    fiber_cond_init (&bq->full);

    return bq;
}

void blockq_push(blockq_t * bq, void *item)
{
    fiber_mutex_lock(&bq->lock);

    while (bq->count >= bq->limit) {
        fiber_cond_wait(&bq->full, &bq->lock);
    }

    queue_push(&bq->q, item);
    bq->count++;

    fiber_cond_signal(&bq->empt);
    fiber_mutex_unlock(&bq->lock);
}

void* blockq_pop(blockq_t * bq)
{
    void * res = NULL;
    fiber_mutex_lock(&bq->lock);

    while (bq->count == 0) {
        fiber_cond_wait(&bq->empt, &bq->lock);
    }

    res = queue_pop(&bq->q);
    bq->count--;
    
    fiber_cond_signal(&bq->full);
    fiber_mutex_unlock(&bq->lock);

    return res;
}

void blockq_delete(blockq_t * bq)
{
    fiber_mutex_destroy(&bq->lock);
    fiber_cond_destroy (&bq->empt);
    fiber_cond_destroy (&bq->full);

    free(bq);
}
////////////////////////////////////////////////////////////////