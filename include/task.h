#ifndef __LIBFIB_TASK_H__
#define __LIBFIB_TASK_H__

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LOCAL_READY_TASKS       (  64)
#define MAX_LOCAL_FREED_TASKS       (  64)
#define MAX_FDPOLLNG_PER_TASK       (  64)

#define MAX_EPOLL_EVENTS_PER_THREAD (8192)

#define TCB_INCREASE_SIZE_AT_EMPTY  (  64)
#define EVT_INCREASE_SIZE_AT_EMPTY  (  64)

/*
 *  The following constants define the individual states which may be
 *  be used to compose and manipulate a thread's state.
 */
#define STATES_READY           (0x00000) /* ready to run        */
#define STATES_DORMANT         (0x00001) /* created not started */
#define STATES_SUSPENDED       (0x00002) /* waiting for resume  */
#define STATES_WAITFOR_TIMER   (0x00004) /* wait for timeout    */
#define STATES_WAITFOR_EVENT   (0x00008) /* wait for event      */

/* user provied stack */
#define MASK_SYSTEM_STACK      (0x00001)
#define MASK_SUSPEND_AT_ENTRY  (0x00002)

struct EventContext;
typedef struct EventContext EventContext;

struct EventContext{
    int   index;

    int   fd;
    int   events_i;
    int   events_o;

    struct FibTCB * tcb;
};

struct FibTCB;
typedef struct FibTCB FibTCB;

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
    int64_t   delta_interval;

    /* binding with epoll like polling method
     * 1. registered events (at most 64 per task) 
     * 2. 64-bits mask for registed events & pending events
     */
    EventContext * events[MAX_FDPOLLNG_PER_TASK];
    uint64_t       usedEventMask;
    uint64_t       pendingEvents;

    struct _regs_s{
        uint64_t    reg_r12;    // 0
        uint64_t    reg_r13;    // 8
        uint64_t    reg_r14;    // 16
        uint64_t    reg_r15;    // 24

        uint64_t    reg_rip;    // 32
        uint64_t    reg_rsp;    // 40
        uint64_t    reg_rbx;    // 48
        uint64_t    reg_rbp;    // 56
    } regs;
};

///////////////////////////////////////////////////////////////////
typedef struct fibthread_args_s {
    bool (*init_func)(void *);
    void * args;
} fibthread_args_t;

/* thread entrypoint for those threads working as service 
 * thread for fibtasks. 
 * it will call init_func with args @ thread startup
 */
void * thread_maintask(void * args);
///////////////////////////////////////////////////////////////////

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
bool FibTaskGlobalStartup();
bool FibTaskThreadStartup();

/* create a task */
FibTCB * fibtask_create(
    void *(*func)(void *), 
    void * args, 
    void * stackaddr, 
    uint32_t stacksize
    );

/* yield will yield control to other task
 * current task will suspend until resume called on it
 */
FibTCB * fibtask_yield(uint64_t code);
uint64_t fibtask_resume(FibTCB * the_task);

/* identify current running task */
FibTCB * fibtask_ident();

/* task usleep (accurate at ms level)*/
void fibtask_usleep(int usec);

/* same functionality of sched_yield, yield the processor
 * sched_yield causes the calling task to relinquish the CPU.
 * The task is moved to the end of the ready queue and 
 * a new task gets to run.
 */
FibTCB * fibtask_sched_yield();
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
/* epoll integeration                                            */
///////////////////////////////////////////////////////////////////
struct epoll_event;
int fibtask_register_events(int fd, int events);
int fibtask_epoll_wait(
    struct epoll_event * events, 
    int maxEvents, 
    int timeout_in_ms
    );
///////////////////////////////////////////////////////////////////

#ifdef __cplusplus
};
#endif

#endif
