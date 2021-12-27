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
static inline int fiber_watchdog_tickle(int gap);;

typedef struct freelist_t {
    struct freelist_t * next;
} freelist_t;

__thread_local FibSCP localscp;

#define local_freedlist_size (localscp.freedlist_size)

#define local_freedlist      (localscp.freedlist     )
#define local_readylist      (localscp.readylist     )
#define local_wadoglist      (localscp.wadoglist     )

#define local_cached_stack_mask (localscp.cached_stack_mask)
#define local_cached_stack      (localscp.cached_stack     )

__thread_local FibTCB * current_task = NULL;
__thread_local FibTCB * the_maintask = NULL;

#define getLocalFibTasksPtr(the_task) ((volatile int64_t *)(&((the_task)->scheddata->nLocalFibTasks)))

/* global lists (need lock to access) */
static volatile freelist_t * global_freedlist;
static fibtcb_chain_t global_readylist[MAX_GLOBAL_GROUPS];

static spinlock spinlock_freedlist = {0};
static spinlock spinlock_readylist[MAX_GLOBAL_GROUPS] = {0};

static volatile struct {
    void *   stackbase;
    uint32_t stacksize;
} global_cached_stack[64];
static volatile uint64_t global_cached_stack_mask;
static spinlock spinlock_cached_stack;

static volatile int64_t mServiceThreads = 0, nGlobalFibTasks = 0;

static sem_t __sem_null;

static inline void pushIntoGlobalReadyList(FibTCB * the_task){
	int group = the_task->group;
	fibtcb_chain_t * the_chain = &global_readylist[group];
	spinlock * the_lock = &spinlock_readylist[group];

	spin_lock(the_lock);
	the_task->scheduler = NULL;
	the_task->scheddata = NULL;
	_CHAIN_INSERT_TAIL(the_chain, the_task);
	spin_unlock(the_lock);
};

static inline FibTCB * popFromGlobalReadyList(int group){
	fibtcb_chain_t * the_chain = &global_readylist[group];
	spinlock * the_lock = &spinlock_readylist[group];
	FibTCB * the_task = NULL;
	spin_lock(the_lock);
	the_task = _CHAIN_EXTRACT_FIRST(the_chain);
	if (the_task){
		the_task->scheduler = the_maintask;
		the_task->scheddata = &localscp;

		/* make it ready */
		the_task->state &= (~STATES_BLOCKED);
	}
	spin_unlock(the_lock);	
	return the_task;
};

#if (0)
static inline void pushIntoBackupList(FibTCB * the_task){
	fibtcb_chain_t * the_chain = &(the_task->scheddata->backplist);
	spinlock * the_lock = &(the_task->scheddata->backplock);

	spin_lock(the_lock);
	/* make it ready */
	the_task->state &= (~STATES_BLOCKED);
	_CHAIN_INSERT_TAIL(the_chain, the_task);
	spin_unlock(the_lock);
};
#endif

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
	/* inialize tcb freedlist */
    global_freedlist = NULL;
    spin_init(&spinlock_freedlist);

    /* initialize global ready list */
    for (int i = 0; i < MAX_GLOBAL_GROUPS; ++i){ 
    	_CHAIN_INIT_EMPTY(&global_readylist[i]); 
    	spin_init(&spinlock_readylist[i]); 
    }

    /* statistical */
    mServiceThreads       = 0;
    nGlobalFibTasks       = 0;

    global_cached_stack_mask = ~0ULL;
    spin_init(&spinlock_cached_stack);

    /* create a null semaphore for timeout */
    sem_init(&__sem_null, 0, 0);

    return true;
};

