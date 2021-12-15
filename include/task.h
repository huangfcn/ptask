#ifndef __LIBFIB_TASK_H__
#define __LIBFIB_TASK_H__

#include "sysdef.h"
#include "rbqb.h"
#include "chain.h"
#include "spinlock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LOCAL_READY_TASKS       (  64)
#define MAX_LOCAL_FREED_TASKS       (  64)
#define MAX_FDPOLLNG_PER_TASK       (  64)

#define MAX_EPOLL_EVENTS_PER_THREAD (8192)

#define TCB_INCREASE_SIZE_AT_EMPTY  (  64)
#define EVT_INCREASE_SIZE_AT_EMPTY  (  64)

#define MAX_TASK_LOCALDATAS         (   4)

/*
 *  The following constants define the individual states which may be
 *  be used to compose and manipulate a thread's state.
 */
#define STATES_READY           (0x00000) /* ready to run        */
#define STATES_DORMANT         (0x00001) /* created not started */
#define STATES_SUSPENDED       (0x00002) /* waiting for resume  */
#define STATES_IN_USLEEP       (0x00004) /* waiting for resume  */
#define STATES_WAIT_TIMEOUTB   (0x00008) /* wait for timeout    */
#define STATES_WAITFOR_EVENT   (0x00010) /* wait for event      */
#define STATES_WAITFOR_MUTEX   (0x00020) /* wait for event      */
#define STATES_WAITFOR_SEMA    (0x00040) /* wait for event      */

#define STATES_BLOCKED         (STATES_SUSPENDED     |    \
                                STATES_IN_USLEEP     |    \
                                STATES_WAIT_TIMEOUTB |    \
                                STATES_WAITFOR_EVENT |    \
                                STATES_WAITFOR_MUTEX |    \
                                STATES_WAITFOR_SEMA  )

/* user provied stack */
#define MASK_SYSTEM_STACK      (0x00001)
#define MASK_SUSPEND_AT_ENTRY  (0x00002)

#define TASK_EVENT_WAIT_ANY    (0x00001)

struct FibTCB;
typedef struct FibTCB FibTCB;

struct FibSCP;
typedef struct FibSCP FibSCP;

typedef struct FibCTX{
    uint64_t    reg_r12;    // 0
    uint64_t    reg_r13;    // 8
    uint64_t    reg_r14;    // 16
    uint64_t    reg_r15;    // 24

    uint64_t    reg_rip;    // 32
    uint64_t    reg_rsp;    // 40
    uint64_t    reg_rbx;    // 48
    uint64_t    reg_rbp;    // 56
} FibCTX;

typedef CHAIN_HEAD(fibtcb_chain, FibTCB) fibtcb_chain_t;

struct FibTCB{
    /* ready / suspend / free queue */
    CHAIN_ENTRY(FibTCB) node;  

    /* start point, arguments */
    void *    (*entry)(void *);
    void *    args;

    /* stack address & size */
    void *    stackaddr;
    uint32_t  stacksize;

    /* task state (READY/SUSPEND/SLEEP/WAIT) */
    uint32_t  state;

    /*  yieldCode / exitCode */
    uint64_t  yieldCode;

    /* timeout support */
    CHAIN_ENTRY(FibTCB) link;
    int       delta_interval;

    /* events waiting/post */
    int       waitingOptions;
    uint64_t  pendingEvents;
    uint64_t  waitingEvents;
    uint64_t  seizedEvents;

    /* thread message queue associated with this task */
    FibTCB *  scheduler;
    FibSCP *  scheddata;
    /* Task Local Storage */
    uint64_t  taskLocalStorages[MAX_TASK_LOCALDATAS];

    /* callback before & after switching a thread */
    bool      (*  preSwitchingThread)(FibTCB *);
    bool      (* postSwitchingThread)(FibTCB *);

    /* on task initialization & deinit */
    bool      (* onTaskStartup)(FibTCB *);
    bool      (* onTaskCleanup)(FibTCB *);

    /* CPU switching context */
    FibCTX regs;
};

struct freelist_t;
typedef struct freelist_t freelist_t;

struct schedmsgq_t;
typedef struct schedmsgq_t schedmsgq_t;

struct FibSCP {
    FibTCB      *  taskonrun;
    schedmsgq_t *  schedmsgq;
    freelist_t  *  freedlist;
    fibtcb_chain_t blocklist;
    fibtcb_chain_t readylist;

    fibtcb_chain_t wadoglist;

    int            freedlist_size;
    int            readylist_size;

    struct {
        void *   stackbase;
        uint32_t stacksize;
    } cached_stack[8];
    uint32_t cached_stack_mask;
};

