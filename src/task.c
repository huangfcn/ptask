#include <sys/mman.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <semaphore.h>
#include <errno.h>

#include "sysdef.h"
#include "spinlock.h"

#include "chain.h"
#include "task.h"

#include "timestamp.h"

#define ACTIVATE_TASKS_LOCALE_READYLIST (0)
#define ACTIVATE_TASKS_GLOBAL_READYLIST (1)
#define ACTIVATE_TASKS_TARGET_READYLIST (0)

static inline int fiber_watchdog_insert(FibTCB * the_task);
static inline int fiber_watchdog_remove(FibTCB * the_task);
static inline int fiber_watchdog_tickle(FibTCB * the_schd, int gap);

typedef struct freelist_t { struct freelist_t * next; } freelist_t;

/////////////////////////////////////////////////////////////////////////
/* local thread specific data                                          */
/////////////////////////////////////////////////////////////////////////
__thread_local FibTCB * current_task = NULL;
__thread_local FibTCB * the_maintask = NULL;

#define getLocalFibTasksPtr(the_task) ( (volatile int32_t *)(&((the_task)->scheddata->nLocalFibTasks)))
#define getLocalFibTasksVal(the_task) (*(volatile int32_t *)(&((the_task)->scheddata->nLocalFibTasks)))

#define getLocalFreedSize(the_task) ((the_task)->scheddata->freedsize)
#define getLocalFreedList(the_task) ((the_task)->scheddata->freedlist)

#define getLocalWadogList(the_task) (&((the_task)->scheddata->wadoglist))
#define getLocalWadogLock(the_task) (&((the_task)->scheddata->wadoglock))

#define getLocalStackMask(the_task) ((the_task)->scheddata->cached_stack_mask)
#define getLocalStackVect(the_task) ((the_task)->scheddata->cached_stack     )

#define getLocalReadyList(the_task) (&((the_task)->scheddata->readylist))
#define getLocalReadyLock(the_task) (&((the_task)->scheddata->readylock))

#if (ACTIVATE_TASKS_TARGET_READYLIST)
#define localReadyListAcquire(the_task) do {spin_lock  (getLocalReadyLock(the_task));} while (0)
#define localReadyListRelease(the_task) do {spin_unlock(getLocalReadyLock(the_task));} while (0)
#else
#define localReadyListAcquire(the_task)
#define localReadyListRelease(the_task)
#endif

#define localReadyQProtectedExtract(_the_task) do {                     \
    localReadyListAcquire(_the_task);                                   \
    _CHAIN_REMOVE(_the_task);                                           \
    localReadyListRelease(_the_task);                                   \
} while (0)

#define localReadyQProtectedInsertBefore(_the_sched, _the_task) do {    \
    localReadyListAcquire(_the_task);                                   \
    _CHAIN_INSERT_BEFORE((_the_sched), (_the_task));                    \
    localReadyListRelease(_the_task);                                   \
} while (0)
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* global variables                                                    */
/////////////////////////////////////////////////////////////////////////
static volatile freelist_t * global_freedlist = NULL;
static fibchain_t global_readylist[MAX_GLOBAL_GROUPS] = {0};

static spinlock_t spinlock_freedlist = {0};
static spinlock_t spinlock_readylist[MAX_GLOBAL_GROUPS] = {0};

static volatile struct {
    void *   stackbase;
    uint32_t stacksize;
} global_cached_stack[64] = {0};
static volatile uint64_t global_cached_stack_mask = 0ULL;
static spinlock_t spinlock_cached_stack = {0};

static volatile int64_t mServiceThreads = 0, nGlobalFibTasks = 0;

static sem_t __sem_null;
/////////////////////////////////////////////////////////////////////////

static __forceinline void pushIntoGlobalReadyList(FibTCB * the_task){
    int group = the_task->group;
    fibchain_t * the_chain = &global_readylist[group];
    spinlock_t * the_lock  = &spinlock_readylist[group];

    spin_lock(the_lock);
    the_task->scheduler = NULL;
    the_task->scheddata = NULL;
    _CHAIN_INSERT_TAIL(the_chain, the_task);
    spin_unlock(the_lock);
};

static __forceinline FibTCB * popFromGlobalReadyList(FibTCB * the_scheduler, int group){
    fibchain_t * the_chain = &global_readylist[group];
    spinlock_t * the_lock  = &spinlock_readylist[group];
    FibTCB * the_task = NULL;
    spin_lock(the_lock);
    the_task = _CHAIN_EXTRACT_FIRST(the_chain);
    if (the_task){
        the_task->scheduler = the_scheduler;
        the_task->scheddata = the_scheduler->scheddata;

        /* make it ready */
        the_task->state &= (~STATES_BLOCKED);
    }
    spin_unlock(the_lock);  
    return the_task;
};

static inline int __usleep__(int64_t us)    
{                                                  
    struct timespec ts; int s;                     
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        return false;                              
    }                                              
                                                                     
    /* add 10ms */                                               
    ts.tv_nsec += us * 1000ULL;                                  
    ts.tv_sec += ts.tv_nsec / 1000000000ULL;                     
    ts.tv_nsec %= 1000000000ULL;                                 
    while ((s = sem_timedwait(&__sem_null, &ts)) == -1 && errno == EINTR){
        continue;       /* Restart if interrupted by handler */  
    }

    return (s == 0) ? (0) : ((errno == ETIMEDOUT) ? (+1) : (-1));
};
/////////////////////////////////////////////////////////////////////////
/* Macro Loop on Set Bit                                               */
/////////////////////////////////////////////////////////////////////////
#define callback_on_setbit(bitset, callback)  do {                      \
    size_t p = 0;                                                       \
    while (bitset != 0) {                                               \
        switch (bitset & 0xf) {                                         \
            case 0:                                    break;           \
            case 1:  callback(p);                      break;           \
            case 2:  callback(p + 1);                  break;           \
            case 3:  callback(p);     callback(p + 1); break;           \
            case 4:  callback(p + 2);                  break;           \
            case 5:  callback(p);     callback(p + 2); break;           \
            case 6:  callback(p + 1); callback(p + 2); break;           \
            case 7:  callback(p);     callback(p + 1);                  \
                     callback(p + 2);                  break;           \
            case 8:  callback(p + 3);                  break;           \
            case 9:  callback(p);     callback(p + 3); break;           \
            case 10: callback(p + 1); callback(p + 3); break;           \
            case 11: callback(p);     callback(p + 1);                  \
                     callback(p + 3);                  break;           \
            case 12: callback(p + 2); callback(p + 3); break;           \
            case 13: callback(p);     callback(p + 2);                  \
                     callback(p + 3);                  break;           \
            case 14: callback(p + 1); callback(p + 2);                  \
                     callback(p + 3);                  break;           \
            case 15: callback(p);     callback(p + 1);                  \
                     callback(p + 2); callback(p + 3); break;           \
        }                                                               \
        bitset >>= 4;                                                   \
        p += 4;                                                         \
    }                                                                   \
} while(0)
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* SYSTEM/THREAD level data initialization                             */
/////////////////////////////////////////////////////////////////////////
bool FiberGlobalStartup(){
    /* initialize global ready list */
    for (int i = 0; i < MAX_GLOBAL_GROUPS; ++i){ 
        _CHAIN_INIT_EMPTY(&global_readylist[i]);
    }

    /* create a null semaphore for timeout */
    sem_init(&__sem_null, 0, 0);

    /* all other global variables statically initialized */
    return true;
};

