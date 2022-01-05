#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <sys/mman.h>
#include <sys/epoll.h>

#include <assert.h>

#include "task.h"
#include "epoll.h"

static int create_and_bind(int port)
{

    int portnum = port;
    int sfd;

    sfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portnum);

    bind(sfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    return sfd;
}

static int make_socket_non_blocking(int sfd)
{
    int flags, s;

    flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1){ return -1; }

    flags |= O_NONBLOCK;
    s = fcntl(sfd, F_SETFL, flags);
    if (s == -1) { return -1; }

    return 0;
}

#define MAXEVENTS  (4)

static const char reply[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-type: text/html\r\n"
    "Connection: close\r\n"
    "Content-Length: 82\r\n"
    "\r\n"
    "<html>\n"
    "<head>\n"
    "<title>performance test</title>\n"
    "</head>\n"
    "<body>\n"
    "Hello FibTask!\n"
    "</body>\n"
    "</html>"
;

ssize_t fiber_read(int fd, char * buf, int bufsize){
    /* read first */
    int rc = read(fd, buf, bufsize);
    if ( (rc > 0) || ((rc < 0) && (errno != EAGAIN)) ){
        return rc;
    }

    /* waiting for events */
    struct epoll_event event;
    fiber_epoll_wait(&event, 1, FIBER_TIMEOUT_INFINITE);
    if ((event.events & EPOLLERR) || (event.events & EPOLLHUP) || (!(event.events & EPOLLIN))){
        return (-1);
    }

    return read(fd, buf, bufsize);
}

typedef struct client_params_t {
    int epoll_fd;
    int fd;
} client_params_t;

void * requestHandler(void* args)
{
    client_params_t * params = (client_params_t *)args;

    int infd = params->fd;
    int epollfd = params->epoll_fd;

    int s = make_socket_non_blocking(infd);
    if (s == -1)
        abort();

    // EventContext ctxs[MAXEVENTS] = {0};
    // EventContextControlBlock ctxcb = {
    //     .maxEvents     = MAXEVENTS,
    //     .usedEventMask = (~0ULL),
    //     .epoll_fd      = epollfd,
    //     .ctxs          = ctxs
    // };
    // fiber_set_localdata(fiber_ident(), 0, (uint64_t)(&ctxcb));
    fiber_epoll_register_events(epollfd, infd, EPOLLIN | EPOLLET);

    /* The event loop */
    while (true)
    {
        {
                /* We have data on the fd waiting to be read. Read and
                 * display it. We must read whatever data is available
                 * completely, as we are running in edge-triggered mode
                 * and won't get a notification again for the same
                 * data. */
                char buf[512], * pbuf = buf;
                int done = 0, rc = fiber_read(infd, pbuf, 512);
                if (rc < 0){done = 1;};
                // if (rc == -1){done = 0;};
                if (rc == +0){done = 1;};

                if (!done){
                    /* Write the reply to connection */
                    s = write(infd, reply, sizeof(reply));
                    if (s < 0){done = 1;};
                }

                if (done){
                    printf("Closed connection on descriptor %d\n", infd);
                    fiber_epoll_unregister_events(epollfd, infd);
                    /* Closing the descriptor will make epoll remove it
                     * from the set of descriptors which are monitored. */
                    close(infd);

                    break;
                }
        }
    }

    return (void *)(0);
}

typedef struct epoll_params_t {
    int portnum;
    int epoll_fd; 
} epoll_params_t;

void* server(void* args)
{
    epoll_params_t * params = (epoll_params_t *)args;

    int portnum = params->portnum;
    int epollfd = params->epoll_fd;

    int sfd, s;
    struct epoll_event events[MAXEVENTS];

    sfd = create_and_bind(portnum);
    if (sfd == -1) abort();

    s = make_socket_non_blocking(sfd);
    if (s == -1) abort();

    s = listen(sfd, SOMAXCONN);
    if (s == -1) {
        perror("listen");
        abort();
    }

    // EventContext ctxs[MAXEVENTS] = {0};
    // EventContextControlBlock ctxcb = {
    //     .maxEvents     = MAXEVENTS,
    //     .usedEventMask = (~0ULL),
    //     .epoll_fd      = epollfd,
    //     .ctxs          = ctxs
    // };
    // fiber_set_localdata(fiber_ident(), 0, (uint64_t)(&ctxcb));
    fiber_epoll_register_events(epollfd, sfd, EPOLLIN | EPOLLET);

    client_params_t client_params;
    client_params.epoll_fd = epollfd;

    /* The event loop */
    while (1)
    {
        int n = fiber_epoll_wait(events, MAXEVENTS, FIBER_TIMEOUT_INFINITE);
        assert(n <= 1);

        if (n == 1){
            int i = 0;
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))) {
                /* An error has occured on this fd, or the socket is not
                 * ready for reading (why were we notified then?) */
                fprintf(stderr, "epoll error. events=%u\n", events[i].events);
                close(events[i].data.fd);
                break;
            }
            else {
                /* We have a notification on the listening socket, which
                 * means one or more incoming connections. */
                while (1)
                {
                    struct sockaddr in_addr;

                    socklen_t in_len = sizeof in_addr;
                    int infd = accept(sfd, &in_addr, &in_len);
                    if (infd == -1) {
                        printf("errno=%d, EAGAIN=%d, EWOULDBLOCK=%d\n", errno, EAGAIN, EWOULDBLOCK);
                        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                            /* We have processed all incoming
                             * connections. */
                            printf("processed all incoming connections.\n");
                            break;
                        }
                        else {
                            perror("accept");
                            break;
                        }
                    }
                    else{
                        client_params.fd = infd; /* not safe, need to allocate/deallocate */
                        fiber_create(requestHandler, (void*)(&client_params), NULL, 8192 * 2);
                    }
                }
            }
        }

    }

    fiber_epoll_unregister_events(epollfd, sfd);
    close(sfd);
    return EXIT_SUCCESS;
}

/////////////////////////////////////////////////////////////////
/* callbacks: create server task & thread message loop (epoll) */
/////////////////////////////////////////////////////////////////
static bool initializeTask(void* args) {
    fiber_create(server, args, NULL, 8192 * 2);
    return true;
}

static bool epoll_msgloop(void * args){
    epoll_params_t * param = (epoll_params_t *)args;

    struct epoll_event epoll_events[64];

    /* call epoll */
    int rc = epoll_wait(param->epoll_fd, epoll_events, 64, 10);
    if (unlikely(rc < 0)){
        return false;
    }

    /* dispatch events */
    fiber_epoll_post(rc, epoll_events);
    return true;
}
/////////////////////////////////////////////////////////////////

// #include <pthread.h>
int main(int argc, char* argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    FiberGlobalStartup();

    int portnum = atoi(argv[1]);

    epoll_params_t params;
    params.portnum  = portnum;
    params.epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    /* run another thread */
    fibthread_args_t args = {
      .threadStartup = initializeTask,
      .threadCleanup = NULL,
      .threadMsgLoop = epoll_msgloop,
      .args = (void *)(&params),
    };
    pthread_scheduler(&args);
    return 0;
}