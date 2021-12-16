#include <sys/mman.h>

#include <assert.h>
#include <stdio.h>

#include <semaphore.h>
#include <errno.h>

#include "sysdef.h"
#include "spinlock.h"

#include "chain.h"
#include "task.h"

#include "timestamp.h"


static inline int fiber_watchdog_insert(FibTCB * the_task);
static inline int fiber_watchdog_remove(FibTCB * the_task);

typedef struct freelist_t {
    struct freelist_t * next;
} freelist_t;

__thread_local FibSCP localscp;

#define local_freedlist_size (localscp.freedlist_size)

#define local_taskonrun      (localscp.taskonrun     )
#define local_schedmsgq      (localscp.schedmsgq     )
#define local_freedlist      (localscp.freedlist     )
#define local_blocklist      (localscp.blocklist     )
#define local_readylist      (localscp.readylist     )
#define local_wadoglist      (localscp.wadoglist     )

#define local_cached_stack_mask (localscp.cached_stack_mask)
#define local_cached_stack      (localscp.cached_stack     )

__thread_local FibTCB * current_task = NULL;
__thread_local FibTCB * the_maintask = NULL;

__thread_local int64_t nLocalFibTasks = 0;

// #define the_maintask         (the_task->scheduler)

/* global lists (need lock to access) */
static volatile freelist_t *   global_freedlist;
static volatile fibtcb_chain_t global_readylist;

static spinlock spinlock_freedlist = {0};
static spinlock spinlock_readylist = {0};

static volatile struct {
    void *   stackbase;
    uint32_t stacksize;
} global_cached_stack[64];
static volatile uint64_t global_cached_stack_mask;
static spinlock spinlock_cached_stack;

static volatile int64_t mServiceThreads = 0, nGlobalFibTasks = 0;

static sem_t __sem_null;

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
    global_freedlist = NULL;
    _CHAIN_INIT_EMPTY(&global_readylist);

    spin_init(&spinlock_freedlist);
    spin_init(&spinlock_readylist);

    mServiceThreads       = 0;
    nGlobalFibTasks       = 0;

    global_cached_stack_mask = ~0ULL;
    spin_init(&spinlock_cached_stack);

    /* create a null semaphore for timeout */
    sem_init(&__sem_null, 0, 0);

    return true;
};

