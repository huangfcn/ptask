/*
 * thread_ring.c - The classic `thread ring' benchmark, using pipes as its
 *                 communication medium.
 */
#include "pipe.h"

#include <stdio.h>
#include <stdlib.h>

#include "task.h"

#define THREADS 20

typedef struct {
    int threadnumber;

    pipe_consumer_t* in;
    pipe_producer_t* out;
} thread_context_t;

static thread_context_t contexts[THREADS];

static bool gameOver = false;

static void* thread_func(void* context)
{
    thread_context_t* ctx = context;

    int n = 0;

    while(pipe_pop(ctx->in, &n, 1))
    {
        if(n == 0)
        {
            printf("fiber %2d game over!\n", ctx->threadnumber);
            gameOver = true;
            break;
        }
        else{
            printf("fiber %2d passing %3d ...\n", ctx->threadnumber, n);
        }
        --n;

        pipe_push(ctx->out, &n, 1);
    }

    pipe_producer_free(ctx->out);
    pipe_consumer_free(ctx->in);

    return NULL;
}

// static fiber_t thread;

static void spawn_thread(thread_context_t* ctx)
{
    fiber_create(&thread_func, ctx, NULL, FIBER_STACKSIZE_MIN);
}

bool initializeTasks(void * args)
{
    int passes = (int64_t)args;

    pipe_t* last_pipe = pipe_new(sizeof(int), 0);

    pipe_producer_t* first = pipe_producer_new(last_pipe);

    for(int i = 0; i < THREADS - 1; ++i)
    {
        thread_context_t* ctx = contexts + i;

        ctx->threadnumber = i + 1;

        ctx->in = pipe_consumer_new(last_pipe);
        pipe_free(last_pipe);

        last_pipe = pipe_new(sizeof(int), 0);
        ctx->out = pipe_producer_new(last_pipe);

        spawn_thread(ctx);
    }

    contexts[THREADS-1] = (thread_context_t) {
        .threadnumber = THREADS,
        .in = pipe_consumer_new(last_pipe),
        .out = first
    };

    pipe_free(last_pipe);

    spawn_thread(contexts + THREADS - 1);

    pipe_push(first, &passes, 1);

    // while (!gameOver){
    //     fiber_sched_yield();
    // }

    return true;
}

int main(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage: %s [N]\nN = the number of times to pass around a token.\n", argv[0]);
        return 255;
    }

    int64_t passes = atoi(argv[1]);


    FiberGlobalStartup();
    /* run another thread */
    fibthread_args_t args = {
      .threadStartup = initializeTasks,
      .threadCleanup = NULL,
      .threadMsgLoop = NULL,
      .args = (void *)(passes),
    };
    pthread_scheduler(&args);

    return (0);
}