static __forceinline FibSCP * fibscp_alloc()
{
    FibSCP * the_scp= ((FibSCP *)malloc(sizeof(FibSCP)));
    if (the_scp == NULL){return NULL;};

    /* local freedlist & readylist */
    memset(the_scp, 0, sizeof(FibSCP));

    _CHAIN_INIT_EMPTY(&(the_scp->readylist));
     CHAIN_INIT_EMPTY(&(the_scp->wadoglist), FibTCB, link);

    the_scp->cached_stack_mask = 0xFF;
    return the_scp;
};
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* TCB alloc/free                                                      */
/////////////////////////////////////////////////////////////////////////
static inline void tcb_free(FibTCB * tcb){
    FibTCB * the_scheduler = the_maintask;
    freelist_t * node = (freelist_t *)tcb;
    if (likely(getLocalFreedSize(the_scheduler) < MAX_LOCAL_FREED_TASKS)){
        node->next = (freelist_t *)getLocalFreedList(the_scheduler);
        getLocalFreedList(the_scheduler) = (void *)node;
        getLocalFreedSize(the_scheduler) += 1;
    }
    else{
        __spin_lock(&spinlock_freedlist);
        node->next = (freelist_t *)global_freedlist;
        global_freedlist = node;
        __spin_unlock(&spinlock_freedlist);
    }
};

static inline FibTCB * tcb_alloc(){
    FibTCB * the_scheduler = the_maintask;
    if (the_scheduler == NULL){
        return ((FibTCB *)malloc(sizeof(FibTCB)));
    }

    if (likely(getLocalFreedList(the_scheduler) != NULL)){
        FibTCB * tcb = (FibTCB *)getLocalFreedList(the_scheduler);
        getLocalFreedList(the_scheduler) = ((freelist_t *)getLocalFreedList(the_scheduler))->next;
        getLocalFreedSize(the_scheduler) -= 1;
        return tcb;
    }
    else{
        FibTCB * tcb = NULL;
        __spin_lock(&spinlock_freedlist);
        tcb = (FibTCB *)global_freedlist;
        if (likely(tcb)){global_freedlist = global_freedlist->next;}
        __spin_unlock(&spinlock_freedlist);
        if (likely(tcb)){return tcb;}
    }

    /* malloc/mmap */
    {
        FibTCB * tcbs = (FibTCB *)malloc(sizeof(FibTCB) * TCB_INCREASE_SIZE_AT_EMPTY);
        if (tcbs == NULL){
            /* logging, memory shortage */
            return NULL;
        }
        for (int i = 1; i < TCB_INCREASE_SIZE_AT_EMPTY; ++i){
            tcb_free(tcbs + i);
        }
        return (tcbs);
    }
};
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* STACK CACHE                                                         */
/////////////////////////////////////////////////////////////////////////
static inline bool fiber_stackcache_put(
    uint8_t * stackbase, 
    uint32_t  stacksize
){
    uint8_t * oldstack = NULL;
    uint32_t  oldsize  = 0;

    FibTCB * the_scheduler = the_maintask;
    /* put into local cache first */
    int index = __ffs32(getLocalStackMask(the_scheduler));
    if (unlikely(index == 0)){
        __spin_lock(&spinlock_cached_stack);
        index = __ffs64(global_cached_stack_mask);
        if (likely(index != 0)){
            --index;
            global_cached_stack_mask &= (~(1ULL << index));
            global_cached_stack[index].stackbase = stackbase;
            global_cached_stack[index].stacksize = stacksize;
        }
        __spin_unlock(&spinlock_cached_stack);

        if (unlikely(index == 0)){
            /* generate a random number */
            uint64_t u64 = ((uint64_t)(stackbase));
            u64 = (u64 >> 8) ^ (u64 >> 16) ^ (u64 >> 24) ^ (u64 >> 32);
            u64 = (u64 >> 4) + (u64 >> 0);
            index =  u64 & 7;

            oldstack = getLocalStackVect(the_scheduler)[index].stackbase;
            oldsize  = getLocalStackVect(the_scheduler)[index].stacksize;

            getLocalStackVect(the_scheduler)[index].stackbase = stackbase;
            getLocalStackVect(the_scheduler)[index].stacksize = stacksize;
        }
    }
    else{
        --index;
        getLocalStackMask(the_scheduler) &= (~(1U << index));
        getLocalStackVect(the_scheduler)[index].stackbase = stackbase;
        getLocalStackVect(the_scheduler)[index].stacksize = stacksize;
    }

    /* free old stack if needed */
    if (oldstack){
        munmap(oldstack, oldsize);
    }

    return true;
}

