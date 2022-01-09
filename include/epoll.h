#ifndef __LIBFIB_EPOLL_H__
#define __LIBFIB_EPOLL_H__

#define EVENT_BITMASK_EPOLL     (1ULL)
#define EPOLL_TASKDAT_INDEX     (0   )

#ifdef __cplusplus
extern "C" {
#endif
struct EventContext;
typedef struct EventContext EventContext;

struct EventContext{
    int   fd;
    int   events;

    struct FibTCB * tcb;
};

///////////////////////////////////////////////////////////////////
/* epoll integeration                                            */
///////////////////////////////////////////////////////////////////
struct epoll_event;
int fiber_epoll_register_events(int epoll_fd, int fd, int events);
int fiber_epoll_unregister_events(int epoll_fd, int fd);
// int fiber_epoll_wait(
//     struct epoll_event * events, 
//     int maxEvents, 
//     int timeout_in_ms
//     );
// int fiber_epoll_post(
//     int nEvents,
//     struct epoll_event * events
//     );

static inline int fiber_epoll_wait(
    struct epoll_event * events, 
    int maxEvents, 
    int timeout_in_ms
){
    // assert(maxEvents == 1);

    uint64_t mask = fiber_event_wait(EVENT_BITMASK_EPOLL, TASK_EVENT_WAIT_ANY, timeout_in_ms * 1000);

    if (mask & EVENT_BITMASK_EPOLL){
        FibTCB       * the_task = fiber_ident();
        EventContext * pctx     = (EventContext *)fiber_get_localdata(the_task, EPOLL_TASKDAT_INDEX);

        events[0].events  = pctx->events;
        events[0].data.fd = pctx->fd;

        return 1;
    }
    return 0;
}

static inline int fiber_epoll_post(
    int nEvents,
    struct epoll_event * events
)
{
    for (int i = 0; i < nEvents; ++i){
        EventContext * ctx = (EventContext *)(events[i].data.ptr);
        ctx->events = events[i].events;
        fiber_event_post(ctx->tcb, EVENT_BITMASK_EPOLL);
    }
    return (0);
}
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
/* thread entrypoint for those threads working as service 
 * thread for fibtasks. 
 * it will call init_func with args @ thread startup
 */
typedef struct epoll_param_t {
    int    epoll_fd;
    bool * bQuit;
} epoll_param_t;
void * pthread_epoll(void *);
///////////////////////////////////////////////////////////////////

#ifdef __cplusplus
};
#endif

#endif