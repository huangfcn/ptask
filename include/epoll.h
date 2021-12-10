#ifndef __LIBFIB_EPOLL_H__
#define __LIBFIB_EPOLL_H__

#ifdef __cplusplus
extern "C" {
#endif

struct EventContext;
typedef struct EventContext EventContext;

struct EventContext{
    int   index;

    int   fd;
    int   events_i;
    int   events_o;

    struct FibTCB * tcb;
};

typedef struct EventContextControlBlock {
    uint64_t maxEvents;
    uint64_t usedEventMask;
    uint64_t tmpEventMasks;

    EventContext * ctxs;
} EventContextControlBlock;

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

///////////////////////////////////////////////////////////////////
typedef struct fibthread_args_s {
    bool (*init_func)(void *);
    void * args;
} fibthread_args_t;

/* thread entrypoint for those threads working as service 
 * thread for fibtasks. 
 * it will call init_func with args @ thread startup
 */
void * epoll_thread(void * args);
bool   epoll_install_callbacks(FibTCB * the_task);
///////////////////////////////////////////////////////////////////

#ifdef __cplusplus
};
#endif

#endif