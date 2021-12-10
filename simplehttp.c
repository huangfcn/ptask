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

#include "libfiber.h"
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

ssize_t fdread(int fd, char * buf, int bufsize){
    ssize_t rc = read(fd, buf, bufsize);
    return (rc >= 0) ? (rc) : ((errno == EAGAIN) ? (-1) : (-2));
}

void * requestHandler(void* args)
{
    int infd = (int64_t)(args);
    int s = make_socket_non_blocking(infd);
    if (s == -1)
        abort();

    EventContext ctxs[MAXEVENTS] = {0};
    EventContextControlBlock ctxcb = {
        .maxEvents     = MAXEVENTS,
        .usedEventMask = ~0ULL,
        .tmpEventMasks =  0ULL,
        .ctxs = ctxs
    };
    fiber_set_localdata(fiber_ident(), 0, (uint64_t)(&ctxcb));
    epoll_install_callbacks(fiber_ident());

    struct epoll_event events[MAXEVENTS];
    fiber_register_events(infd, EPOLLIN | EPOLLET);

    /* The event loop */
    while (true)
    {
        int n = fiber_epoll_wait(events, MAXEVENTS, 2000);
        assert(n <= 1);

        if (n == 1) {
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
                /* We have data on the fd waiting to be read. Read and
                 * display it. We must read whatever data is available
                 * completely, as we are running in edge-triggered mode
                 * and won't get a notification again for the same
                 * data. */
                char buf[512], * pbuf = buf;
                ssize_t nread = 0, bufsize = sizeof buf, done = 0, rc; 
                while (1) {
                    rc = fdread(events[i].data.fd, pbuf, bufsize);
                    if (rc == -2){done = 1; break;};
                    if (rc == -1){done = 0; break;};
                    if (rc == +0){done = 1; break;};

                    nread += rc; pbuf += rc; bufsize -= rc;
                    if (bufsize <= 0){
                        nread = 0; pbuf = buf; bufsize = sizeof buf;
                    }
                }

                if (!done){
                    /* Write the reply to connection */
                    s = write(events[i].data.fd, reply, sizeof(reply));
                    if (s < 0){done = 1;};
                }

                if (done){
                    printf("Closed connection on descriptor %d\n", events[i].data.fd);
                    /* Closing the descriptor will make epoll remove it
                     * from the set of descriptors which are monitored. */
                    close(events[i].data.fd);

                    break;
                }
            }
        }
    }

    return (void *)(0);
}

void* server(void* args)
{
    int portnum = (int64_t)(args);

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

    EventContext ctxs[MAXEVENTS] = {0};
    EventContextControlBlock ctxcb = {
        .maxEvents     = MAXEVENTS,
        .usedEventMask = ~0ULL,
        .tmpEventMasks =  0ULL,
        .ctxs = ctxs
    };
    fiber_set_localdata(fiber_ident(), 0, (uint64_t)(&ctxcb));
    epoll_install_callbacks(fiber_ident());

    fiber_register_events(sfd, EPOLLIN | EPOLLET);

    /* The event loop */
    while (1)
    {
        int n = fiber_epoll_wait(events, MAXEVENTS, 1000);
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
                        fiber_create(requestHandler, (void*)((int64_t)infd), NULL, 8192 * 2);
                    }
                }
            }
        }

    }

    close(sfd);
    return EXIT_SUCCESS;
}


bool initializeTask(void* args) {
    fiber_create(server, args, NULL, 8192 * 2);
    return true;
}

int main(int argc, char* argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    FiberGlobalStartup();

    int64_t portnum = atoi(argv[1]);
    fibthread_args_t args = {
      .init_func = initializeTask,
      .args = (void *)(portnum),
    };

    epoll_thread(&args);

    return 0;
}