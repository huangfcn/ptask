#include <assert.h>
#include <stdio.h>

#include "libfiber.h"

void * generator(void * args){
    for (int i = 0; i < 1000; ++i){
        fiber_yield(i);
    }
    return (void *)(0);
}

void * generator_maintask(void * args){
    FibTCB * the_gen = fiber_create(generator, args, NULL, 8192 * 2);

    for (int i = 0; i < 1000; ++i){
        fiber_sched_yield();

        int64_t code = fiber_resume(the_gen);
        printf("code = %ld\n", code);
    }

    return (void *)(0);
}

void * generator_thread(void * args){
    /* initialize thread environment */
    FiberThreadStartup();

    /* create maintask (reuse thread's stack) */
    struct {} C;
    FibTCB * the_task = fiber_create(generator_maintask, args, (void *)(&C), 0UL);

    /* call maintask goto_context */
    goto_contxt2(&(the_task->regs));

    return ((void *)(0));
}

int main(){
    FiberGlobalStartup();

    generator_thread(NULL);

    return (0);
}