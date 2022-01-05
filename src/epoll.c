#include <sys/epoll.h>
#include <stdio.h>

#include <assert.h>

#include "task.h"
#include "epoll.h"

/////////////////////////////////////////////////////////////////////////
/* EPOLL BINDING                                                       */
/////////////////////////////////////////////////////////////////////////
int fiber_epoll_register_events(int epoll_fd, int fd, int events){
    FibTCB * the_task = fiber_ident();
    EventContext * pctx = (EventContext *)malloc(sizeof(EventContext));
    if (pctx == NULL){
        return -1;
    }

    /* fill the EventContext */
    pctx->fd       = fd;
    pctx->tcb      = the_task;

    struct epoll_event event;
    event.data.ptr = (void *)pctx;
    event.events   = events;

    if (unlikely(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)){
        free(pctx);
        return (-1);
    }

    /* set local data */
    fiber_set_localdata(the_task, EPOLL_TASKDAT_INDEX, (uint64_t)pctx);
    return (0);
}

int fiber_epoll_unregister_events(int epoll_fd, int fd){
    /* clear local data */
    FibTCB * the_task = fiber_ident();
    EventContext * pctx = (EventContext *)fiber_get_localdata(the_task, EPOLL_TASKDAT_INDEX);
    if (pctx){
        free(pctx); 
    }
    fiber_set_localdata(the_task, EPOLL_TASKDAT_INDEX, 0ULL);

    /* delete from epoll */
    epoll_ctl(                  
        epoll_fd, EPOLL_CTL_DEL,
        fd, NULL   
        );
    return (0);
}
/////////////////////////////////////////////////////////////////////////