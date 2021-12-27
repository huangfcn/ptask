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

bool initializeTasks(void* args) {
    fiber_create(generator_maintask, args, NULL, 8192);
    return true;
}

int main(){
    FiberGlobalStartup();

    /* run another thread */
    fibthread_args_t args = {
      .threadStartup = initializeTasks,
      .threadCleanup = NULL,
      .args = (void *)(0),
    };
    pthread_scheduler(&args);

    return (0);
}