bool FiberThreadStartup(){
	/* local freedlist & readylist */
    local_freedlist = NULL;
    _CHAIN_INIT_EMPTY(&local_readylist);
    
    /* watch dog list */
    CHAIN_INIT_EMPTY(&local_wadoglist, FibTCB, link);
    spin_init(&localscp.wadoglock);

    local_freedlist_size = 0;
    localscp.nLocalFibTasks = 0;

    /* backup list */
    // spin_init(&localscp.backplock);
    // _CHAIN_INIT_EMPTY(&localscp.backplist);

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
static inline uint32_t hash6432shift(uint64_t key)
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

    // if ((the_task->scheduler == NULL) || (the_task->scheddata == NULL)){
    //     printf("task scheduler not set!\n");
    //     the_task->scheduler = (the_maintask);
    //     the_task->scheddata = &localscp;
    // }

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
    _CHAIN_REMOVE(the_task);
    
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
    the_task->onTaskStartup       = NULL;
    the_task->onTaskCleanup       = NULL;

    /* r12 (tcb), r15 (cpp_taskmain), rip (asm_taskmain), rsp */
    the_task->regs.reg_r12 = (uint64_t)(the_task);
    the_task->regs.reg_r15 = (uint64_t)(cpp_taskmain);
    the_task->regs.reg_rip = (uint64_t)(asm_taskmain);
    the_task->regs.reg_rsp = (uint64_t)(stackbase + (stacksize & (~15UL)));

    /* usually the first task created of thread is the maintask */
    if (the_maintask == NULL){
        the_maintask = the_task;
        current_task = the_task;
    }

    /* load maintask to scheduler */
    FibTCB * the_scheduler = the_maintask;

    /* increase number of fibtasks in system */
    FAA(&nGlobalFibTasks);

    /* load balance when task first created */
    if (unlikely((the_task != the_scheduler) && (localscp.nLocalFibTasks > ((nGlobalFibTasks * 7 / 6 + mServiceThreads - 1) / mServiceThreads)))){
        /* set scheduler & scheddata to NULL */
        pushIntoGlobalReadyList(the_task);
    }
    else{
        /* set scheduler & scheddata */
        the_task->scheduler = (the_scheduler);
        the_task->scheddata = (&localscp);

        /* increase local tasks */
        FAA(getLocalFibTasksPtr(the_task));

        /* put next_task onto end of ready list */
        if (likely(the_task != the_scheduler)){
            _CHAIN_INSERT_BEFORE(the_scheduler, the_task);
        }
        else
        {
            _CHAIN_INSERT_TAIL(&local_readylist, the_task);
        }
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
    FibTCB * the_task = current_task;
    FibTCB * the_next = _CHAIN_FIRST(&local_readylist);
    if (likely(the_next != the_task)){
        _CHAIN_REMOVE(the_next);
    }
    else{
        /* cannot find a candidate, 
         * if and only if called from maintask and only maintask running,
         * go back to epoll 
         */
        return;
    }

    current_task = the_next;

    /* put the_next onto end of ready list */
    if (likely(the_next != the_next->scheduler)){
        _CHAIN_INSERT_BEFORE(the_next->scheduler, the_next);
    }
    else{
        _CHAIN_INSERT_TAIL(&local_readylist, the_next);        
    }

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
    _CHAIN_REMOVE(the_task);

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

    /* insert into local ready list */
    _CHAIN_INSERT_BEFORE(the_task->scheduler, the_task);
    return (0);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* yield/resume                                                        */
/////////////////////////////////////////////////////////////////////////
#define FIBER_TASK_EVENT_YIELD  (1ULL << 63)
#define FIBER_TASK_EVENT_RESUME (1ULL << 62)

FibTCB * fiber_yield(uint64_t code){
	FibTCB * the_task = current_task;
    the_task->yieldCode = code;
    fiber_setstate(the_task, STATES_SUSPENDED);
    return (the_task);
}

uint64_t fiber_resume(FibTCB * the_task){
	uint64_t yieldCode = the_task->yieldCode;
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
    the_task->state |= STATES_WAITFOR_EVENT;

    /* extract from ready list */
    _CHAIN_REMOVE(the_task);
    spin_unlock(&(the_task->eventlock));

    /* timeout set (?) */
    if (likely(timeout > 0)){
        the_task->delta_interval = timeout;
        fiber_watchdog_insert(the_task);
    }

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

        /* extract from watchdog list if needed */
        if (likely(the_task->state & STATES_WAIT_TIMEOUTB)){
        	fiber_watchdog_remove(the_task);
        }

        /* insert into current ready list (before scheduler) */
        _CHAIN_INSERT_BEFORE(the_maintask, the_task);
    }
    else{
    	spin_unlock(&(the_task->eventlock));    	
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
	spinlock * the_lock = &(the_task->scheddata->wadoglock);
	fibtcb_chain_t * the_chain = &(the_task->scheddata->wadoglist);
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
	spinlock * the_lock = &(the_task->scheddata->wadoglock);
	
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
static inline int fiber_watchdog_tickle(int gap){
    FibTCB * the_task, * the_nxt;

	spinlock * the_lock = &(localscp.wadoglock);
	fibtcb_chain_t * the_chain = &local_wadoglist;

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
            		_CHAIN_INSERT_BEFORE(the_task->scheduler, the_task);

            	}
            	spin_unlock(&(psem->qlock));
            }
            else if (the_task->state & STATES_WAITFOR_EVENT){
            	spin_lock(&(the_task->eventlock));
            	if (the_task->state & STATES_WAITFOR_EVENT){
            		/* clear WAITING STATES */
            		the_task->state &= (~STATES_WAITFOR_EVENT);

            	    /* always call from scheduler's thread */
            		_CHAIN_INSERT_BEFORE(the_task->scheduler, the_task);
            	}
            	spin_unlock(&(the_task->eventlock));
            }
            else if (the_task->state & STATES_IN_USLEEP){
                /* clear WAITING STATES */
            	the_task->state &= (~STATES_WAITFOR_EVENT);

            	/* always call from scheduler's thread */
            	_CHAIN_INSERT_BEFORE(the_task->scheduler, the_task);
            }
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
        /* clear the block state */
        fiber_clrstate(the_first, STATES_WAITFOR_MUTEX);
        return (true);
    }
    else{
    	/* decrease & increase */
    	FAS(getLocalFibTasksPtr(the_first));
    	FAA(getLocalFibTasksPtr(the_task ));

    	the_first->scheduler = the_task->scheduler;
    	the_first->scheddata = the_task->scheddata;
        
        /* clear the block state */
        fiber_clrstate(the_first, STATES_WAITFOR_MUTEX);

        // pushIntoBackupList(the_first);

    	/* push to global readylist */
    	// FAS(getLocalFibTasksPtr(the_first));
    	// pushIntoGlobalReadyList(the_first);

    	return true;
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
    the_task->state |= STATES_WAITFOR_SEMPH;

    /* set waiting object */
    the_task->waitingObject = (uint64_t)(psem);
    
    /* set return code */
    the_task->yieldCode = 0;

    spin_unlock(&(psem->qlock));

    /* put into watchdog waiting list */
    if (timeout > 0){
        the_task->delta_interval = timeout;
        fiber_watchdog_insert(the_task);
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

    /* remove the_first from watchdog list */
    if (the_first->state & STATES_WAIT_TIMEOUTB){
    	fiber_watchdog_remove(the_first);
    }

    if (the_first->scheduler == the_task->scheduler){
        /* clear the block state */
        _CHAIN_INSERT_BEFORE(the_task->scheduler, the_first);
        return (true);
    }
    else{
    	/* decrease number of tasks running on target threads */
    	FAS(getLocalFibTasksPtr(the_first));
    	FAA(getLocalFibTasksPtr(the_task ));

    	/* insert into local queue */
    	the_first->scheduler = the_task->scheduler;
    	the_first->scheddata = the_task->scheddata;

        _CHAIN_INSERT_BEFORE(the_task->scheduler, the_first);
        
        // pushIntoBackupList(the_first);

    	/* push to global ready list */
    	// FAS(getLocalFibTasksPtr(the_first));
    	// pushIntoGlobalReadyList(the_first);
    	return true;
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
    the_task->state |= STATES_WAITFOR_CONDV;

    /* set waiting object */
    the_task->waitingObject = (uint64_t)(pcond);
    
    /* set return code */
    the_task->yieldCode = 0;

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

    /* remove the_first from watchdog list */
    if (the_first->state & STATES_WAIT_TIMEOUTB){
    	fiber_watchdog_remove(the_first);
    }

    if (the_first->scheduler == the_task->scheduler){
        /* clear the block state */
        _CHAIN_INSERT_BEFORE(the_task->scheduler, the_first);
        return (true);
    }
    else{
    	/* decrease number of tasks running on target threads */
    	FAS(getLocalFibTasksPtr(the_first));
    	FAA(getLocalFibTasksPtr(the_task ));

    	/* insert into local queue */
    	the_first->scheduler = the_task->scheduler;
    	the_first->scheddata = the_task->scheddata;

        _CHAIN_INSERT_BEFORE(the_task->scheduler, the_first);
        
        // pushIntoBackupList(the_first);

    	/* push to global ready list */
    	// FAS(getLocalFibTasksPtr(the_first));
    	// pushIntoGlobalReadyList(the_first);

    	return true;
    }
}

bool fiber_cond_broadcast(FibCondition * pcond){
    FibTCB * the_task = current_task;

    /* collect all waiting tasks */
    fibtcb_chain_t localchain;
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

        /* remove the_first from watchdog list */
        if (the_tcb->state & STATES_WAIT_TIMEOUTB){
    	    /* lock on watchdog */
        	fiber_watchdog_remove(the_tcb);
        }

        if (the_tcb->scheduler == the_task->scheduler){
            /* clear the block state */
            _CHAIN_INSERT_BEFORE(the_task->scheduler, the_tcb);
        }
        else{
    	    /* decrease number of tasks running on target threads */
        	FAS(getLocalFibTasksPtr(the_tcb ));
        	FAA(getLocalFibTasksPtr(the_task));

    	    /* insert into local queue */
        	the_tcb->scheduler = the_task->scheduler;
        	the_tcb->scheddata = the_task->scheddata;

        	_CHAIN_INSERT_BEFORE(the_task->scheduler, the_tcb);

        	/* decreasing number of tasks running in target thread */
        	// FAS(getLocalFibTasksPtr(the_tcb));
        	// pushIntoGlobalReadyList(the_tcb);

        	/* second readyq on target thread */
        	// pushIntoBackupList(the_tcb);
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

    /* get current time (for timeout) */
    uint64_t prev_stmp = _utime();
    while (true){
        /* fire watchdogs */
    	uint64_t curr_stmp = _utime();
    	uint64_t curr_gapp = curr_stmp - prev_stmp;
    	if (likely(curr_gapp)){ 
    		fiber_watchdog_tickle(curr_gapp); 
    		prev_stmp = curr_stmp;
    	}

        /* workload balance between service threads */
        if (unlikely(localscp.nLocalFibTasks < ((nGlobalFibTasks * 16 / 15 + mServiceThreads - 1) / mServiceThreads))){
            /* get a task from global if too few tasks running locally */
            FibTCB * next_task = popFromGlobalReadyList(localscp.group);
            localscp.group = (localscp.group + 1) & (MAX_GLOBAL_GROUPS - 1);

            if (unlikely(next_task != NULL)){
                /* insert into local ready list */
            	_CHAIN_INSERT_BEFORE(the_scheduler, next_task);

                /* one more task in local system */
            	FAA(getLocalFibTasksPtr(next_task));

                // printf("thread %lu pull task %p in.\n", ((uint64_t)pthread_self()), next_task);
            }
        }

        /* exhaust all ready fibers */
        while (_CHAIN_FIRST(&local_readylist) != _CHAIN_LAST(&local_readylist)) 
        {
            fiber_sched_yield();
        }

        /* nothing to run, sleep for a while if come to here */
        __usleep__(10);
    }
}

void * pthread_scheduler(void * args){
    /* initialize thread execution environment */
    FiberThreadStartup();

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

/* send message to another task (internally using only) */
int fiber_send_message_internal(FibTCB * the_task, uint32_t type, uint32_t msgc, void * user, uint64_t valu){
    return 0;
};

/* user send message to another task */
int fiber_send_message(FibTCB * the_task, uint32_t msgc, void * user, uint64_t valu){
    return 0;
};
/////////////////////////////////////////////////////////////////////////