typedef struct FiberMutex {
    volatile uint64_t holder;
    volatile uint32_t reentries;

    spinlock       qlock;
    fibtcb_chain_t waitq;
} FibMutex;

typedef struct FiberSemaphore {
    volatile int64_t count;

    spinlock       qlock;
    fibtcb_chain_t waitq;
} FibSemaphore;

typedef FibMutex fiber_mutex_t;
typedef FibSemaphore fiber_sem_t;

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
bool FiberThreadStartup();

/* create a task */
FibTCB * fiber_create(
    void *(*func)(void *), 
    void * args, 
    void * stackaddr, 
    uint32_t stacksize
    );

/* yield will yield control to other task
 * current task will suspend until resume called on it
 */
FibTCB * fiber_yield(uint64_t code);
uint64_t fiber_resume(FibTCB * the_task);

/* identify current running task */
FibTCB * fiber_ident();

/* task usleep (accurate at ms level)*/
void fiber_usleep(int usec);

/* same functionality of sched_yield, yield the processor
 * sched_yield causes the calling task to relinquish the CPU.
 * The task is moved to the end of the ready queue and 
 * a new task gets to run.
 */
FibTCB * fiber_sched_yield();

/* scheduler thread */
void * pthread_scheduler(void * args);
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
/* service thread maintask related functions                     */
///////////////////////////////////////////////////////////////////
/* feed watchdog @ thread service maintask */
int fiber_watchdog_tickle(int gap);

/* set thread maintask (thread scheduling task) */
bool fiber_set_thread_maintask(FibTCB * the_task);

/* wait for events (events bitmask in, wait for any/all, and timeout) 
   post event to a task
*/ 
uint64_t fiber_event_wait(uint64_t events_in, int options, int timeout);
int      fiber_event_post(FibTCB * the_task, uint64_t events_in);

/* mutex APIs */
int  fiber_mutex_init(FibMutex * pmutex);
bool fiber_mutex_lock(FibMutex * pmutex);
bool fiber_mutex_unlock(FibMutex * pmutex);
bool fiber_mutex_destroy(FibMutex * pmtx);

/* sempahore APIs */
int  fiber_sem_init(FibSemaphore * psem, int initval);
bool fiber_sem_wait(FibSemaphore * psem);
bool fiber_sem_post(FibSemaphore * psem);
bool fiber_sem_destroy(FibSemaphore * psem);
///////////////////////////////////////////////////////////////////

//////////////////////////////////
/* assembly code in context.S   */
//////////////////////////////////
void swap_context(void *, void *);
void goto_context(void *        );
void goto_contxt2(void *        );
void asm_taskmain(              );
//////////////////////////////////

typedef struct fibthread_args_s {
    bool (*init_func)(void *);
    void * args;
} fibthread_args_t;

///////////////////////////////////////////////////////////////////
/* message queue of each service thread                          */
///////////////////////////////////////////////////////////////////
#define MSG_TYPE_SCHEDULER       (0x00000001)
#define MSG_TYPE_USERPOSTD       (0xF0000000)

// scheduler
#define MSG_CODE_ACTIVATED       (0x00000001)
#define MSG_CODE_POSTEVENT       (0x00000002)

int fiber_send_message_internal(
    FibTCB * the_task, 
    uint32_t type, 
    uint32_t msg, 
    void   * user, 
    uint64_t valu
);

int fiber_send_message(
    FibTCB * the_task,
    uint32_t msg, 
    void   * user, 
    uint64_t valu
);
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
/* extensions                                                    */
///////////////////////////////////////////////////////////////////
static inline bool fiber_set_localdata(FibTCB * the_task, int index, uint64_t data){
    if (index >= MAX_TASK_LOCALDATAS){return false;};
    the_task->taskLocalStorages[index] = data;
    return true;
}

static inline uint64_t fiber_get_localdata(FibTCB * the_task, int index){
    if (index >= MAX_TASK_LOCALDATAS){return 0ULL;};

    return the_task->taskLocalStorages[index];
}

static inline bool fiber_install_callbacks(
    FibTCB * the_task,
    bool (*  preSwitchingThread)(FibTCB *),
    bool (* postSwitchingThread)(FibTCB *),
    bool (* onTaskStartup)(FibTCB *      ),
    bool (* onTaskCleanup)(FibTCB *      )    
    )
{
    the_task->preSwitchingThread  = preSwitchingThread;
    the_task->postSwitchingThread = postSwitchingThread;
    the_task->onTaskStartup       = onTaskStartup;
    the_task->onTaskCleanup       = onTaskCleanup;

    return true;
}
///////////////////////////////////////////////////////////////////

#ifdef __cplusplus
};
#endif

#endif
