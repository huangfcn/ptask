#include <assert.h>
#include <stdio.h>

#include "sysdef.h"
#include "spinlock.h"
#include "chain.h"
#include "task.h"

void * generator(void * args){
	for (int i = 0; i < 1000; ++i){
		fibtask_yield(i);
	}
	return (void *)(0);
}


bool initializeTask(void* args) {
    return true;
}

void * generator_maintask(void * args){
	FibTCB * the_gen = fibtask_create(generator, args, NULL, 8192 * 2);

	for (int i = 0; i < 1000; ++i){
		fibtask_sched_yield();

		int64_t code = fibtask_resume(the_gen);
		printf("code = %ld\n", code);
	}

	return (void *)(0);
}

void * generator_thread(void * args){
    /* initialize thread environment */
    FibTaskThreadStartup();

    /* create maintask (reuse thread's stack) */
    struct {} C;
    FibTCB * the_task = fibtask_create(generator_maintask, args, (void *)(&C), 0UL);
    fibtask_set_thread_maintask(the_task);

    /* call maintask goto_context */
    goto_contxt2(&(the_task->regs));

    return ((void *)(0));
}

int main(){
	FibTaskGlobalStartup();

	generator_thread(NULL);

	return (0);
}