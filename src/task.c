#include <sys/mman.h>

#include <assert.h>
#include <stdio.h>

#include "sysdef.h"
#include "spinlock.h"

#include "chain.h"
#include "task.h"

#include "timestamp.h"

static inline int fiber_watchdog_insert(FibTCB * the_tcb);
static inline int fiber_watchdog_remove(FibTCB * the_tcb);

typedef struct freelist_t {
    struct freelist_t * next;
} freelist_t;

typedef CHAIN_HEAD(fibtcb_chain, FibTCB) fibtcb_chain_t;

/* thread cached lists */
static __thread_local freelist_t *   local_freedlist = NULL;
static __thread_local fibtcb_chain_t local_blocklist;
static __thread_local fibtcb_chain_t local_readylist;

static __thread_local fibtcb_chain_t local_wadoglist;

static __thread_local int local_freedlist_size = 0;
static __thread_local int local_readylist_size = 0;

static __thread_local FibTCB * current_task = NULL;
static __thread_local FibTCB * the_maintask = NULL;

static __thread_local struct {
    void *   stackbase;
    uint32_t stacksize;
} local_cached_stack[8];
static __thread_local uint32_t local_cached_stack_mask;

/* global lists (need lock to access) */
static volatile freelist_t *   global_freedlist;
static volatile fibtcb_chain_t global_readylist;

static spinlock spinlock_freedlist = {0};
static spinlock spinlock_readylist = {0};

static volatile int global_readylist_size = 0;

static volatile struct {
    void *   stackbase;
    uint32_t stacksize;
} global_cached_stack[64];
static volatile uint64_t global_cached_stack_mask;
static spinlock spinlock_cached_stack;

static volatile int64_t mServiceThreads = 0, nFibTaskInSystem = 0;

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

    global_readylist_size = 0;
    mServiceThreads  = 0;
    nFibTaskInSystem = 0;

    global_cached_stack_mask = ~0ULL;
    spin_init(&spinlock_cached_stack);

    return true;
};