static inline uint8_t * fiber_stackcache_get(uint32_t * stacksize){
    FibTCB * the_scheduler = the_maintask;

    /* find in local */
    if (the_scheduler) {
        uint32_t mask = (~getLocalStackMask(the_scheduler)) & 0xFF;
        int      index = -1;
        #define callback_setlocalstack(p) do {                      \
            if (getLocalStackVect(the_scheduler)[p].stacksize >= (*stacksize)){   \
                mask  = 0U;                                         \
                index = p;                                          \
            }                                                       \
        } while (0)

        callback_on_setbit(mask, callback_setlocalstack);
        if (index >= 0){
            getLocalStackMask(the_scheduler) |= (1U << index);
            *stacksize = getLocalStackVect(the_scheduler)[index].stacksize;
            return getLocalStackVect(the_scheduler)[index].stackbase;
        }
    }

    /* find in global */
    {
        int       index     = -1;
        uint8_t * stackaddr = NULL;     

        __spin_lock(&spinlock_cached_stack);
        uint64_t  mask  = ~global_cached_stack_mask;
        #define callback_setglobalstack(p) do {                      \
            if (global_cached_stack[p].stacksize >= (*stacksize)){   \
                mask  = 0ULL;                                        \
                index = p;                                           \
            }                                                        \
        } while (0)

        callback_on_setbit(mask, callback_setglobalstack);

        if (index >= 0){
            global_cached_stack_mask |= (1ULL << index);

            *stacksize = global_cached_stack[index].stacksize;
            stackaddr = global_cached_stack[index].stackbase;
        }
        __spin_unlock(&spinlock_cached_stack);
        if (index >= 0) {return stackaddr;}
    }

    /* mmap from system */
    {
        // (*stacksize) = (((*stacksize) + 4095) / 4096) * 4096;
        uint8_t * stackbase = (uint8_t *)mmap(
            NULL, (*stacksize),
            0,
            MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK,
            -1, 0
            );
        if (stackbase){
            int rc = mprotect (
                stackbase + 4096, 
                4096 * ((*stacksize) / 4096 - 1), 
                PROT_READ | PROT_WRITE
                );
            if (rc < 0){
                // perror("mprotect: ");
                munmap(stackbase, (*stacksize));
                return NULL;
            }
        }
        return stackbase;
    }
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* fibtask/thread entry point                                          */
/* for fibtask:                                                        */
/*     asm_taskmain --> cpp_taskmain --> user entry                    */
/*                                                                     */
/* for fibthread:                                                      */
/*     thread_maintask --> asm_taskmain --> maintask -->               */
/*     user init_func  --> scheduler                                   */
/////////////////////////////////////////////////////////////////////////
static __forceinline uint32_t hash6432shift(uint64_t key)
{
  key = (~key) + (key << 18); // key = (key << 18) - key - 1;
  key = key ^ (key >> 31);
  key = key * 21; // key = (key + (key << 2)) + (key << 4);
  key = key ^ (key >> 11);
  key = key + (key << 6);
  key = key ^ (key >> 22);
  return (uint32_t) key;
}