bool FiberThreadStartup(){
    local_freedlist = NULL;

    _CHAIN_INIT_EMPTY(&local_blocklist);
    _CHAIN_INIT_EMPTY(&local_readylist);
    
   /* watch dog list */
    CHAIN_INIT_EMPTY(&local_wadoglist, FibTCB, link);

    local_freedlist_size = 0;
    nLocalFibTasks       = 0;

    local_cached_stack_mask = 0xFF;
    return true;
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
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* TCB alloc/free                                                      */
/////////////////////////////////////////////////////////////////////////
static inline void tcb_free(FibTCB * tcb){
    freelist_t * node = (freelist_t *)tcb;
    if (likely(local_freedlist_size < MAX_LOCAL_FREED_TASKS)){
        node->next = local_freedlist;
        local_freedlist = node;
        ++local_freedlist_size;
    }
    else{
        spin_lock(&spinlock_freedlist);
        node->next = (freelist_t *)global_freedlist;
        global_freedlist = node;
        spin_unlock(&spinlock_freedlist);
    }
};

static inline FibTCB * tcb_alloc(){
    if (likely(local_freedlist != NULL)){
        FibTCB * tcb = (FibTCB *)local_freedlist;
        local_freedlist = local_freedlist->next;

        --local_freedlist_size;
        return tcb;
    }
    else{
        FibTCB * tcb = NULL;
        spin_lock(&spinlock_freedlist);
        tcb = (FibTCB *)global_freedlist;
        if (likely(tcb)){global_freedlist = global_freedlist->next;}
        spin_unlock(&spinlock_freedlist);
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

    /* put into local cache first */
    int index = __ffs32(local_cached_stack_mask);
    if (unlikely(index == 0)){
        spin_lock(&spinlock_cached_stack);
        index = __ffs64(global_cached_stack_mask);
        if (likely(index != 0)){
            --index;
            global_cached_stack_mask &= (~(1ULL << index));
            global_cached_stack[index].stackbase = stackbase;
            global_cached_stack[index].stacksize = stacksize;
        }
        spin_unlock(&spinlock_cached_stack);

        if (unlikely(index == 0)){
            /* generate a random number */
            uint64_t u64 = ((uint64_t)(stackbase));
            u64 = (u64 >> 8) ^ (u64 >> 16) ^ (u64 >> 24) ^ (u64 >> 32);
            u64 = (u64 >> 4) + (u64 >> 0);
            index =  u64 & 7;

            oldstack = local_cached_stack[index].stackbase;
            oldsize  = local_cached_stack[index].stacksize;

            local_cached_stack[index].stackbase = stackbase;
            local_cached_stack[index].stacksize = stacksize;
        }
    }
    else{
        --index;
        local_cached_stack_mask &= (~(1U << index));
        local_cached_stack[index].stackbase = stackbase;
        local_cached_stack[index].stacksize = stacksize;
    }

    /* free old stack if needed */
    if (oldstack){
        munmap(oldstack, oldsize);
    }

    return true;
}

static inline uint8_t * fiber_stackcache_get(uint32_t * stacksize){
    /* find in local */
    {
        uint32_t mask = (~local_cached_stack_mask) & 0xFF;
        int      index = -1;
        #define callback_setlocalstack(p) do {                      \
            if (local_cached_stack[p].stacksize >= (*stacksize)){   \
                mask  = 0U;                                         \
                index = p;                                          \
            }                                                       \
        } while (0)

        callback_on_setbit(mask, callback_setlocalstack);
        if (index >= 0){
            local_cached_stack_mask |= (1U << index);
            *stacksize = local_cached_stack[index].stacksize;
            return local_cached_stack[index].stackbase;
        }
    }

    /* find in global */
    {
        int       index     = -1;
        uint8_t * stackaddr = NULL;     

        spin_lock(&spinlock_cached_stack);
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
        spin_unlock(&spinlock_cached_stack);
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
            mprotect (
                stackbase + 4096, 
                4096 * ((*stacksize) / 4096 - 1), 
                PROT_READ | PROT_WRITE
                );
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
static void * cpp_taskmain(FibTCB * the_task){
    /* callback taskStartup */
    if (the_task->onTaskStartup){
        the_task->onTaskStartup(the_task);
    }

    /* increase number of fibtasks in system */
    FAA(&nGlobalFibTasks);
    nLocalFibTasks += 1;

    /* call user entry */
    the_task->entry(the_task->args);

    /* decrease number of fibtasks in system */
    FAS(&nGlobalFibTasks);
    nLocalFibTasks -= 1;

    bool is_maintask = (the_task == the_maintask);

    /* callback on taskCleanup */
    if (the_task->onTaskCleanup){
        the_task->onTaskCleanup(the_task);
    }

    /* remove current task from ready list*/
    _CHAIN_REMOVE(the_task);

    // int local_ready = local_readylist_size;
    // printf("%d tasks ready now.\n", local_ready);
    
    /* free the_task */
    tcb_free(the_task);

    /* free stack */
    if (the_task->stacksize & MASK_SYSTEM_STACK){
        /* cannot free at here, put into cache for next thread */
        fiber_stackcache_put(
            the_task->stackaddr,
            the_task->stacksize & (~15UL)
            );
    }

    if (is_maintask){ return (void *)(0); }

    /* switch to thread maintask */
    current_task = the_maintask;
    goto_context(&(the_maintask->regs));

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
    if (the_task == NULL){return NULL;}

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

    /* callbacks */
    the_task->preSwitchingThread  = NULL;
    the_task->postSwitchingThread = NULL;
    the_task->onTaskStartup       = NULL;
    the_task->onTaskCleanup       = NULL;

    /* r12 (tcb), r15 (cpp_taskmain), rip (asm_taskmain), rsp */
    the_task->regs.reg_r12 = (uint64_t)(the_task);
    the_task->regs.reg_r15 = (uint64_t)(cpp_taskmain);
    the_task->regs.reg_rip = (uint64_t)(asm_taskmain);
    the_task->regs.reg_rsp = (uint64_t)(stackbase + (stacksize & (~15UL)));

   /* put next_task onto end of ready list */
    _CHAIN_INSERT_TAIL(&local_readylist, the_task);

    /* usually the first task created of thread is the maintask */
    if (the_maintask == NULL){
        the_maintask = the_task;
        current_task = the_task;
    }

    the_task->scheduler = (the_maintask);
    the_task->scheddata = &localscp;
    return the_task;
}
/////////////////////////////////////////////////////////////////////////


bool fiber_set_thread_maintask(FibTCB * the_task){
    the_maintask = the_task;
    if (current_task == NULL){
        current_task = the_task;
    }
    return true;
}

FibTCB * fiber_ident(){
    return current_task;
}

/////////////////////////////////////////////////////////////////////////
/* schedule to next task                                               */
/////////////////////////////////////////////////////////////////////////
static inline void fiber_sched(){

    /* find next ready task */
    FibTCB * next_task = NULL, * the_task = current_task;
    {
        /* only get a task from local if possible */
        next_task = _CHAIN_FIRST(&local_readylist);
        if (likely(next_task != the_task)){
            _CHAIN_REMOVE(next_task);
        }
        else{
            /* cannot find a candidate, 
               if and only if called from maintask and only maintask running,
               go back to epoll 
             */
            return;
        }
    };

    current_task = next_task;

    /* put next_task onto end of ready list */
    _CHAIN_INSERT_TAIL(&local_readylist, next_task);

    /* swap context and jump to next task */
    swap_context(&(the_task->regs), &(next_task->regs));
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* set / clear blocking states                                         */
/////////////////////////////////////////////////////////////////////////
static inline int fiber_setstate(FibTCB * the_task, uint64_t states){
    /* set state */
    the_task->state |= states;

    /* extract from ready list */
    _CHAIN_REMOVE(the_task);

    /* insert into blocked list */
    _CHAIN_INSERT_TAIL(&local_blocklist, the_task);

    /* fiber_sched to next task */
    fiber_sched();

    return (0);
}

static inline int fiber_clrstate(FibTCB * the_task, uint64_t states){
    /* check those states are set (?) */
    if (unlikely((the_task->state & states) == 0)){
        return (0);
    }

    /* clear the state */
    the_task->state &= (~states);
    if (unlikely(the_task->state & STATES_BLOCKED)){
        return (0);
    }

    /* extract from blocked list */
    _CHAIN_REMOVE(the_task);

    /* put into global ready list if too much tasks in local list */
    if ((the_task != the_maintask) && (nLocalFibTasks > ((nGlobalFibTasks * 9 / 8 + mServiceThreads - 1) / mServiceThreads))){
        /* call the callback when switching thread */
        if (the_task->preSwitchingThread){
            the_task->preSwitchingThread(the_task);
        }
        
        /* put onto global ready list */
        spin_lock(&spinlock_readylist);
        _CHAIN_INSERT_TAIL(&global_readylist, the_task);
        spin_unlock(&spinlock_readylist);

        /* decrease local tasks */
        nLocalFibTasks -= 1;
    }
    else{
        _CHAIN_INSERT_TAIL(&local_readylist, the_task);
    }
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
    int yieldCode = the_task->yieldCode;
    fiber_clrstate(the_task, STATES_SUSPENDED);
    return (yieldCode);
}

__force_noinline FibTCB * fiber_sched_yield(){
    FibTCB * the_task = current_task;

    /* move to end of ready list */
    _CHAIN_REMOVE(the_task);
    _CHAIN_INSERT_TAIL(&local_readylist, the_task);

    /* fiber_sched to next task */
    fiber_sched();

    return (the_task);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* wait/post events (only used internally)                             */
/////////////////////////////////////////////////////////////////////////
uint64_t fiber_event_wait(uint64_t events_in, int options, int timeout){
    FibTCB * the_task = current_task;
    
    /* check the pending events */
    uint64_t seized_events = the_task->pendingEvents & events_in;
    if (seized_events && ((seized_events == events_in) || (options & TASK_EVENT_WAIT_ANY))){
        the_task->pendingEvents &= (~seized_events);
        return seized_events;
    }

    /* no wait, test only (?) */
    if (unlikely(timeout == 0)){
        return 0ULL;
    }

    uint64_t states = STATES_WAITFOR_EVENT; 
    the_task->seizedEvents   = 0ULL;
    the_task->waitingEvents  = events_in;
    the_task->waitingOptions = options;

    /* timeout set (?) */
    if (likely(timeout > 0)){
        states |= STATES_WAIT_TIMEOUTB;
        the_task->delta_interval = timeout;
        fiber_watchdog_insert(the_task);
    }

    fiber_setstate(the_task, states);
    return (the_task->seizedEvents);
}

/* post event (only used internally) */
int fiber_event_post(FibTCB * the_task, uint64_t events_in){
    /* check target task running on same thread (?) */
    if (the_task->scheduler != the_maintask){
        return fiber_send_message_internal(
            the_task, 
            MSG_TYPE_SCHEDULER,
            MSG_CODE_POSTEVENT,
            NULL,
            events_in
            );
    }

    /* put onto pendingEvents */
    the_task->pendingEvents |= events_in;
    
    /* waiting on events (?) */
    if (unlikely((the_task->state & (STATES_WAITFOR_EVENT | STATES_WAIT_TIMEOUTB)) == 0)){
        return (0);
    }

    /* wakeup the task (?) */
    uint64_t seized_events = the_task->pendingEvents & the_task->waitingEvents;
    if (seized_events && ((seized_events == the_task->waitingEvents) || (the_task->waitingOptions & TASK_EVENT_WAIT_ANY))){
        the_task->pendingEvents &= (~seized_events);
        the_task->seizedEvents = seized_events;

        /* extract from watchdog list if needed */
        if (likely(the_task->state & STATES_WAIT_TIMEOUTB)){
            fiber_watchdog_remove(the_task);
        }

        /* clear WAITFOR_EVENT and WAIT_TIMEOUTB sytates */
        fiber_clrstate(the_task, (STATES_WAITFOR_EVENT | STATES_WAIT_TIMEOUTB));
    }
    return (0);
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
    FibTCB * after = CHAIN_FIRST(&local_wadoglist);
    int delta_interval = the_task->delta_interval;
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
    return (0);
}

static inline int fiber_watchdog_remove(FibTCB * the_task){
    FibTCB * nxt_tcb = CHAIN_NEXT(the_task, link);
    if (CHAIN_NEXT(nxt_tcb, link)){
        nxt_tcb->delta_interval += the_task->delta_interval;
    }
    CHAIN_REMOVE(the_task, FibTCB, link);

    return (0);
}

/* this functions ias called from thread maintask (scheduling task) */
int fiber_watchdog_tickle(int gap){
    FibTCB * the_task, * the_nxt;
    CHAIN_FOREACH_SAFE(the_task, &local_wadoglist, link, the_nxt){
        if (the_task->delta_interval <= gap){
            CHAIN_REMOVE(the_task, FibTCB, link);

            /* make state be volatile (it may get changed in other thread) */
            volatile uint32_t * pstate = &(the_task->state);
            if ((*pstate) & (STATES_WAITFOR_SEMPH | STATES_WAITFOR_CONDV)){
                /* Semaphore & Condition Variable waitq at same position */
                FibSemaphore * psem = (FibSemaphore *)(the_task->waitingObject);
                spin_lock(&(psem->qlock));
                if ((*pstate) & STATES_TRANSIENT){
                    /* extracted from waitq by another thread 
                     * do nothing now
                     */
                    spin_unlock(&(psem->qlock));
                }
                else{
                    /* extract from semaphore's waiting q */
                    _CHAIN_REMOVE(the_task);
                    spin_unlock(&(psem->qlock));

                    /* insert into blocklist & clear state */
                    _CHAIN_INSERT_TAIL(&local_blocklist, the_task);

                    /* make it ready */
                    fiber_clrstate(the_task, STATES_BLOCKED);
                }
            }
            else{
                /* make it ready */
                fiber_clrstate(the_task, STATES_BLOCKED);
            }
        }
        else{
            the_task->delta_interval -= gap;
            break;
        }
    }

    return (0);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* fiber mutex                                                         */
/////////////////////////////////////////////////////////////////////////
int fiber_mutex_init(FibMutex * pmutex){
    memset(pmutex, 0, sizeof(FibMutex));

    spin_init(&(pmutex->qlock));
    _CHAIN_INIT_EMPTY(&(pmutex->waitq));

    return (0);
};

bool fiber_mutex_lock(FibMutex * pmutex){
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
    _CHAIN_REMOVE(the_task);

    /* put onto waiting list */
    _CHAIN_INSERT_TAIL(&(pmutex->waitq), the_task);

    spin_unlock(&(pmutex->qlock));

    /* schedule to another task */
    fiber_sched();
    return true;
}

bool fiber_mutex_unlock(FibMutex * pmutex){
    FibTCB * the_task = current_task;
    if (pmutex->holder != (uint64_t)(the_task)){
        return false;
    }

    /* check entries */
    pmutex->reentries -= 1;
    if (pmutex->reentries){
        return true;
    }

    FibTCB * the_first = NULL;

    /* wakeup one task waiting on the mutex */
    spin_lock(&(pmutex->qlock));
    the_first = _CHAIN_EXTRACT_FIRST(&(pmutex->waitq));
    pmutex->holder = (uint64_t)(the_first);
    spin_unlock(&(pmutex->qlock));

    if (the_first == NULL){
        return true;
    }

    /* unlock & switch holder */
    pmutex->reentries = 1;

    if (the_first->scheduler == the_task->scheduler){
        /* insert the_first into blocked list */
        _CHAIN_INSERT_TAIL(&(local_blocklist), the_first);

        /* clear the block state */
        fiber_clrstate(the_first, STATES_WAITFOR_MUTEX);
        return (true);
    }
    else{
        /* activate by its scheduler */
        return fiber_send_message_internal(
            the_first,
            MSG_TYPE_SCHEDULER,
            MSG_CODE_ACTIVATED,
            (void *)pmutex, 
            STATES_WAITFOR_MUTEX
            );
    }
}

bool fiber_mutex_destroy(FibMutex * pmtx){
    spin_destroy(&(pmtx->qlock));
    return true;
}
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

    /* check resource count again once locked */
    if (psem->count >= 0){
        spin_unlock(&(psem->qlock));
        return true;
    }

    /* extract from local ready list */
    _CHAIN_REMOVE(the_task);

    /* insert into semaphore's waitq */
    _CHAIN_INSERT_TAIL(&(psem->waitq), the_task);

    /* set state */
    the_task->state |= STATES_WAITFOR_SEMPH;

    /* set waiting object */
    the_task->waitingObject = (uint64_t)(psem);

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

    /* check resource count again once locked */
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
    _CHAIN_REMOVE(the_task);

    /* insert into semaphore's waitq */
    _CHAIN_INSERT_TAIL(&(psem->waitq), the_task);

    /* set state */
    the_task->state |= (((timeout > 0) ? STATES_WAIT_TIMEOUTB : 0) | STATES_WAITFOR_SEMPH);

    /* set waiting object */
    the_task->waitingObject = (uint64_t)(psem);
    
    spin_unlock(&(psem->qlock));

    /* put into watchdog waiting list */
    if (timeout > 0){
        the_task->delta_interval = timeout;
        fiber_watchdog_insert(the_task);
    }

    /* schedule to another task */
    fiber_sched();
    return true;
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

    /* the task is on transient now */
    volatile uint32_t * pstate = &(the_first->state);
    *pstate |= STATES_TRANSIENT;

    /* unlock & switch holder */
    spin_unlock(&(psem->qlock));

    if (the_first->scheduler == the_task->scheduler){
        /* insert the_first into blocked list */
        _CHAIN_INSERT_TAIL(&(local_blocklist), the_first);

        /* clear the block state */
        fiber_clrstate(
            the_first, 
            STATES_WAITFOR_SEMPH | STATES_TRANSIENT | STATES_WAIT_TIMEOUTB
            );
        return (true);
    }
    else{
        /* activate by its scheduler */
        return fiber_send_message_internal(
            the_first,
            MSG_TYPE_SCHEDULER,
            MSG_CODE_ACTIVATED,
            (void *)psem, 
            STATES_WAITFOR_SEMPH | STATES_TRANSIENT | STATES_WAIT_TIMEOUTB
            );
    }
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
    _CHAIN_REMOVE(the_task);

    /* insert into semaphore's waitq */
    _CHAIN_INSERT_TAIL(&(pcond->waitq), the_task);

    /* set state */
    the_task->state |= STATES_WAITFOR_CONDV;

    /* set waiting object */
    the_task->waitingObject = (uint64_t)(pcond);

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
    _CHAIN_REMOVE(the_task);

    /* insert into condition's waitq */
    _CHAIN_INSERT_TAIL(&(pcond->waitq), the_task);

    /* set state */
    the_task->state |= (((timeout > 0) ? STATES_WAIT_TIMEOUTB : 0) | STATES_WAITFOR_CONDV);

    /* set waiting object */
    the_task->waitingObject = (uint64_t)(pcond);
    
    spin_unlock(&(pcond->qlock));

    /* put into watchdog waiting list */
    if (timeout > 0){
        the_task->delta_interval = timeout;
        fiber_watchdog_insert(the_task);
    }

    /* release the mutex */
    fiber_mutex_unlock(pmutex);
    
    /* schedule to another task */
    fiber_sched();

    /* acquire the mutex */
    fiber_mutex_lock(pmutex);

    return true;
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

    /* the task is on transient now (write back to memory) */
    volatile uint32_t * pstate = &(the_first->state);
    *pstate |= STATES_TRANSIENT;

    /* unlock & switch holder */
    spin_unlock(&(pcond->qlock));

    if (the_first->scheduler == the_task->scheduler){
        /* insert the_first into blocked list */
        _CHAIN_INSERT_TAIL(&(local_blocklist), the_first);

        /* clear the block state */
        fiber_clrstate(
            the_first, 
            STATES_WAITFOR_CONDV | STATES_TRANSIENT | STATES_WAIT_TIMEOUTB
            );
        return (true);
    }
    else{
        /* activate by its scheduler */
        return fiber_send_message_internal(
            the_first,
            MSG_TYPE_SCHEDULER,
            MSG_CODE_ACTIVATED,
            (void *)pcond, 
            STATES_WAITFOR_CONDV | STATES_TRANSIENT | STATES_WAIT_TIMEOUTB
            );
    }
}

bool fiber_cond_boardcast(FibCondition * pcond){
    FibTCB * the_task = current_task;

    /* collect all waiting tasks */
    fibtcb_chain_t localchain;
    _CHAIN_INIT_EMPTY(&localchain);

    FibTCB * the_tcb, * the_nxt;

    /* lock */
    spin_lock(&(pcond->qlock));

    /* wakeup all tasks on waitq */
    _CHAIN_FOREACH_SAFE(the_tcb, &(pcond->waitq), FibTCB, the_nxt){
        _CHAIN_REMOVE(the_tcb);
        _CHAIN_INSERT_TAIL(&localchain, the_tcb);

        /* the task is on transient now (write back to memory) */
        volatile uint32_t * pstate = &(the_tcb->state);
        *pstate |= STATES_TRANSIENT;
    }

    /* unlock */
    spin_unlock(&(pcond->qlock));

    /* make all waiting tasks ready */
    _CHAIN_FOREACH_SAFE(the_tcb, &localchain, FibTCB, the_nxt){
        /* extract from local chain */
        _CHAIN_REMOVE(the_tcb);

        if (the_tcb->scheduler == the_task->scheduler){
            /* insert the_first into blocked list */
            _CHAIN_INSERT_TAIL(&(local_blocklist), the_tcb);

            /* clear the block state */
            fiber_clrstate(
                the_tcb, 
                STATES_WAITFOR_CONDV | STATES_TRANSIENT | STATES_WAIT_TIMEOUTB
                );
            return (true);
        }
        else{
            /* activate by its scheduler */
            return fiber_send_message_internal(
                the_tcb,
                MSG_TYPE_SCHEDULER,
                MSG_CODE_ACTIVATED,
                (void *)pcond, 
                STATES_WAITFOR_CONDV | STATES_TRANSIENT | STATES_WAIT_TIMEOUTB
                );
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
/* fiber bounded message queue                                         */
/////////////////////////////////////////////////////////////////////////
bool fiber_msgq_init(FibMsgQ * pq, int qsize, int dsize, void (*copydatafunc)(void *, const void *)){
    fiber_sem_init(&(pq->semSpac), qsize);
    fiber_sem_init(&(pq->semData), 0    );

    pq->buffer = (uint8_t *)malloc(qsize * dsize);
    pq->head = pq->tail = 0;
    pq->qsize = qsize;
    pq->dsize = dsize;
    pq->copyfunc = copydatafunc;

    return true;
}

bool fiber_msgq_push(FibMsgQ * pq, const void * data){
    fiber_sem_wait(&(pq->semSpac));

    int64_t wptr = FAA(&(pq->head));
    wptr = wptr % ((uint64_t)(pq->qsize));
    pq->copyfunc((void *)(pq->buffer + wptr * pq->dsize), data);

    fiber_sem_post(&(pq->semData));
    return true;
}

bool fiber_msgq_pop(FibMsgQ * pq, void * data){
    fiber_sem_wait(&(pq->semData));

    int64_t rptr = FAA(&(pq->tail));
    rptr = rptr % ((uint64_t)(pq->qsize));
    pq->copyfunc(data, (const void *)(pq->buffer + rptr * pq->dsize));

    fiber_sem_post(&(pq->semSpac));
    return true;
}

bool fiber_msgq_destroy(FibMsgQ * pq){
    fiber_sem_destroy(&(pq->semSpac));
    fiber_sem_destroy(&(pq->semData));

    free(pq->buffer);
    return true;
};
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* Scheduler Task (the default maintask of a thread)                   */
/////////////////////////////////////////////////////////////////////////
typedef struct _schedmsgnode_t {
   int32_t type;
   int32_t code;
   void *  data;
   void *  user;
   int64_t valu;
} schedmsgnode_t;

#define copymsg(from, to) do {  \
   (to)->type = (from)->type;   \
   (to)->code = (from)->code;   \
   (to)->data = (from)->data;   \
   (to)->user = (from)->user;   \
   (to)->valu = (from)->valu;   \
} while (0)

#ifdef __SCHEDULER_USING_BLOCKQ__
RBQ_PROTOTYPE_STATIC(schedmsgq, schedmsgnode_t, copymsg, 10000ULL,   16);
#else
RBQ_PROTOTYPE_STATIC(schedmsgq, schedmsgnode_t, copymsg, __usleep__, 16);
#endif

__thread_local schedmsgq_t schedmsgq;
static void * fiber_scheduler(void * args){
    /* user initialization function */
    fibthread_args_t * pargs = (fibthread_args_t *)args;
    if (!pargs->init_func(pargs->args)){
        // pthread_exit(-1);
        return ((void *)(0));
    }
    
    /* get current time (for timeout) */
    uint64_t prev_stmp = _utime();

    schedmsgnode_t msg;
    while (true){
        if (schedmsgq_pop(&schedmsgq, &msg)){
            switch (msg.type)
            {
                case MSG_TYPE_SCHEDULER:
                switch (msg.code)
                {
                    case MSG_CODE_ACTIVATED:
                    {
                        FibTCB * the_task = (FibTCB *)(msg.data);
                        uint64_t mask     = msg.valu;

                        if (the_task->state & mask){
                            /* put into local block list & clear the state */
                            _CHAIN_INSERT_TAIL(&local_blocklist, the_task);

                            /* clear block state, make it ready */
                            fiber_clrstate(the_task, mask);
                        }
                    }
                    break;

                    case MSG_CODE_POSTEVENT:
                    {
                        FibTCB * the_task = (FibTCB *)(msg.data);
                        uint64_t events  = msg.valu;
                        // printf("scheduler: %p events %04x\n", the_task, events);
                        fiber_event_post(the_task, events);
                    }
                    break;

                    default:;
                }
                break;

                default:;
            }
        }
        else {
            /* fire watchdogs */
            uint64_t curr_stmp = _utime();
            uint64_t curr_gapp = curr_stmp - prev_stmp;
            fiber_watchdog_tickle(curr_gapp);
            prev_stmp = curr_stmp;
    

            /* workload balance between service threads */
            FibTCB * next_task = NULL;
            if (nLocalFibTasks < ((nGlobalFibTasks + mServiceThreads - 1) / mServiceThreads)){
                /* get a task from global if too few tasks running locally */
                spin_lock(&spinlock_readylist);
                next_task = _CHAIN_EXTRACT_FIRST(&global_readylist);
                spin_unlock(&spinlock_readylist);

                if (likely(next_task != NULL)){
                    /* one more task in local system */
                    nLocalFibTasks += 1;

                    /* switch scheduler & local data */
                    next_task->scheduler = the_maintask;
                    next_task->scheddata = &localscp;

                    /* call the callback after switching thread */
                    if (next_task->postSwitchingThread){
                        next_task->postSwitchingThread(next_task);
                    }
                }
            }
        }

        /* sleep if only schediuler ready to run otherwise yield */
        if (_CHAIN_FIRST(&local_readylist) == _CHAIN_LAST(&local_readylist)) {
            __usleep__(10);
        }
        else {
            fiber_sched_yield();
        }
    }
}

void * pthread_scheduler(void * args){
    /* initialize thread environment */
    FiberThreadStartup();

    schedmsgq_init(&schedmsgq);

    /* one service thread joined */
    mServiceThreads += 1;

    /* create maintask (reuse thread's stack) */
    struct {} C;
    FibTCB * the_task = fiber_create(
        fiber_scheduler, args, (void *)(&C), 0UL
        );

    /* set mesage queue */
    the_task->scheddata->schedmsgq = (void *)(&schedmsgq);

    /* set current task to maintask & switch to it */
    goto_contxt2(&(the_task->regs));

    /* one service thread left */
    mServiceThreads -= 1;

    /* never return here */
    return ((void *)(0));
}

/* send message to another task (internally using only) */
int fiber_send_message_internal(FibTCB * the_task, uint32_t type, uint32_t msgc, void * user, uint64_t valu){
    schedmsgnode_t msg = {
        .type = type,
        .code = msgc,
        .data = (void *)(the_task),
        .user = user,
        .valu = valu,
    };
    return schedmsgq_push((schedmsgq_t *)(the_task->scheddata->schedmsgq), &msg);
};

/* user send message to another task */
int fiber_send_message(FibTCB * the_task, uint32_t msgc, void * user, uint64_t valu){
    schedmsgnode_t msg = {
        .type = MSG_TYPE_USERPOSTD,
        .code = msgc,
        .data = (void *)(the_task),
        .user = user,
        .valu = valu,
    };
    return schedmsgq_push((schedmsgq_t *)(the_task->scheddata->schedmsgq), &msg);
};
/////////////////////////////////////////////////////////////////////////