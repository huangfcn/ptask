#include <sys/mman.h>
#include <sys/epoll.h>

#include <assert.h>
#include <stdio.h>

#include "sysdef.h"
#include "spinlock.h"

#include "chain.h"
#include "timestamp.h"
#include "task.h"

//////////////////////////////////
/* assembly code in context.S   */
//////////////////////////////////
void swap_context(void *, void *);
void save_context(void *        );
void goto_context(void *        );
void asm_taskmain(              );
//////////////////////////////////

static inline int fibtask_unregister_all_events(FibTCB * the_tcb);
static inline int fibtask_reregister_all_events(FibTCB * the_tcb);
static inline int fibtask_free_all_eventcontext(FibTCB * the_tcb);


static inline int fibtask_watchdog_insert(FibTCB * the_tcb);
static inline int fibtask_watchdog_remove(FibTCB * the_tcb);
static inline int fibtask_watchdog_tickle(int gap);

static inline int fibtask_wait(int timeout);
static inline int fibtask_post(FibTCB * the_task);
static inline void fibtask_sched();

typedef struct freelist_t {
    struct freelist_t * next;
} freelist_t;

typedef CHAIN_HEAD(fibtcb_chain, FibTCB) fibtcb_chain_t;

/* thread cached lists */
static __thread_local freelist_t *   local_eventlist = NULL;
static __thread_local freelist_t *   local_freedlist = NULL;
static __thread_local fibtcb_chain_t local_blocklist;
static __thread_local fibtcb_chain_t local_readylist;

static __thread_local fibtcb_chain_t local_wadoglist;

static __thread_local int local_eventlist_size = 0;
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
static volatile freelist_t *   global_eventlist;
static volatile freelist_t *   global_freedlist;
static volatile fibtcb_chain_t global_readylist;

static spinlock spinlock_freedlist = {0};
static spinlock spinlock_readylist = {0};
static spinlock spinlock_eventlist = {0};

static volatile int global_readylist_size = 0;

static volatile struct {
    void *   stackbase;
    uint32_t stacksize;
} global_cached_stack[64];
static volatile uint64_t global_cached_stack_mask;
static spinlock spinlock_cached_stack;

/* epoll specific data */
static __thread_local int epoll_fd = 0;
static __thread_local struct epoll_event epoll_events[MAX_EPOLL_EVENTS_PER_THREAD];

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
bool FibTaskGlobalStartup(){
    global_eventlist = NULL;
    global_freedlist = NULL;
    _CHAIN_INIT_EMPTY(&global_readylist);

    spin_init(&spinlock_freedlist);
    spin_init(&spinlock_readylist);
    spin_init(&spinlock_eventlist);

    global_readylist_size = 0;
    mServiceThreads  = 0;
    nFibTaskInSystem = 0;

    global_cached_stack_mask = ~0ULL;
    spin_init(&spinlock_cached_stack);

    return true;
};

