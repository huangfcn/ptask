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
void   blockq_push(blockq_t * bq, void * object);
void * blockq_pop (blockq_t * bq);
void blockq_delete(blockq_t * bq);
//////////////////////////////////////////////////////////

#define NUM_PRODUCERS (16)
#define NUM_CONSUMERS (16)

typedef struct {
    blockq_t * bq;
    int index;
} context_t;

context_t ctxProducers[NUM_PRODUCERS], ctxConsumers[NUM_CONSUMERS];

static volatile uint64_t nPosted = 0ULL, nRecved = 0ULL; 
void * producer(void *arg)
{
    context_t * ctx = (context_t *)arg;
    blockq_t  * bq  = ctx->bq;
    
    int index     = ctx->index;
    while (true) {
        int    i      = index % 26; // rand() % 26;
        char   val    = 'A' + i;
        char * object = (void *)(int64_t)(val);
        blockq_push(bq, object);
        // if ((index & 8191) == 0)
        //     printf("producer %2d sent %c.\n", ctx->index, val);

        fiber_sched_yield();
        // int timo = rand() % 2000 + 450;
        // fiber_usleep(timo * 1000);
        FAA(&nPosted);
    }
    return NULL;
}

void *consumer(void *arg)
{
    context_t * ctx = (context_t *)arg;
    blockq_t  * bq  = ctx->bq;

    // int index = ctx->index;
    while (true) {
        char val = (int64_t)blockq_pop(bq);
        // if ((index & 8191) == 4095)
        //     printf("consumer %2d received %c.\n", ctx->index, val);

        fiber_sched_yield();
        // int timo = rand() % 2000 + 450;
        // fiber_usleep(timo * 1000);
        FAA(&nRecved);
    }
    return NULL;
}

/* create a fully loaded thread, it will push loading to other threads */
bool initializeTasks(void * args)
{
    blockq_t *bq = (blockq_t *)args;
    int n = (NUM_PRODUCERS < NUM_CONSUMERS) ? (NUM_PRODUCERS) : (NUM_CONSUMERS);

    for (int i = 0; i < n; ++i){
        ctxProducers[i].index = i;
        ctxProducers[i].bq    = bq;
        fiber_create(&producer, &ctxProducers[i], NULL, FIBER_STACKSIZE_MIN);
        
        ctxConsumers[i].index = i;
        ctxConsumers[i].bq    = bq;
        fiber_create(&consumer, &ctxConsumers[i], NULL, FIBER_STACKSIZE_MIN);
    }

    for (int i = n; i < NUM_PRODUCERS; ++i){
        ctxProducers[i].index = i;
        ctxProducers[i].bq    = bq;
        fiber_create(&producer, &ctxProducers[i], NULL, FIBER_STACKSIZE_MIN);
    }

    for (int i = n; i < NUM_CONSUMERS; ++i){
        ctxConsumers[i].index = i;
        ctxConsumers[i].bq    = bq;
        fiber_create(&consumer, &ctxConsumers[i], NULL, FIBER_STACKSIZE_MIN);
    }

    return true;
}

int main(){
    FiberGlobalStartup();

    blockq_t *bq = blockq_new(1024);

    pthread_t tid;
    /* create some service threads and wait it running */
    pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);
    // pthread_create(&tid, NULL, pthread_scheduler, NULL); sleep(1);

    fibthread_args_t args = {
      .threadStartup = initializeTasks,
      .threadCleanup = NULL,
      .args = (void *)(bq),
    };

    pthread_create(&tid, NULL, pthread_scheduler, &args); sleep(1);

    while (true){
        sleep(10);
        printf("message posted = %16lu, message received = %16lu\n", nPosted, nRecved);
    }
    return (0);
}


////////////////////////////////////////////////////////////////
/* blockq.c                                                   */
/* blocking queue using linked list & condition variables     */
////////////////////////////////////////////////////////////////
struct qnode;
typedef struct qnode qnode_t;

/*
 * Queue data structure.
 */
typedef struct queue {
    qnode_t *head, *tail;
} queue_t;

struct qnode {
    qnode_t * next;
    void    * object;
};

static inline void queue_init(queue_t *q)
{
    q->head = q->tail = NULL;
}

static inline int queue_is_empty(queue_t *q)
{
    return q->head == NULL;
}

static inline void queue_push(queue_t *q, qnode_t *node)
{
    if (q->tail != NULL) {
        q->tail->next = node;
        q->tail = node;
    } else {
        q->head = q->tail = node;
    }
}

static inline qnode_t * queue_pop(queue_t *q)
{
    qnode_t * oldhead;

    assert(!queue_is_empty(q));

    oldhead = q->head;
    q->head = q->head->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }

    return oldhead;
}

////////////////////////////////////////////////////////////
struct blockq_t {
    queue_t q;
    qnode_t * freelist;

    int32_t limit;
    int32_t count;

    fiber_mutex_t lock;
    fiber_cond_t  empt;
    fiber_cond_t  full;  
};

blockq_t * blockq_new(size_t limit)
{
    /* allocate all the memory we need */
    blockq_t * bq = (blockq_t *)malloc(
        sizeof(blockq_t) + sizeof(qnode_t) * (limit + 8)
        );
    if (bq == NULL){ return NULL; }
    memset(bq, 0, sizeof(blockq_t));

    /* setup qnodes list */
    qnode_t * blocks = (qnode_t *)(bq + 1);
    for (int i = 0; i < (limit + 8); ++i){
        blocks[i].next = bq->freelist;
        bq->freelist = (blocks + i);
    }

    queue_init(&bq->q);
    bq->limit = limit;
    bq->count = 0;

    fiber_mutex_init(&bq->lock);
    fiber_cond_init (&bq->empt);
    fiber_cond_init (&bq->full);

    return bq;
}

void blockq_push(blockq_t * bq, void *object)
{
    fiber_mutex_lock(&bq->lock);

    while (bq->count >= bq->limit) {
        fiber_cond_wait(&bq->full, &bq->lock);
    }

    /* allocate a qnode */
    qnode_t * node = bq->freelist;
    bq->freelist = node->next;

    assert(node != NULL);

    node->object = object;
    node->next   = NULL;

    queue_push(&bq->q, node);
    bq->count++;

    fiber_cond_signal(&bq->empt);
    fiber_mutex_unlock(&bq->lock);
}

void * blockq_pop(blockq_t * bq)
{
    void * object = NULL;
    fiber_mutex_lock(&bq->lock);

    while (bq->count == 0) {
        fiber_cond_wait(&bq->empt, &bq->lock);
    }

    qnode_t * node = queue_pop(&bq->q);
    object = node->object;
    
    node->next = bq->freelist;
    bq->freelist = node;

    bq->count--;
    
    fiber_cond_signal(&bq->full);
    fiber_mutex_unlock(&bq->lock);

    return object;
}

void blockq_delete(blockq_t * bq)
{
    fiber_mutex_destroy(&bq->lock);
    fiber_cond_destroy (&bq->empt);
    fiber_cond_destroy (&bq->full);

    free(bq);
}
////////////////////////////////////////////////////////////////