static void * cpp_taskmain(FibTCB * the_task){
    /* callback taskStartup */
    if (the_task->onTaskStartup){
        the_task->onTaskStartup(the_task);
    }

    /* call user entry */
    the_task->entry(the_task->args);

    /* decrease number of fibtasks in system */
    FAS(&nGlobalFibTasks);
    FAS(getLocalFibTasksPtr(the_task));

    FibTCB * the_scheduler = the_task->scheduler;
    bool is_maintask = (the_task == the_scheduler);

    /* callback on taskCleanup */
    if (the_task->onTaskCleanup){
        the_task->onTaskCleanup(the_task);
    }

    /* remove current task from ready list*/
    localReadyQProtectedExtract(the_task);

    /* free stack */
    if (the_task->stacksize & MASK_SYSTEM_STACK){
        /* cannot free at here, put into cache for next thread */
        fiber_stackcache_put(
            the_task->stackaddr,
            the_task->stacksize & (~15UL)
            );
    }

    /* free the_task */
    tcb_free(the_task);

    if (is_maintask){ return (void *)(0); }

    /* switch to thread maintask */
    current_task = the_scheduler;
    goto_context(&(the_scheduler->regs));

    /* never reach here */
    return ((void *)(0));
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* create a fibtask & put onto ready list                              */
/////////////////////////////////////////////////////////////////////////
FibTCB * fiber_create(
    void *(*func)(void *), 
    void * args, 
    void * stackaddr, 
    uint32_t stacksize
){
    FibTCB * the_task = tcb_alloc();
    if (the_task == NULL){
        return NULL;
    }

    /* initialize to (0) */
    the_task->state = STATES_READY;

    uint8_t * stackbase = (uint8_t *)stackaddr;
    if (stackbase == NULL){
        stacksize = ((stacksize + 4095) / 4096) * 4096;
        stackbase = fiber_stackcache_get(&stacksize);
        if (stackbase == NULL){
            tcb_free(the_task);
            return NULL;
        }

        /* system stack */
        stacksize |= MASK_SYSTEM_STACK;
    }
    the_task->stackaddr = (void *)stackbase;
    the_task->stacksize = stacksize;

    the_task->entry = func;
    the_task->args  = args;

    /* initialize task's group */
    the_task->group = hash6432shift( ((uint64_t)(the_task)) ^ ((uint64_t)(stackbase)) ) & (MAX_GLOBAL_GROUPS - 1);

    /* initialize event */
    spin_init(&(the_task->eventlock));

    /* callbacks */
    the_task->onTaskStartup = NULL;
    the_task->onTaskCleanup = NULL;

    /* r12 (tcb), r15 (cpp_taskmain), rip (asm_taskmain), rsp */
    the_task->regs.reg_r12 = (uint64_t)(the_task);
    the_task->regs.reg_r15 = (uint64_t)(cpp_taskmain);
    the_task->regs.reg_rip = (uint64_t)(asm_taskmain);
    the_task->regs.reg_rsp = (uint64_t)(stackbase + (stacksize & (~15UL)));

    /* usually the first task created of thread is the maintask */
    if (unlikely(the_maintask == NULL)){
        the_maintask = the_task;
        current_task = the_task;
        the_task->scheddata = fibscp_alloc();
    }

    /* load maintask to scheduler */
    FibTCB * the_scheduler = the_maintask;
    fibchain_t * the_chain = getLocalReadyList(the_scheduler);

    /* increase number of fibtasks in system */
    FAA(&nGlobalFibTasks);

    #ifndef __1_N_MODEL__
    /* load balance when task first created */
    if (unlikely((the_task != the_scheduler) && (getLocalFibTasksVal(the_scheduler) > ((nGlobalFibTasks * 7 / 6 + mServiceThreads - 1) / mServiceThreads)))){
        /* set scheduler & scheddata to NULL */
        pushIntoGlobalReadyList(the_task);
    }
    else
    #endif
    {
        /* set scheduler & scheddata */
        the_task->scheduler = (the_scheduler);
        the_task->scheddata = (the_scheduler->scheddata);

        /* increase local tasks */
        FAA(getLocalFibTasksPtr(the_task));

        /* put next_task onto end of ready list */
        localReadyListAcquire(the_task);
        if (likely(the_task != the_scheduler)){
            _CHAIN_INSERT_BEFORE(the_scheduler, the_task);
        }
        else {
            _CHAIN_INSERT_TAIL(the_chain, the_task);
        }
        localReadyListRelease(the_task);
    }
    return the_task;
}
/////////////////////////////////////////////////////////////////////////

FibTCB * fiber_ident(){
    return current_task;
}

/////////////////////////////////////////////////////////////////////////
/* schedule to next task                                               */
/////////////////////////////////////////////////////////////////////////
static inline void fiber_sched(){
    /* find next ready task */
    FibTCB * the_scheduler = the_maintask, * the_task = current_task;
    fibchain_t * the_chain = getLocalReadyList(the_scheduler);

    localReadyListAcquire(the_scheduler);
    FibTCB * the_next = _CHAIN_FIRST(the_chain);
    if (unlikely(the_next == the_task)){
        /* cannot find a candidate, 
         * if and only if called from maintask and only maintask running,
         */
        localReadyListRelease(the_scheduler);
        return;
    }

    /* extract the task and set it as current task */
    _CHAIN_REMOVE(the_next);
    current_task = the_next;

    /* put the_next onto end of ready list */
    if (likely(the_next != the_scheduler)){
        _CHAIN_INSERT_BEFORE(the_scheduler, the_next);
    }
    else{
        _CHAIN_INSERT_TAIL(the_chain, the_next);        
    }
    localReadyListRelease(the_scheduler);
    
    /* swap context and jump to next task */
    swap_context(&(the_task->regs), &(the_next->regs));
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* set / clear blocking states                                         */
/////////////////////////////////////////////////////////////////////////
static inline int fiber_setstate(FibTCB * the_task, uint64_t states){
    /* set state */
    the_task->state |= states;

    /* extract from ready list */
    localReadyQProtectedExtract(the_task);

    /* fiber_sched to next task */
    fiber_sched();

    return (0);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* yield/resume                                                        */
/////////////////////////////////////////////////////////////////////////
FibTCB * fiber_yield(uint64_t code){
    FibTCB * the_task = current_task;
    the_task->yieldCode = code;
    fiber_setstate(the_task, STATES_SUSPENDED);
    return (the_task);
}

uint64_t fiber_resume(FibTCB * the_task){
    uint64_t yieldCode = the_task->yieldCode;
    the_task->state &= (~STATES_SUSPENDED);
    localReadyQProtectedInsertBefore(the_task->scheduler, the_task);
    return (yieldCode);
}

__force_noinline FibTCB * fiber_sched_yield(){
    FibTCB * the_scheduler = the_maintask, * the_task = current_task;
    fibchain_t * the_chain = getLocalReadyList(the_scheduler);

    localReadyListAcquire(the_scheduler);
    /* move to end of ready list */
    _CHAIN_REMOVE(the_task);

    if (unlikely(the_task == the_scheduler)){
        _CHAIN_INSERT_TAIL(the_chain, the_task);
    }
    else{
        _CHAIN_INSERT_BEFORE(the_scheduler, the_task);
    }
    localReadyListRelease(the_scheduler);

    /* fiber_sched to next task */
    fiber_sched();

    return (the_task);
}

void fiber_usleep(int usec){
    FibTCB * the_task = current_task;

    the_task->delta_interval = usec;
    fiber_watchdog_insert(the_task);
    fiber_setstate(the_task, STATES_IN_USLEEP);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* watchdog or timeout support                                         */
/////////////////////////////////////////////////////////////////////////
static inline int fiber_watchdog_insert(FibTCB * the_task){
    spinlock_t * the_lock  = &(the_task->scheddata->wadoglock);
    fibchain_t * the_chain = &(the_task->scheddata->wadoglist);
    int delta_interval = the_task->delta_interval;
    
    spin_lock(the_lock);
    if (the_task->state & STATES_WAIT_TIMEOUTB){
        spin_unlock(the_lock);
        return (1);
    }

    FibTCB * after = CHAIN_FIRST(the_chain);
    for (;;after = CHAIN_NEXT(after, link)){

        if (delta_interval == 0 || !CHAIN_NEXT(after, link)){ break; }

        if (delta_interval < after->delta_interval) {
            after->delta_interval -= delta_interval;
            break;
        }

        delta_interval -= after->delta_interval;
    }
    the_task->delta_interval = delta_interval;
    CHAIN_INSERT_BEFORE(after, the_task, FibTCB, link);

    /* set timeout flag of the task */
    the_task->state |= STATES_WAIT_TIMEOUTB;
    spin_unlock(the_lock);

    return (0);
}

static inline int fiber_watchdog_remove(FibTCB * the_task){
    spinlock_t * the_lock = &(the_task->scheddata->wadoglock);
    
    spin_lock(the_lock);
    if (the_task->state & STATES_WAIT_TIMEOUTB){
        FibTCB * nxt_tcb = CHAIN_NEXT(the_task, link);
        if (CHAIN_NEXT(nxt_tcb, link)){
            nxt_tcb->delta_interval += the_task->delta_interval;
        }
        CHAIN_REMOVE(the_task, FibTCB, link);

        /* clear timeout flag of the task */
        the_task->state &= (~STATES_WAIT_TIMEOUTB);
    }
    spin_unlock(the_lock);

    return (0);
}

/* this functions ias called from thread maintask (scheduling task) */
static inline int fiber_watchdog_tickle(FibTCB * the_scheduler, int gap){
    FibTCB * the_task, * the_nxt;

    spinlock_t * the_lock  = getLocalWadogLock(the_scheduler);
    fibchain_t * the_chain = getLocalWadogList(the_scheduler);

    spin_lock(the_lock);
    CHAIN_FOREACH_SAFE(the_task, the_chain, link, the_nxt){
        if (the_task->delta_interval <= gap){
            /* remove from the chain */
            CHAIN_REMOVE(the_task, FibTCB, link);

            /* clear timeout flag of the task */
            the_task->state &= (~STATES_WAIT_TIMEOUTB);
                    
            if (the_task->state & (STATES_WAITFOR_SEMPH | STATES_WAITFOR_CONDV)){
                /* semaphore & condition variable only operate when seized the control */
                FibSemaphore * psem = (FibSemaphore *)(the_task->waitingObject);
                spin_lock(&(psem->qlock));
                if (the_task->state & (STATES_WAITFOR_SEMPH | STATES_WAITFOR_CONDV)){
                    /* clear WAITING STATES */
                    the_task->state &= (~(STATES_WAITFOR_SEMPH | STATES_WAITFOR_CONDV));

                    /* extract from semaphore's waiting q */
                    _CHAIN_REMOVE(the_task);

                    /* always call from scheduler's thread */
                    localReadyQProtectedInsertBefore(the_task->scheduler, the_task);
                }
                spin_unlock(&(psem->qlock));
            }
            else if (the_task->state & STATES_WAITFOR_EVENT){
                spin_lock(&(the_task->eventlock));
                if (the_task->state & STATES_WAITFOR_EVENT){
                    /* clear WAITING STATES */
                    the_task->state &= (~STATES_WAITFOR_EVENT);

                    /* always call from scheduler's thread */
                    localReadyQProtectedInsertBefore(the_task->scheduler, the_task);
                }
                spin_unlock(&(the_task->eventlock));
            }
            else if (the_task->state & STATES_IN_USLEEP){
                /* clear WAITING STATES */
                the_task->state &= (~STATES_IN_USLEEP);

                /* always call from scheduler's thread */
                localReadyQProtectedInsertBefore(the_task->scheduler, the_task);
            }

            /* what happened (?) */
        }
        else{
            the_task->delta_interval -= gap;
            break;
        }
    }
    spin_unlock(the_lock);

    return (0);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* wait/post events (only used internally)                             */
/////////////////////////////////////////////////////////////////////////
uint64_t fiber_event_wait(uint64_t events_in, int options, int timeout){
    FibTCB * the_task = current_task;
    
    spin_lock(&(the_task->eventlock));
    /* check the pending events */
    uint64_t seized_events = the_task->pendingEvents & events_in;
    if (seized_events && ((seized_events == events_in) || (options & TASK_EVENT_WAIT_ANY))){
        the_task->pendingEvents &= (~seized_events);
        spin_unlock(&(the_task->eventlock));

        return seized_events;
    }

    /* no wait, test only (?) */
    if (unlikely(timeout == 0)){
        spin_unlock(&(the_task->eventlock));

        return 0ULL;
    }

    the_task->seizedEvents   = 0ULL;
    the_task->waitingEvents  = events_in;
    the_task->waitingOptions = options;

    /* have to wait */
    the_task->state |= (((timeout > 0) ? STATES_TRANSIENT : 0) | STATES_WAITFOR_EVENT);

    /* extract from ready list */
    localReadyQProtectedExtract(the_task);

    spin_unlock(&(the_task->eventlock));

    /* timeout set (?) */
    if (likely(timeout > 0)){
        the_task->delta_interval = timeout;
        fiber_watchdog_insert(the_task);

        the_task->state &= (~STATES_TRANSIENT);
    }

    /* schedule to another task */
    fiber_sched();

    return (the_task->seizedEvents);
}

/* post event (only used internally) */
int fiber_event_post(FibTCB * the_task, uint64_t events_in){
    /* get the event lock */
    spin_lock(&(the_task->eventlock));

    /* put onto pendingEvents */
    the_task->pendingEvents |= events_in;
    
    /* waiting on events (?) */
    if (unlikely((the_task->state & STATES_WAITFOR_EVENT) == 0)){
        spin_unlock(&(the_task->eventlock));
        return (0);
    }

    /* wakeup the task (?) */
    uint64_t seized_events = the_task->pendingEvents & the_task->waitingEvents;
    if (seized_events && ((seized_events == the_task->waitingEvents) || (the_task->waitingOptions & TASK_EVENT_WAIT_ANY))){
        /* the one who first clear state will set new states & return values */
        the_task->pendingEvents &= (~seized_events);
        the_task->seizedEvents = seized_events;

        the_task->state &= (~STATES_WAITFOR_EVENT);
        spin_unlock(&(the_task->eventlock));

        #ifndef __1_N_MODEL__
        /* waiting for timeout operation finished */
        while (unlikely(the_task->state & STATES_TRANSIENT)){ cpu_relax(); }
        #endif

        /* extract from watchdog list if needed */
        if (likely(the_task->state & STATES_WAIT_TIMEOUTB)){
            fiber_watchdog_remove(the_task);
        }

        FibTCB * the_scheduler = the_maintask;
        if (the_task->scheduler == the_scheduler){
            /* insert into current ready list (before scheduler) */
            localReadyQProtectedInsertBefore(the_scheduler, the_task);
        }
        else{
            /* put onto local ready queue */
            #if (ACTIVATE_TASKS_LOCALE_READYLIST)
            FAS(getLocalFibTasksPtr(the_task     ));
            FAA(getLocalFibTasksPtr(the_scheduler));

            the_task->scheduler = the_scheduler;
            the_task->scheddata = the_scheduler->scheddata;
        
            _CHAIN_INSERT_BEFORE(the_scheduler, the_task);
            #endif

            /* put onto global ready queue */
            #if (ACTIVATE_TASKS_GLOBAL_READYLIST)
            FAS(getLocalFibTasksPtr(the_task));
            pushIntoGlobalReadyList(the_task);
            #endif

            /* put onto target thread's ready queue */
            #if (ACTIVATE_TASKS_TARGET_READYLIST)
            localReadyQProtectedInsertBefore(the_task->scheduler, the_task);
            #endif
        }
    }
    else{
        spin_unlock(&(the_task->eventlock));        
    }

    return (0);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* fiber mutex                                                         */
/////////////////////////////////////////////////////////////////////////
static int fiber_mutex_init_(FibMutex * pmutex){
    memset(pmutex, 0, sizeof(FibMutex));

    spin_init(&(pmutex->qlock));
    _CHAIN_INIT_EMPTY(&(pmutex->waitq));

    return (0);
};

static bool fiber_mutex_lock_(FibMutex * pmutex){
    FibTCB * the_task = current_task;
    if (pmutex->holder == ((uint64_t)the_task)){
        pmutex->reentries += 1;
        return true;
    }

    /* put onto mutex's waiting list */
    spin_lock(&(pmutex->qlock));

    /* try again (in case another thread unlocked it) */
    if (pmutex->holder == 0ULL){
        pmutex->holder = (uint64_t)(the_task);
        spin_unlock(&(pmutex->qlock));

        pmutex->reentries = 1;
        return true;
    }

    /* have to wait */
    the_task->state |= STATES_WAITFOR_MUTEX;

    /* extract from ready list */
    localReadyQProtectedExtract(the_task);

    /* put onto waiting list */
    _CHAIN_INSERT_TAIL(&(pmutex->waitq), the_task);

    spin_unlock(&(pmutex->qlock));

    /* schedule to another task */
    fiber_sched();
    return true;
}

static bool fiber_mutex_unlock_(FibMutex * pmutex){
    FibTCB * the_task = current_task;
    if (pmutex->holder != (uint64_t)(the_task)){
        return false;
    }

    /* check entries */
    --pmutex->reentries;
    if (pmutex->reentries){ return true; }

    FibTCB * the_first = NULL;

    /* wakeup one task waiting on the mutex */
    spin_lock(&(pmutex->qlock));
    the_first = _CHAIN_EXTRACT_FIRST(&(pmutex->waitq));
    pmutex->holder = (uint64_t)(the_first);
    spin_unlock(&(pmutex->qlock));

    if (the_first == NULL){ return true; }

    /* unlock & switch holder */
    pmutex->reentries = 1;

    /* clear the block state */
    the_first->state &= (~STATES_WAITFOR_MUTEX);
    if (likely(the_first->scheduler == the_task->scheduler)){
        /* put onto ready list */
        localReadyQProtectedInsertBefore(the_first->scheduler, the_first);
    }
    else{
        /* put onto local ready queue */
        #if (ACTIVATE_TASKS_LOCALE_READYLIST)
        FAS(getLocalFibTasksPtr(the_first));
        FAA(getLocalFibTasksPtr(the_task ));

        the_first->scheduler = the_task->scheduler;
        the_first->scheddata = the_task->scheddata;

        _CHAIN_INSERT_BEFORE(the_task->scheduler, the_first);
        #endif

        /* put onto global ready queue */
        #if (ACTIVATE_TASKS_GLOBAL_READYLIST)
        FAS(getLocalFibTasksPtr(the_first));
        pushIntoGlobalReadyList(the_first);
        #endif

        /* put onto target thread's ready queue */
        #if (ACTIVATE_TASKS_TARGET_READYLIST)
        localReadyQProtectedInsertBefore(the_first->scheduler, the_first);
        #endif
    }
    return true;
}

static bool fiber_mutex_destroy_(FibMutex * pmtx){
    spin_destroy(&(pmtx->qlock));
    return true;
}

#if !defined(__1_N_MODEL__)
int  fiber_mutex_init   (FibMutex * pmutex) __attribute__((alias("fiber_mutex_init_"  )));
bool fiber_mutex_lock   (FibMutex * pmutex) __attribute__((alias("fiber_mutex_lock_"  )));
bool fiber_mutex_unlock (FibMutex * pmutex) __attribute__((alias("fiber_mutex_unlock_")));
bool fiber_mutex_destroy(FibMutex * pmutex) __attribute__((alias("fiber_mutex_destroy_")));
#endif 
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* fiber semaphore                                                     */
/////////////////////////////////////////////////////////////////////////
int fiber_sem_init(FibSemaphore * psem, int initval){
    memset(psem, 0, sizeof(FibSemaphore));

    spin_init(&(psem->qlock));
    _CHAIN_INIT_EMPTY(&(psem->waitq));

    psem->count = initval;
    return (0);
};

bool fiber_sem_wait(FibSemaphore * psem){
    FibTCB * the_task = current_task;

    /* put onto semaphore's waiting list */
    spin_lock(&(psem->qlock));

    /* decrease the resource count */
    --psem->count;
    if (psem->count >= 0){
        spin_unlock(&(psem->qlock));
        return true;
    }

    /* extract from local ready list */
    localReadyQProtectedExtract(the_task);

    /* insert into semaphore's waitq */
    _CHAIN_INSERT_TAIL(&(psem->waitq), the_task);

    /* set state */
    the_task->state |= STATES_WAITFOR_SEMPH;

    spin_unlock(&(psem->qlock));

    /* schedule to another task */
    fiber_sched();
    return true;
}

bool fiber_sem_timedwait(FibSemaphore * psem, int timeout){
    FibTCB * the_task = current_task;

    /* put onto semaphore's waiting list */
    spin_lock(&(psem->qlock));

    /* decrease the resource count */
    --psem->count;
    if (psem->count >= 0){
        spin_unlock(&(psem->qlock));
        return true;
    }

    if (timeout == 0){
        ++psem->count;
        spin_unlock(&(psem->qlock));
        return false;
    }

    /* extract from local ready list */
    localReadyQProtectedExtract(the_task);

    /* insert into semaphore's waitq */
    _CHAIN_INSERT_TAIL(&(psem->waitq), the_task);

    /* set state */
    the_task->state |= (((timeout > 0) ? (STATES_TRANSIENT) : 0) | STATES_WAITFOR_SEMPH);

    /* set waiting object */
    the_task->waitingObject = (uint64_t)(psem);
    
    /* set return code */
    the_task->yieldCode = 0;

    spin_unlock(&(psem->qlock));

    /* put into watchdog waiting list */
    if (timeout > 0){
        the_task->delta_interval = timeout;
        fiber_watchdog_insert(the_task);

        the_task->state &= (~STATES_TRANSIENT);
    }

    /* schedule to another task */
    fiber_sched();
    return the_task->yieldCode == 1;
}

bool fiber_sem_post(FibSemaphore * psem){
    FibTCB * the_task = current_task;

    /* lock */
    spin_lock(&(psem->qlock));
    
    /* increase the resource count */
    ++psem->count;

    /* wakeup task if possible */
    FibTCB * the_first = _CHAIN_EXTRACT_FIRST(&(psem->waitq));
    if (the_first == NULL){
        spin_unlock(&(psem->qlock));
        return true;
    }

    /* clear the state */
    the_first->state &= (~STATES_WAITFOR_SEMPH);

    /* set return code */
    the_first->yieldCode = 1;

    /* unlock & switch holder */
    spin_unlock(&(psem->qlock));

    #ifndef __1_N_MODEL__
    /* waiting for timeout operation done */
    while (unlikely(the_first->state & STATES_TRANSIENT)){ cpu_relax(); }
    #endif

    /* remove the_first from watchdog list */
    if (unlikely(the_first->state & STATES_WAIT_TIMEOUTB)){
        fiber_watchdog_remove(the_first);
    }

    if (likely(the_first->scheduler == the_task->scheduler)){
        /* clear the block state */
        localReadyQProtectedInsertBefore(the_task->scheduler, the_first);
    }
    else{
        /* put onto local ready queue */
        #if (ACTIVATE_TASKS_LOCALE_READYLIST)
        FAS(getLocalFibTasksPtr(the_first));
        FAA(getLocalFibTasksPtr(the_task ));

        the_first->scheduler = the_task->scheduler;
        the_first->scheddata = the_task->scheddata;

        _CHAIN_INSERT_BEFORE(the_task->scheduler, the_first);
        #endif

        /* put onto global ready queue */
        #if (ACTIVATE_TASKS_GLOBAL_READYLIST)
        FAS(getLocalFibTasksPtr(the_first));
        pushIntoGlobalReadyList(the_first);
        #endif

        /* put onto target thread's ready queue */
        #if (ACTIVATE_TASKS_TARGET_READYLIST)
        localReadyQProtectedInsertBefore(the_first->scheduler, the_first);
        #endif
    }
    return true;
}

bool fiber_sem_destroy(FibSemaphore * psem){
    spin_destroy(&(psem->qlock));
    return true;
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* fiber condition variables (close to semaphore)                      */
/////////////////////////////////////////////////////////////////////////
int fiber_cond_init(FibCondition * pcond){
    memset(pcond, 0, sizeof(FibCondition));

    spin_init(&(pcond->qlock));
    _CHAIN_INIT_EMPTY(&(pcond->waitq));
    return (0);
};

bool fiber_cond_wait(FibCondition * pcond, FibMutex * pmutex){
    FibTCB * the_task = current_task;

    /* grab control of condition's waiting list */
    spin_lock(&(pcond->qlock));

    /* extract from local ready list */
    localReadyQProtectedExtract(the_task);

    /* insert into semaphore's waitq */
    _CHAIN_INSERT_TAIL(&(pcond->waitq), the_task);

    /* set state */
    the_task->state |= STATES_WAITFOR_CONDV;

    spin_unlock(&(pcond->qlock));

    /* release the mutex */
    fiber_mutex_unlock(pmutex);
    
    /* schedule to another task */
    fiber_sched();

    /* acquire the mutex */
    fiber_mutex_lock(pmutex);

    return true;
}

bool fiber_cond_timedwait(FibCondition * pcond, FibMutex * pmutex, int timeout){
    FibTCB * the_task = current_task;

    /* put onto semaphore's waiting list */
    spin_lock(&(pcond->qlock));

    if (timeout == 0){
        spin_unlock(&(pcond->qlock));
        return false;
    }

    /* extract from local ready list */
    localReadyQProtectedExtract(the_task);

    /* insert into condition's waitq */
    _CHAIN_INSERT_TAIL(&(pcond->waitq), the_task);

    /* set state */
    the_task->state |= (((timeout > 0) ? STATES_TRANSIENT : 0U) | STATES_WAITFOR_CONDV);

    /* set waiting object */
    the_task->waitingObject = (uint64_t)(pcond);
    
    /* set return code */
    the_task->yieldCode = 0;

    spin_unlock(&(pcond->qlock));

    /* put into watchdog waiting list */
    if (timeout > 0){
        the_task->delta_interval = timeout;
        fiber_watchdog_insert(the_task);

        /* done put onto timeout queue */
        the_task->state &= (~STATES_TRANSIENT);
    }

    /* release the mutex */
    fiber_mutex_unlock(pmutex);
    
    /* schedule to another task */
    fiber_sched();

    /* acquire the mutex */
    fiber_mutex_lock(pmutex);

    return the_task->yieldCode == 1;
}

bool fiber_cond_signal(FibCondition * pcond){
    FibTCB * the_task = current_task;

    /* lock */
    spin_lock(&(pcond->qlock));

    /* wakeup task if possible */
    FibTCB * the_first = _CHAIN_EXTRACT_FIRST(&(pcond->waitq));
    if (the_first == NULL){
        spin_unlock(&(pcond->qlock));
        return true;
    }

    /* clear waiting state */
    the_first->state &= (~STATES_WAITFOR_CONDV);

    /* set return code for wait */
    the_first->yieldCode = 1;

    /* unlock & switch holder */
    spin_unlock(&(pcond->qlock));

    #ifndef __1_N_MODEL__
    /* waiting for timedwait finished */
    while (unlikely(the_first->state & STATES_TRANSIENT)){  cpu_relax(); }
    #endif

    /* remove the_first from watchdog list */
    if (unlikely(the_first->state & STATES_WAIT_TIMEOUTB)){ 
        fiber_watchdog_remove(the_first);
    }

    if (likely(the_first->scheduler == the_task->scheduler)){
        /* clear the block state */
        localReadyQProtectedInsertBefore(the_task->scheduler, the_first);
    }
    else{
        /* put onto local ready queue */
        #if (ACTIVATE_TASKS_LOCALE_READYLIST)
        FAS(getLocalFibTasksPtr(the_first));
        FAA(getLocalFibTasksPtr(the_task ));

        the_first->scheduler = the_task->scheduler;
        the_first->scheddata = the_task->scheddata;

        _CHAIN_INSERT_BEFORE(the_task->scheduler, the_first);
        #endif

        /* put onto global ready queue */
        #if (ACTIVATE_TASKS_GLOBAL_READYLIST)
        FAS(getLocalFibTasksPtr(the_first));
        pushIntoGlobalReadyList(the_first);
        #endif

        /* put onto target thread's ready queue */
        #if (ACTIVATE_TASKS_TARGET_READYLIST)
        localReadyQProtectedInsertBefore(the_first->scheduler, the_first);
        #endif
    }
    return true;
}

bool fiber_cond_broadcast(FibCondition * pcond){
    FibTCB * the_task = current_task;

    /* collect all waiting tasks */
    fibchain_t localchain;
    _CHAIN_INIT_EMPTY(&localchain);

    FibTCB * the_tcb, * the_nxt;

    /* lock */
    spin_lock(&(pcond->qlock));

    /* wakeup all tasks on waitq */
    _CHAIN_FOREACH_SAFE(the_tcb, &(pcond->waitq), FibTCB, the_nxt){
        /* move tcb to temporary list */
        _CHAIN_REMOVE(the_tcb);
        _CHAIN_INSERT_TAIL(&localchain, the_tcb);

        /* clear waiting state */
        the_tcb->state &= (~STATES_WAITFOR_CONDV);

        /* set return code for wait */
        the_tcb->yieldCode = 1;
    }

    /* unlock */
    spin_unlock(&(pcond->qlock));

    /* make all waiting tasks ready */
    _CHAIN_FOREACH_SAFE(the_tcb, &localchain, FibTCB, the_nxt){
        /* extract from local chain */
        _CHAIN_REMOVE(the_tcb);

        #ifndef __1_N_MODEL__
        /* waiting for timeout operation done! */
        while (unlikely(the_tcb->state & STATES_TRANSIENT)){ cpu_relax(); }
        #endif

        /* remove the_first from watchdog list */
        if (unlikely(the_tcb->state & STATES_WAIT_TIMEOUTB)){
            /* lock on watchdog */
            fiber_watchdog_remove(the_tcb);
        }

        if (likely(the_tcb->scheduler == the_task->scheduler)){
            /* clear the block state */
            localReadyQProtectedInsertBefore(the_task->scheduler, the_tcb);
        }
        else{
            /* put onto local ready queue */
            #if (ACTIVATE_TASKS_LOCALE_READYLIST)
            FAS(getLocalFibTasksPtr(the_tcb ));
            FAA(getLocalFibTasksPtr(the_task));

            the_tcb->scheduler = the_task->scheduler;
            the_tcb->scheddata = the_task->scheddata;

            _CHAIN_INSERT_BEFORE(the_task->scheduler, the_tcb);
            #endif

            /* put onto global ready queue */
            #if (ACTIVATE_TASKS_GLOBAL_READYLIST)
            FAS(getLocalFibTasksPtr(the_tcb));
            pushIntoGlobalReadyList(the_tcb);
            #endif

            /* put onto target thread's ready queue */
            #if (ACTIVATE_TASKS_TARGET_READYLIST)
            localReadyQProtectedInsertBefore(the_tcb->scheduler, the_tcb);
            #endif        
        }
    }

    return true;
}

bool fiber_cond_destroy(FibCondition * pcond){
    spin_destroy(&(pcond->qlock));
    return true;
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* Scheduler Task (the default maintask of a thread)                   */
/////////////////////////////////////////////////////////////////////////
static void * fiber_scheduler(void * args){
    /* user initialization function */
    fibthread_args_t * pargs = (fibthread_args_t *)args;
    if (pargs && pargs->threadStartup && !pargs->threadStartup(pargs->args)){
        return ((void *)(0));
    }
    
    FibTCB * the_scheduler = the_maintask;
    fibchain_t * the_chain = getLocalReadyList(the_scheduler);

    /* get current time (for timeout) */
    uint64_t prev_stmp = _utime();
    while (true){
        /* fire watchdogs */
        uint64_t curr_stmp = _utime();
        uint64_t curr_gapp = curr_stmp - prev_stmp;
        if (likely(curr_gapp)){ 
            fiber_watchdog_tickle(the_scheduler, curr_gapp); 
            prev_stmp = curr_stmp;
        }

        #ifndef __1_N_MODEL__
        /* workload balance between service threads */
        if (unlikely(getLocalFibTasksVal(the_scheduler) < ((nGlobalFibTasks * 16 / 15 + mServiceThreads - 1) / mServiceThreads))){
            /* get a task from global if too few tasks running locally */
            int localgroup = the_scheduler->scheddata->group;
            FibTCB * next_task = popFromGlobalReadyList(the_scheduler, localgroup);
            the_scheduler->scheddata->group = (localgroup + 1) & (MAX_GLOBAL_GROUPS - 1);

            if (unlikely(next_task != NULL)){
                /* insert into local ready list */
                localReadyQProtectedInsertBefore(the_scheduler, next_task);

                /* one more task in local system */
                FAA(getLocalFibTasksPtr(next_task));
            }
        }
        #endif

        /* exhaust all ready fibers */
        while (_CHAIN_FIRST(the_chain) != _CHAIN_LAST(the_chain)) {
            fiber_sched_yield();
        }

        if ((pargs && pargs->threadMsgLoop)){
            pargs->threadMsgLoop(pargs->args);
        }

        // #if (ACTIVATE_TASKS_LOCALE_READYLIST)
        // /* nothing to run, sleep for a while if come to here */
        // __usleep__(10);
        // #endif
    }
}

void * pthread_scheduler(void * args){
    /* one service thread joined */
    FAA(&mServiceThreads);

    /* create maintask (reuse thread's stack) */
    struct {} C;
    FibTCB * the_task = fiber_create(
        fiber_scheduler, args, (void *)(&C), 0UL
        );

    /* set current task to maintask & switch to it */
    goto_contxt2(&(the_task->regs));

    /* one service thread left */
    FAS(&mServiceThreads);

    /* never return here */
    return ((void *)(0));
}
/////////////////////////////////////////////////////////////////////////