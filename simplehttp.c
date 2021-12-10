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

#include "sysdef.h"
#include "spinlock.h"
#include "chain.h"
#include "task.h"

static int create_and_bind(int port)
{

    int portnum = port;
    int s, sfd;

    sfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portnum);

    s = bind(sfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    return sfd;
}

static int make_socket_non_blocking(int sfd)
{
    int flags, s;

    flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1)
    {
        return -1;
    }

    flags |= O_NONBLOCK;
    s = fcntl(sfd, F_SETFL, flags);
    if (s == -1)
    {
        return -1;
    }

    return 0;
}

#define MAXEVENTS 64

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

void * requestHandler(void* args)
{
    int infd = (int)(args);
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
    fibtask_set_localdata(fibtask_ident(), 0, (uint64_t)(&ctxcb));
    epoll_install_callbacks(fibtask_ident());

    struct epoll_event events[MAXEVENTS];
    fibtask_register_events(infd, EPOLLIN | EPOLLET);

    int done = 0;
    /* The event loop */
    while (!done)
    {
        int n, i;

        n = fibtask_epoll_wait(events, MAXEVENTS, -1);
        for (i = 0; (i < n) && (!done); i++) {

            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN)))
            {
                /* An error has occured on this fd, or the socket is not
                 * ready for reading (why were we notified then?) */
                fprintf(stderr, "epoll error. events=%u\n", events[i].events);
                close(events[i].data.fd);
                continue;
            }
            else
            {
                /* We have data on the fd waiting to be read. Read and
                 * display it. We must read whatever data is available
                 * completely, as we are running in edge-triggered mode
                 * and won't get a notification again for the same
                 * data. */
                 while (1)
                {
                    ssize_t count;
                    char buf[512];

                    count = read(events[i].data.fd, buf, sizeof buf);
                    if (count == -1)
                    {
                        /* If errno == EAGAIN, that means we have read all
                         * data. So go back to the main loop. */
                        if (errno != EAGAIN)
                        {
                            perror("read");
                            done = 1;
                        }
                        break;
                    }
                    else if (count == 0)
                    {
                        /* End of file. The remote has closed the
                         * connection. */
                        done = 1;
                        break;
                    }

                    /* Write the reply to connection */
                    s = write(events[i].data.fd, reply, sizeof(reply));
                    if (s == -1)
                    {
                        perror("write");
                        abort();
                    }
                }

                if (done)
                {
                    printf("Closed connection on descriptor %d\n", events[i].data.fd);

                    /* Closing the descriptor will make epoll remove it
                     * from the set of descriptors which are monitored. */
                    close(events[i].data.fd);

                    break;
                }
            }
        }
    }
}

void* server(void* args)
{
    int portnum = (int)(args);

    int sfd, s;
    int efd;
    struct epoll_event event;
    struct epoll_event* events;

    sfd = create_and_bind(portnum);
    if (sfd == -1)
        abort();

    s = make_socket_non_blocking(sfd);
    if (s == -1)
        abort();

    s = listen(sfd, SOMAXCONN);
    if (s == -1)
    {
        perror("listen");
        abort();
    }

    efd = epoll_create1(0);
    if (efd == -1)
    {
        perror("epoll_create");
        abort();
    }

    EventContext ctxs[MAXEVENTS] = {0};
    EventContextControlBlock ctxcb = {
        .maxEvents     = MAXEVENTS,
        .usedEventMask = ~0ULL,
        .tmpEventMasks =  0ULL,
        .ctxs = ctxs
    };
    fibtask_set_localdata(fibtask_ident(), 0, (uint64_t)(&ctxcb));
    epoll_install_callbacks(fibtask_ident());

    fibtask_register_events(sfd, EPOLLIN | EPOLLET);

    /* Buffer where events are returned */
    events = calloc(MAXEVENTS, sizeof event);

    /* The event loop */
    while (1)
    {
        int n, i;

        n = fibtask_epoll_wait(events, MAXEVENTS, -1);
        for (i = 0; i < n; i++) {

            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN)))
            {
                /* An error has occured on this fd, or the socket is not
                 * ready for reading (why were we notified then?) */
                fprintf(stderr, "epoll error. events=%u\n", events[i].events);
                close(events[i].data.fd);
                continue;
            }
            else if (sfd == events[i].data.fd)
            {
                /* We have a notification on the listening socket, which
                 * means one or more incoming connections. */
                while (1)
                {
                    struct sockaddr in_addr;
                    socklen_t in_len;
                    int infd;

                    in_len = sizeof in_addr;
                    infd = accept(sfd, &in_addr, &in_len);
                    if (infd == -1)
                    {
                        printf("errno=%d, EAGAIN=%d, EWOULDBLOCK=%d\n", errno, EAGAIN, EWOULDBLOCK);
                        if ((errno == EAGAIN) ||
                            (errno == EWOULDBLOCK))
                        {
                            /* We have processed all incoming
                             * connections. */
                            printf("processed all incoming connections.\n");
                            break;
                        }
                        else
                        {
                            perror("accept");
                            break;
                        }
                    }
                    else{
                        fibtask_create(requestHandler, (void*)(infd), NULL, 8192 * 2);
                    }
                }
            }
        }

    }

    free(events);

    close(sfd);

    return EXIT_SUCCESS;
}


bool initializeTask(void* args) {
    fibtask_create(server, args, NULL, 8192 * 2);
    return true;
}

int main(int argc, char* argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    FibTaskGlobalStartup();

    int portnum = atoi(argv[1]);
    fibthread_args_t args = {
      .init_func = initializeTask,
      .args = (void *)(portnum),
    };

    epoll_thread(&args);

    return 0;
}