#include <sys/epoll.h>
#include <stdio.h>

#include "libfiber.h"
#include "epoll.h"

#include "timestamp.h"

/* epoll specific data */
static int epoll_fd = 0;
static struct epoll_event epoll_events[MAX_EPOLL_EVENTS_PER_THREAD];

/* maintask should be the only task can call thread level blocking functions (epoll)
 */ 
static int64_t fiber_epoll(int fd, volatile bool * bQuit){
    while (!bQuit[0]){
        /* call epoll */
        int rc = epoll_wait(fd, epoll_events, MAX_EPOLL_EVENTS_PER_THREAD, 10);
        if (unlikely(rc < 0)){
            /* fatal error */
            continue;
        }

        /* merge events */
        for (int i = 0; i < rc; ++i){
            EventContext * ctx = (EventContext *)(epoll_events[i].data.ptr);
            EventContextControlBlock * pcb = (EventContextControlBlock *)fiber_get_localdata(ctx->tcb, 0);

            ctx->events_o = epoll_events[i].events;
            pcb->tmpEventMasks |= (1 << (ctx->index));
            printf("Event: fd = %d, tcb = %p\n", ctx->fd, ctx->tcb);
        }

        /* post events */
        for (int i = 0; i < rc; ++i){
            EventContext * ctx = (EventContext *)(epoll_events[i].data.ptr);
            EventContextControlBlock * pcb = (EventContextControlBlock *)fiber_get_localdata(ctx->tcb, 0);

            if (pcb->tmpEventMasks == 0ULL){
                continue;
            }

            fiber_send_message_internal(
                ctx->tcb, 
                MSG_TYPE_SCHEDULER,
                MSG_CODE_POSTEVENT,
                NULL,
                pcb->tmpEventMasks
                );

            pcb->tmpEventMasks = 0ULL;
        }
    }

    return (0LL);
}

void * pthread_epoll(void * args){
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    return (void *)fiber_epoll(epoll_fd, (bool *)args);
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
int fiber_epoll_register_events(int fd, int events){
    FibTCB * the_task = fiber_ident();

    EventContextControlBlock * pcb = (EventContextControlBlock *)fiber_get_localdata(the_task, 0);
    int index = __ffs64(pcb->usedEventMask);
    if (unlikely((index == 0) || (index > pcb->maxEvents))){return -1;};
    
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

static inline int fiber_epoll_polling_events(
    FibTCB * the_task, 
    uint64_t mask,
    struct epoll_event * events
    ){
    int n = 0;
    EventContextControlBlock * pcb = (EventContextControlBlock *)fiber_get_localdata(the_task, 0);
    EventContext * ctxs = pcb->ctxs;

    #define callback_setfd(p) do {                                      \
        events[n].events  = ctxs[p].events_o;                           \
        events[n].data.fd = ctxs[p].fd;                                 \
        ++n;                                                            \
    } while(0)

    callback_on_setbit(mask, callback_setfd);
    return n;
}

int fiber_epoll_unregister_event(FibTCB * the_tcb, int p){
    EventContextControlBlock * pcb = (EventContextControlBlock *)fiber_get_localdata(the_tcb, 0);
    epoll_ctl(                  
        epoll_fd, EPOLL_CTL_DEL,
        pcb->ctxs[p].fd, NULL   
        );     
    pcb->usedEventMask |= (1ULL << p);                 

    return (0);
}

int fiber_epoll_wait(
    struct epoll_event * events, 
    int maxEvents, 
    int timeout_in_ms
){
    FibTCB * the_task = fiber_ident();
    uint64_t mask = fiber_event_wait(~0ULL, TASK_EVENT_WAIT_ANY, timeout_in_ms * 1000);
    return fiber_epoll_polling_events(the_task, mask, events);
}
/////////////////////////////////////////////////////////////////////////