bool FiberThreadStartup(){
    local_freedlist = NULL;

    _CHAIN_INIT_EMPTY(&local_blocklist);
    _CHAIN_INIT_EMPTY(&local_readylist);
    
   /* watch dog list */
    CHAIN_INIT_EMPTY(&local_wadoglist, FibTCB, link);

    local_freedlist_size = 0;
    local_readylist_size = 0;

    local_cached_stack_mask = 0xFF;
    return true;
};
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
static void * cpp_taskmain(FibTCB * the_tcb){
    /* callback taskStartup */
    if (the_tcb->onTaskStartup){
        the_tcb->onTaskStartup(the_tcb);
    }

    /* increase number of fibtasks in system */
    FAA(&nFibTaskInSystem);

    /* call user entry */
    the_tcb->entry(the_tcb->args);

    /* decrease number of fibtasks in system */
    FAS(&nFibTaskInSystem);

    bool is_maintask = (the_tcb == the_maintask);

    /* callback on taskCleanup */
    if (the_tcb->onTaskCleanup){
        the_tcb->onTaskCleanup(the_tcb);
    }

    /* remove current task from ready list*/
    _CHAIN_REMOVE(the_tcb);
    --local_readylist_size;

    // int local_ready = local_readylist_size;
    // printf("%d tasks ready now.\n", local_ready);
    
    /* free the_tcb */
    tcb_free(the_tcb);

    /* free stack */
    if (the_tcb->stacksize & MASK_SYSTEM_STACK){
        /* cannot free at here, put into cache for next thread */
        fiber_stackcache_put(
            the_tcb->stackaddr,
            the_tcb->stacksize & (~15UL)
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
    ++local_readylist_size;

    /* usually the first task created of thread is the maintask */
    if (the_maintask == NULL){
        the_maintask = the_task;
        current_task = the_task;
    }
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
    while (true) {
        /* only get a task from local if possible */
        next_task = _CHAIN_FIRST(&local_readylist);
        if (likely(next_task != the_task)){
            _CHAIN_REMOVE(next_task);
            --local_readylist_size;

            break;
        }

        /* get a task from global */
        spin_lock(&spinlock_readylist);
        next_task = _CHAIN_EXTRACT_FIRST(&global_readylist);
        if (likely(next_task != NULL)){--global_readylist_size;};
        spin_unlock(&spinlock_readylist);

        if (likely(next_task != NULL)){
            /* add all polling events */
            if (next_task->postSwitchingThread){
                next_task->postSwitchingThread(next_task);
            }
            break;
        }

        /* cannot find a candidate, 
           if and only if called from maintask and only maintask running,
           go back to epoll 
         */
        if (current_task == the_maintask){
            return;
        }
    };

    current_task = next_task;

    /* put next_task onto end of ready list */
    _CHAIN_INSERT_TAIL(&local_readylist, next_task);
    ++local_readylist_size;

    /* swap context and jump to next task */
    swap_context(&(the_task->regs), &(next_task->regs));
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* set / clear blocking states                                         */
/////////////////////////////////////////////////////////////////////////
static inline int fiber_setstate(FibTCB * the_task, uint64_t states){
    /* decrease # of ready task in local list */ 
    if (likely((the_task->state & STATES_BLOCKED) == 0)){
        --local_readylist_size;
    }

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
    if (unlikely((the_task != the_maintask) && (local_readylist_size >= MAX_LOCAL_READY_TASKS))){
        /* delete all polling events */
        if (the_task->preSwitchingThread){
            the_task->preSwitchingThread(the_task);
        }
        
        /* put onto global ready list */
        spin_lock(&spinlock_readylist);
        _CHAIN_INSERT_TAIL(&global_readylist, the_task);
        ++global_readylist_size;
        spin_unlock(&spinlock_readylist);
    }
    else{
        _CHAIN_INSERT_TAIL(&local_readylist, the_task);
        ++local_readylist_size;
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

FibTCB * fiber_sched_yield(){
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
uint64_t fiber_wait(uint64_t events_in, int options, int timeout){
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
int fiber_post(FibTCB * the_task, uint64_t events_in){
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
static inline int fiber_watchdog_insert(FibTCB * the_tcb){
    FibTCB * after = CHAIN_FIRST(&local_wadoglist);
    int delta_interval = the_tcb->delta_interval;
    for (;;after = CHAIN_NEXT(after, link)){

        if (delta_interval == 0 || !CHAIN_NEXT(after, link)){ break; }

        if (delta_interval < after->delta_interval) {
            after->delta_interval -= delta_interval;
            break;
        }

        delta_interval -= after->delta_interval;
    }
    the_tcb->delta_interval = delta_interval;
    CHAIN_INSERT_BEFORE(after, the_tcb, FibTCB, link);
    return (0);
}

static inline int fiber_watchdog_remove(FibTCB * the_tcb){
    FibTCB * nxt_tcb = CHAIN_NEXT(the_tcb, link);
    if (CHAIN_NEXT(nxt_tcb, link)){
        nxt_tcb->delta_interval += the_tcb->delta_interval;
    }
    CHAIN_REMOVE(the_tcb, FibTCB, link);

    return (0);
}

/* this functions ias called from thread maintask (scheduling task) */
int fiber_watchdog_tickle(int gap){
    FibTCB * the_tcb, * the_nxt;
    CHAIN_FOREACH_SAFE(the_tcb, &local_wadoglist, link, the_nxt){
        if (the_tcb->delta_interval <= gap){
            CHAIN_REMOVE(the_tcb, FibTCB, link);

            /* make it ready */
            fiber_clrstate(the_tcb, STATES_BLOCKED);
        }
        else{
            the_tcb->delta_interval -= gap;
            break;
        }
    }

    return (0);
}
/////////////////////////////////////////////////////////////////////////