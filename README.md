# ptask
	ptask is a symetric stackful coroutine (task/fiber) library with pthread like API. 
	Although C++20 goes with stackless corouines, stackful coroutine with a much more 
	elegent way of yielding/resuming still has its ground. Since stackful 
	coroutine (task) is very close to thread, it will be convenient if the library 
	can provide coroutine aware synchronization methods. But most stackful coroutine 
	libraries in C/C++ are asymetric with very simple API (create/yield/resume) only, 
	so here comes ptask, a thread safe symetric coroutine library with pthread like 
	synchronization APIs.  

# Features

	1. Very compact, small code base (< 1500 lines)
	2. Support 1:N and M:N scheduling (thread safe)
	3. Stack caching & stack protection
	4. Support coroutine aware pthread style mutex/semaphore/condition synchronization
	5. Support bitmask event synchronization to natually integerate async events 
	6. Support coroutine ware timeout/sleep (usleep)
	7. Support task local variables
    8. Fully integeraged with epoll
	
# Pthread Style API
```c
    ///////////////////////////////////////////////////////////////////
    /* coroutine lib standard APIs:                                  */
    /* 1. libary initialization                                      */
    /* 2. create a task                                              */
    /* 3. yield                                                      */
    /* 4. resume                                                     */
    /* ------------------------------------------------------------- */
    /* 5. usleep (task level usleep, wont hangup the thread)         */
    /* 6. sched_yield                                                */
    ///////////////////////////////////////////////////////////////////
    /* called @ system startup */
    bool FiberGlobalStartup();

    /* create a task */
    fiber_t fiber_create(
        void *(*func)(void *), 
        void * args, 
        void * stackaddr, 
        uint32_t stacksize
    );

    /* yield will yield control to other task
    * current task will suspend until resume called on it
    */
    fiber_t fiber_yield(uint64_t code);
    uint64_t fiber_resume(fiber_t the_task);

    /* identify current running task */
    fiber_t fiber_ident();

    /* task usleep (accurate at ms level)*/
    void fiber_usleep(int usec);

    /* same functionality of sched_yield, yield the processor
    * sched_yield causes the calling task to relinquish the CPU.
    * The task is moved to the end of the ready queue and 
    * a new task gets to run.
    */
    fiber_t fiber_sched_yield();

    /* mutex */
    int  fiber_mutex_init(fiber_mutex_t * the_mutex);
    bool fiber_mutex_lock(fiber_mutex_t * the_mutex);
    bool fiber_mutex_unlock(fiber_mutex_t * the_mutex);
    void fiber_mutex_destroy(fiber_mutex_t * the_mutex);

    /* sempahore */
    int  fiber_sem_init(fiber_sem_t * psem, int initval);
    bool fiber_sem_wait(fiber_sem_t * psem);
    bool fiber_sem_timedwait(fiber_sem_t * psem, int timeout);
    bool fiber_sem_post(fiber_sem_t * psem);
    void fiber_sem_destroy(fiber_sem_t * psem);

    /* Conditional Variables */
    int  fiber_cond_init(fiber_cond_t * pcond);
    bool fiber_cond_wait(fiber_cond_t * pcond, fiber_mutex_t * pmutex);
    bool fiber_cond_timedwait(fiber_cond_t * pcond, fiber_mutex_t * pmutex, int timeout);
    bool fiber_cond_signal(fiber_cond_t * pcond);
    bool fiber_cond_broadcast(fiber_cond_t * pcond);
    void fiber_cond_destroy(fiber_cond_t * pcond);

    /* Extremely efficient bitmask based Events (ptask specific) 
    *  A task can wait for up to 64 events by specifying waiting any event or all events
    */
    uint64_t fiber_event_wait(uint64_t events_bitmask_waitingfor, int options, int timeout);
    int fiber_event_post(fiber_t the_task, uint64_t events_bitmask_in);
    ///////////////////////////////////////////////////////////////////
```
# epoll Integeration
```c
    ///////////////////////////////////////////////////////////////////
    /* epoll integeration                                            */
    ///////////////////////////////////////////////////////////////////
    int fiber_epoll_register_events(int fd, int events);
    int fiber_epoll_unregister_event(fiber_t the_tcb, int index);
    int fiber_epoll_wait(
        struct epoll_event * events, 
        int maxEvents, 
        int timeout_in_ms
        );
    int fiber_epoll_post(
        int nEvents,
        struct epoll_event * events
        );
    ///////////////////////////////////////////////////////////////////
```

# Example 1: Generator
```c
void * generator(void * args){
    for (int i = 0; i < 1000; ++i){
        fiber_yield(i);
    }
    return (void *)(0);
}

void * generator_maintask(void * args){
    FibTCB * the_gen = fiber_create(generator, args, NULL, 8192 * 2);

    for (int i = 0; i < 1000; ++i){
        fiber_sched_yield();
        int64_t code = fiber_resume(the_gen);
        printf("code = %ld\n", code);
    }

    return (void *)(0);
}
```

# Example 2: MPMC Blocking Queue (mutex + cv)
```c
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
```

# Example 3: Read-Write Lock

```c

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
```

# Example 4: port pipe (https://github.com/cgaebel/pipe)


```c
// a mapping of pthread to ptask synchronization API
#elif defined(__PTHREAD__) /* pthread */

#include <pthread.h>

#define mutex_t pthread_mutex_t
#define cond_t  pthread_cond_t

#define mutex_init(m)  pthread_mutex_init((m), NULL)

#define mutex_lock     pthread_mutex_lock
#define mutex_unlock   pthread_mutex_unlock
#define mutex_destroy  pthread_mutex_destroy

#define cond_init(c)   pthread_cond_init((c), NULL)
#define cond_signal    pthread_cond_signal
#define cond_broadcast pthread_cond_broadcast
#define cond_wait      pthread_cond_wait
#define cond_destroy   pthread_cond_destroy

#else  /* fiber */

#include "task.h"

#define mutex_t fiber_mutex_t
#define cond_t  fiber_cond_t

#define mutex_init(m)  fiber_mutex_init((m))

#define mutex_lock     fiber_mutex_lock
#define mutex_unlock   fiber_mutex_unlock
#define mutex_destroy  fiber_mutex_destroy

#define cond_init(c)   fiber_cond_init((c))
#define cond_signal    fiber_cond_signal
#define cond_broadcast fiber_cond_broadcast
#define cond_wait      fiber_cond_wait
#define cond_destroy   fiber_cond_destroy

#endif /* windows */
```

# Example 5: simple http webserver using epoll

    src/epoll.c and simplehttp.c 