#include <sys/epoll.h>
#include <stdio.h>

#include "sysdef.h"
#include "chain.h"
#include "task.h"
#include "timestamp.h"

static inline bool fibtask_unregister_all_events(FibTCB * the_tcb);
static inline bool fibtask_reregister_all_events(FibTCB * the_tcb);

/* epoll specific data */
static __thread_local int epoll_fd = 0;
static __thread_local struct epoll_event epoll_events[MAX_EPOLL_EVENTS_PER_THREAD];

/* maintask should be the only task can call thread level blocking functions (epoll)
 */ 
static void * epoll_maintask(void * args){
    /* user initialization function */
    fibthread_args_t * pargs = (fibthread_args_t *)args;
    if (!pargs->init_func(pargs->args)){
        // pthread_exit(-1);
        return ((void *)(0));
    }

    /* increase number of service threads in system */
    // FAA(&mServiceThreads);

    uint64_t prev_stmp = _utime();
    /* running thread level epoll & scheduling */
    while (true){
        /* call epoll */
        int rc = epoll_wait(epoll_fd, epoll_events, MAX_EPOLL_EVENTS_PER_THREAD, 10);
        if (unlikely(rc < 0)){
            /* fatal error */
            continue;
        }

        /* merge events */
        for (int i = 0; i < rc; ++i){
            EventContext * ctx = (EventContext *)(epoll_events[i].data.ptr);
            EventContextControlBlock * pcb = (EventContextControlBlock *)fibtask_get_localdata(ctx->tcb, 0);

            ctx->events_o = epoll_events[i].events;
            pcb->tmpEventMasks |= (1 << (ctx->index));
            printf("Event: fd = %d, tcb = %p\n", ctx->fd, ctx->tcb);
        }

        /* post events */
        for (int i = 0; i < rc; ++i){
            EventContext * ctx = (EventContext *)(epoll_events[i].data.ptr);
            EventContextControlBlock * pcb = (EventContextControlBlock *)fibtask_get_localdata(ctx->tcb, 0);

            if (pcb->tmpEventMasks == 0ULL){
                continue;
            }

            fibtask_post(ctx->tcb, pcb->tmpEventMasks);
            pcb->tmpEventMasks = 0ULL;
        }

        /* fire watchdogs */
        uint64_t curr_stmp = _utime();
        uint64_t curr_gapp = curr_stmp - prev_stmp;
        fibtask_watchdog_tickle(curr_gapp);
        prev_stmp = curr_stmp;

        /* yield control to other tasks in thread */
        fibtask_sched_yield();
    }
    
    /* decrease number of service threads in system */
    // FAA(&mServiceThreads);

    // pthread_exit(0);
    return ((void *)(0));
}

bool epoll_install_callbacks(FibTCB * the_task){
    fibtask_install_callbacks(
        the_task,
        fibtask_unregister_all_events,
        fibtask_reregister_all_events,
        NULL,
        NULL
        );
    return true;    
}

void * epoll_thread(void * args){
    /* initialize thread environment */
    FibTaskThreadStartup();

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    /* create maintask (reuse thread's stack) */
    struct {} C;
    FibTCB * the_task = fibtask_create(epoll_maintask, args, (void *)(&C), 0UL);
    fibtask_set_thread_maintask(the_task);

    /* set current task to maintask & switch to it */
    goto_contxt2(&(the_task->regs));

    /* never return here */
    return ((void *)(0));
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
/* EPOLL BINDING                                                       */
/////////////////////////////////////////////////////////////////////////
int fibtask_register_events(int fd, int events){
    FibTCB * the_task = fibtask_ident();

    EventContextControlBlock * pcb = (EventContextControlBlock *)fibtask_get_localdata(the_task, 0);
    int index = __ffs64(pcb->usedEventMask);
    if (unlikely(index == 0)){return -1;};
    
    /* decrease index -> 0 based */
    --index;

    EventContext * pctx = &(pcb->ctxs[index]);

    /* fill the EventContext */
    pctx->fd       = fd; 
    pctx->events_i = events;
    pctx->index    = index;
    pctx->tcb      = the_task;

    /* remove unused mask bit */
    pcb->usedEventMask &= (~(1ULL << (index)));

    struct epoll_event event;
    event.data.ptr = (void *)pctx;
    event.events   = events;

    if (unlikely(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)){
        pcb->usedEventMask |= (1ULL << index);
        return (-1);
    }
    return (index);
}

static inline int fibtask_polling_events(
    FibTCB * the_task, 
    uint64_t mask,
    struct epoll_event * events
    ){
    int n = 0;
    EventContextControlBlock * pcb = (EventContextControlBlock *)fibtask_get_localdata(the_task, 0);
    EventContext * ctxs = pcb->ctxs;

    #define callback_setfd(p) do {                                      \
        events[n].events  = ctxs[p].events_o;                           \
        events[n].data.fd = ctxs[p].fd;                                 \
        ++n;                                                            \
    } while(0)

    callback_on_setbit(mask, callback_setfd);
    return n;
}

static inline bool fibtask_unregister_all_events(FibTCB * the_tcb){
    EventContextControlBlock * pcb = (EventContextControlBlock *)fibtask_get_localdata(the_tcb, 0);
    uint64_t mask = (~(pcb->usedEventMask));
    #define callback_unreg(p) do {                                      \
        epoll_ctl(                                                      \
            epoll_fd, EPOLL_CTL_DEL,                                    \
            pcb->ctxs[p].fd, NULL                                       \
            );                                                          \
    } while(0)

    callback_on_setbit(mask, callback_unreg);
    return (0);
}

static inline bool fibtask_reregister_all_events(FibTCB * the_tcb){
    EventContextControlBlock * pcb = (EventContextControlBlock *)fibtask_get_localdata(the_tcb, 0);
    uint64_t mask = (~(pcb->usedEventMask));
    #define callback_rereg(p) do {                                      \
        struct epoll_event event;                                       \
        event.data.ptr = (void *)(&(pcb->ctxs[p]));                     \
        event.events   = pcb->ctxs[p].events_i;                         \
        epoll_ctl(                                                      \
            epoll_fd, EPOLL_CTL_ADD,                                    \
            pcb->ctxs[p].fd, &event                                     \
            );                                                          \
    } while(0)

    callback_on_setbit(mask, callback_rereg);
    return (0);
}

int fibtask_epoll_wait(
    struct epoll_event * events, 
    int maxEvents, 
    int timeout_in_ms
){
    FibTCB * the_task = fibtask_ident();
    uint64_t mask = fibtask_wait(~0ULL, TASK_EVENT_WAIT_ANY, timeout_in_ms * 1000);
    return fibtask_polling_events(the_task, mask, events);
}
/////////////////////////////////////////////////////////////////////////