bool FibTaskThreadStartup(){
    local_eventlist = NULL;
    local_freedlist = NULL;

    _CHAIN_INIT_EMPTY(&local_blocklist);
    _CHAIN_INIT_EMPTY(&local_readylist);
    
   /* watch dog list */
    CHAIN_INIT_EMPTY(&local_wadoglist, FibTCB, link);

    local_eventlist_size = 0;
    local_freedlist_size = 0;
    local_readylist_size = 0;

    local_cached_stack_mask = 0xFF;

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
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
/* EVENT CONTEXT alloc/free                                            */
/////////////////////////////////////////////////////////////////////////
static inline void evtctx_free(EventContext * ctx){
    freelist_t * node = (freelist_t *)ctx;
    if (likely(local_eventlist_size < MAX_LOCAL_FREED_TASKS)){
        node->next = local_eventlist;
        local_eventlist = node;
        ++local_eventlist_size;
    }
    else{
        spin_lock(&spinlock_eventlist);
        node->next = (freelist_t *)global_eventlist;
        global_eventlist = node;
        spin_unlock(&spinlock_eventlist);
    }
};

static inline EventContext * evtctx_alloc(){
    if (likely(local_eventlist != NULL)){
        EventContext * ctx = (EventContext *)local_eventlist;
        local_eventlist = local_eventlist->next;

        --local_eventlist_size;
        return ctx;
    }
    else{
        EventContext * ctx = NULL;
        spin_lock(&spinlock_eventlist);
        ctx = (EventContext *)global_eventlist;
        if (likely(ctx)){global_eventlist = global_eventlist->next;}
        spin_unlock(&spinlock_eventlist);
        if (likely(ctx)){return ctx;}
    }

    /* malloc/mmap */
    {
        EventContext * ctxs = (EventContext *)malloc(sizeof(EventContext) * EVT_INCREASE_SIZE_AT_EMPTY);
        if (ctxs == NULL){
            /* logging, memory shortage */
            return NULL;
        }
        for (int i = 1; i < EVT_INCREASE_SIZE_AT_EMPTY; ++i){
            evtctx_free(ctxs + i);
        }
        return (ctxs);
    }
};
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* STACK CACHE                                                         */
/////////////////////////////////////////////////////////////////////////
static inline bool fibtask_stackcache_put(
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

static inline uint8_t * fibtask_stackcache_get(uint32_t * stacksize){
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
    /* increase number of fibtasks in system */
    FAA(&nFibTaskInSystem);

    /* call user entry */
    the_tcb->entry(the_tcb->args);

    /* decrease number of fibtasks in system */
    FAS(&nFibTaskInSystem);

    /* remove current task from ready list*/
    _CHAIN_REMOVE(the_tcb);
    --local_readylist_size;

    // int local_ready = local_readylist_size;
    // printf("%d tasks ready now.\n", local_ready);

    /* delete all polling events */
    fibtask_unregister_all_events(the_tcb);

    /* free EventContext */
    fibtask_free_all_eventcontext(the_tcb);
    
    /* free the_tcb */
    tcb_free(the_tcb);

    /* free stack */
    if (the_tcb->stacksize & MASK_SYSTEM_STACK){
        /* cannot free at here, put into cache for next thread */
        fibtask_stackcache_put(
            the_tcb->stackaddr,
            the_tcb->stacksize & (~15UL)
            );
    }

    /* switch to thread maintask */
    current_task = the_maintask;
    goto_context(&(the_maintask->regs));

    /* never reach here */
    return ((void *)(0));
}

/* maintask should be the only task can call thread level blocking functions.
 */ 
static void * maintask(void * args){
    /* user initialization function */
    fibthread_args_t * pargs = (fibthread_args_t *)args;
    if (!pargs->init_func(pargs->args)){
        // pthread_exit(-1);
        return ((void *)(0));
    }

    /* increase number of service threads in system */
    FAA(&mServiceThreads);

    uint64_t prev_stmp = _utime();
    /* running thread level epoll & scheduling */
    while (true){
        /* call epoll */
        int rc = epoll_wait(epoll_fd, epoll_events, MAX_EPOLL_EVENTS_PER_THREAD, 10);
        if (unlikely(rc < 0)){
            /* fatal error */
            continue;
        }

        /* collecting activated tasks */
        fibtcb_chain_t local_postlist;
        _CHAIN_INIT_EMPTY(&local_postlist);

        for (int i = 0; i < rc; ++i){
            EventContext * ctx = (EventContext *)(epoll_events[i].data.ptr);
            FibTCB * the_tcb = ctx->tcb;

            the_tcb->pendingEvents |= (1 << (ctx->index));
            the_tcb->events[ctx->index]->events_o = epoll_events[i].events;

            /* extract from blocklist */
            _CHAIN_REMOVE(the_tcb);

            /* append to local post list */
            _CHAIN_INSERT_TAIL(&local_postlist, the_tcb);

            printf("Event: fd = %d, tcb = %p\n", ctx->fd, ctx->tcb);
        }

        /* post/activate tasks */
        {
            FibTCB * the_tcb, * next_tcb;
            _CHAIN_FOREACH_SAFE(the_tcb, &local_postlist, FibTCB, next_tcb){
                fibtask_post(the_tcb);
            }
        }

        /* fire watchdogs */
        uint64_t curr_stmp = _utime();
        uint64_t curr_gapp = curr_stmp - prev_stmp;
        fibtask_watchdog_tickle(curr_gapp);
        prev_stmp = curr_stmp;

        /* yield control to other tasks in thread */
        fibtask_sched();
    }
    
    /* decrease number of service threads in system */
    FAA(&mServiceThreads);

    // pthread_exit(0);
    return ((void *)(0));
}

void * thread_maintask(void * args){
    /* initialize thread environment */
    FibTaskThreadStartup();

    /* create maintask (reuse thread's stack) */
    struct {} C;
    the_maintask = fibtask_create(maintask, args, (void *)(&C), 0UL);

    /* set current task to maintask & switch to it */
    current_task = the_maintask;
    goto_context(&(the_maintask->regs));

    /* never return here */
    return ((void *)(0));
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* create a fibtask & put onto ready list                              */
/////////////////////////////////////////////////////////////////////////
FibTCB * fibtask_create(
    void *(*func)(void *), 
    void * args, 
    void * stackaddr, 
    uint32_t stacksize
){
    FibTCB * the_task = tcb_alloc();
    if (the_task == NULL){return NULL;}

    /* initialize to (0) */
    the_task->state = STATES_READY;
    the_task->usedEventMask = ~0ULL;
    the_task->pendingEvents =  0ULL;

    uint8_t * stackbase = (uint8_t *)stackaddr;
    if (stackbase == NULL){
        stacksize = ((stacksize + 4095) / 4096) * 4096;
        stackbase = fibtask_stackcache_get(&stacksize);
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

    /* r12 (tcb), r15 (cpp_taskmain), rip (asm_taskmain), rsp */
    the_task->regs.reg_r12 = (uint64_t)(the_task);
    the_task->regs.reg_r15 = (uint64_t)(cpp_taskmain);
    the_task->regs.reg_rip = (uint64_t)(asm_taskmain);
    the_task->regs.reg_rsp = (uint64_t)(stackbase + (stacksize & (~15UL)));

   /* put next_task onto end of ready list */
    _CHAIN_INSERT_TAIL(&local_readylist, the_task);
    ++local_readylist_size;

    return the_task;
}
/////////////////////////////////////////////////////////////////////////

FibTCB * fibtask_ident(){
    return current_task;
}

/////////////////////////////////////////////////////////////////////////
/* schedule to next task                                               */
/////////////////////////////////////////////////////////////////////////
static inline void fibtask_sched(){
    /* find next ready task */
    FibTCB * next_task = NULL, * the_task = current_task;
    while (true) {
        /* only get a task from local if possible */
        if((local_readylist_size > 1) || (current_task != the_maintask))
        { 
            _CHAIN_FOREACH(next_task, &local_readylist, FibTCB){
                if (next_task != current_task){
                    break;
                }
            }

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
            fibtask_reregister_all_events(next_task);
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
/* yield/resume                                                        */
/////////////////////////////////////////////////////////////////////////
FibTCB * fibtask_yield(uint64_t code){
    FibTCB * the_task = current_task;
    
    /* set state */
    the_task->state |= STATES_SUSPENDED;
    the_task->yieldCode = code;

    /* extract from ready list */
    _CHAIN_REMOVE(the_task);
    if (likely(the_task->state == STATES_SUSPENDED)){ --local_readylist_size; }

    /* insert into blocked list */
    _CHAIN_INSERT_TAIL(&local_blocklist, the_task);

    /* fibtask_sched to next task */
    fibtask_sched();

    return (the_task);
}

uint64_t fibtask_resume(FibTCB * the_task){
    int yieldCode = the_task->yieldCode;

    /* already in ready state */
    if (unlikely((the_task->state & STATES_SUSPENDED) == 0)){
        return (yieldCode);
    }

    /* remove SUSPEND mask */
    the_task->state &= (~STATES_SUSPENDED);

    /* still blocked (?) */
    if (unlikely(the_task->state)){
        return (yieldCode);
    }

    /* extract from blocked list */
    _CHAIN_REMOVE(the_task);

    /* put into global ready list if too much tasks in local list */
    if (unlikely((the_task != the_maintask) && (local_readylist_size >= MAX_LOCAL_READY_TASKS))){
        /* delete all polling events */
        fibtask_unregister_all_events(the_task);

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
    return (yieldCode);
}

FibTCB * fibtask_sched_yield(){
    FibTCB * the_task = current_task;

    /* move to end of ready list */
    _CHAIN_REMOVE(the_task);
    _CHAIN_INSERT_TAIL(&local_readylist, the_task);

    /* fibtask_sched to next task */
    fibtask_sched();

    return (the_task);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* wait/post events (only used internally)                             */
/////////////////////////////////////////////////////////////////////////
static inline int fibtask_wait(int timeout){
    FibTCB * the_task = current_task;
    
    /* set state */
    the_task->state |= STATES_WAITFOR_EVENT;

    /* extract from ready list */
    _CHAIN_REMOVE(the_task);
    if (likely(the_task->state == STATES_WAITFOR_EVENT)){ --local_readylist_size; }

    /* insert into blocked list */
    _CHAIN_INSERT_TAIL(&local_blocklist, the_task);

    /* insert into watchdog list if specified */
    if (timeout > 0){
    	the_task->state |= STATES_WAITFOR_TIMER;
    	the_task->delta_interval = timeout;
    	fibtask_watchdog_insert(the_task);
    }

    /* fibtask_sched to next task */
    fibtask_sched();

    return (0);
}

/* post event (only used internally) */
static inline int fibtask_post(FibTCB * the_task){
    /* already in ready state */
    if (unlikely((the_task->state & (STATES_WAITFOR_EVENT | STATES_WAITFOR_TIMER)) == 0)){
        return (0);
    }

    /* remove WAITFOR EVENTS mask */
    the_task->state &= (~STATES_WAITFOR_EVENT);

    /* check if on watchdog chain (?) */
    if (the_task->state & STATES_WAITFOR_TIMER){
    	fibtask_watchdog_remove(the_task);
    }
    the_task->state &= (~STATES_WAITFOR_TIMER);

    /* still blocked (?) */
    if (unlikely(the_task->state)){
        return (1);
    }

    /* extract from blocked list */
    _CHAIN_REMOVE(the_task);

    /* put into global ready list if too much tasks in local list */
    if (unlikely((the_task != the_maintask) && (local_readylist_size >= MAX_LOCAL_READY_TASKS))){
        /* delete all polling events */
        fibtask_unregister_all_events(the_task);

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

void fibtask_usleep(int usec){
    fibtask_wait(usec);
}
/////////////////////////////////////////////////////////////////////////
/* watchdog or timeout support                                         */
/////////////////////////////////////////////////////////////////////////
static inline int fibtask_watchdog_insert(FibTCB * the_tcb){
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

static inline int fibtask_watchdog_remove(FibTCB * the_tcb){
	FibTCB * nxt_tcb = CHAIN_NEXT(the_tcb, link);
	if (CHAIN_NEXT(nxt_tcb, link)){
		nxt_tcb->delta_interval += the_tcb->delta_interval;
	}
	CHAIN_REMOVE(the_tcb, FibTCB, link);

	return (0);
}

static inline int fibtask_watchdog_tickle(int gap){
	FibTCB * the_tcb, * the_nxt;
	CHAIN_FOREACH_SAFE(the_tcb, &local_wadoglist, link, the_nxt){
		if (the_tcb->delta_interval <= gap){
			/* activate the tcb */
			fibtask_post(the_tcb);
		}
		else{
			the_tcb->delta_interval -= gap;
			break;
		}
	}

	return (0);
}
/////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////
/* EPOLL BINDING                                                       */
/////////////////////////////////////////////////////////////////////////
int fibtask_register_events(int fd, int events){
    FibTCB * the_task = current_task;

    int index = __ffs64(the_task->usedEventMask);
    if (unlikely(index == 0)){return -1;};

    EventContext * pctx = evtctx_alloc();
    if (unlikely(pctx == NULL)){return -1;};

    /* decrease index -> 0 based */
    --index;

    /* fill the EventContext */
    pctx->fd       = fd; 
    pctx->events_i = events;

    pctx->index    = index;

    pctx->tcb      = the_task;

    /* remove unused mask bit */
    the_task->usedEventMask &= (~(1ULL << (index)));
    the_task->events[index] = (void *)(pctx);

    struct epoll_event event;
    event.data.ptr = (void *)pctx;
    event.events   = events;

    if (unlikely(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)){
        the_task->usedEventMask |= (1ULL << index);
        evtctx_free(pctx);

        return (-1);
    }
    return (index);
}

static inline int fibtask_polling_events(
    FibTCB * the_task, 
    struct epoll_event * events
    ){
    uint64_t mask = the_task->pendingEvents;
    int      n    = 0;

    #define callback_setfd(p) do {                                      \
        events[n].events  = the_task->events[p]->events_o;              \
        events[n].data.fd = the_task->events[p]->fd;                    \
        ++n;                                                            \
    } while(0)

    callback_on_setbit(mask, callback_setfd);
    the_task->pendingEvents = 0ULL;
    return n;
}

static inline int fibtask_unregister_all_events(FibTCB * the_tcb){
    uint64_t mask = (~(the_tcb->usedEventMask));

    #define callback_unreg(p) do {                                      \
        epoll_ctl(                                                      \
            epoll_fd, EPOLL_CTL_DEL,                                    \
            the_tcb->events[p]->fd, NULL                                \
            );                                                          \
    } while(0)

    callback_on_setbit(mask, callback_unreg);
    return (0);
}

static inline int fibtask_reregister_all_events(FibTCB * the_tcb){
    uint64_t mask = (~(the_tcb->usedEventMask));

    #define callback_rereg(p) do {                                      \
        struct epoll_event event;                                       \
        event.data.ptr = (void *)(the_tcb->events[p]);                  \
        event.events   = the_tcb->events[p]->events_i;                  \
        epoll_ctl(                                                      \
            epoll_fd, EPOLL_CTL_ADD,                                    \
            the_tcb->events[p]->fd, &event                              \
            );                                                          \
    } while(0)

    callback_on_setbit(mask, callback_rereg);
    return (0);
}

static inline int fibtask_free_all_eventcontext(FibTCB * the_tcb){
    uint64_t mask = (~(the_tcb->usedEventMask));

    #define callback_freectx(p) do {                                    \
        evtctx_free(the_tcb->events[p]);                                \
    } while(0)

    callback_on_setbit(mask, callback_freectx);
    return (0);
}

int fibtask_epoll_wait(
    struct epoll_event * events, 
    int maxEvents, 
    int timeout_in_ms
){
    FibTCB * the_task = current_task;
    if (likely(the_task->pendingEvents == 0)){ fibtask_wait(timeout_in_ms * 1000); }
    return fibtask_polling_events(the_task, events);
}
/////////////////////////////////////////////////////////////////////////