# libfibtask
	
	A compact stackful coroutines library (in C). Features:
	1. Very compact, small code base
	2. M:N scheduling (Multiple threads suported)
	3. Fully integeraged with epoll
	3. Stack Caching & Stack Protection
	4. Timeout supported


# Simple API

	///////////////////////////////////////////////////////////////////
	/* coroutine lib standard APIs:                                  */
	/* 1. libary initialization                                      */
	/* 2. create a task                                              */
	/* 3. yield                                                      */
	/* 4. resume                                                     */
	/* ------------------------------------------------------------- */
	/* 5. usleep (task level usleep, wont hangup the thread)         */
	/* 6. sched_yield                                                */
	///////////////////////////////////////////////////////////////////
	/* called @ system/thread startup */
	bool FibTaskGlobalStartup();
	bool FibTaskThreadStartup();

	/* create a task */
	FibTCB * fibtask_create(
		void *(*func)(void *), 
		void * args, 
		void * stackaddr, 
		uint32_t stacksize
    );

    /* yield will yield control to other task
    * current task will suspend until resume called on it
    */
    FibTCB * fibtask_yield(uint64_t code);
    uint64_t fibtask_resume(FibTCB * the_task);

    /* identify current running task */
    FibTCB * fibtask_ident();

    /* task usleep (accurate at ms level)*/
    void fibtask_usleep(int usec);

    /* same functionality of sched_yield, yield the processor
    * sched_yield causes the calling task to relinquish the CPU.
    * The task is moved to the end of the ready queue and 
    * a new task gets to run.
    */
    FibTCB * fibtask_sched_yield();
    ///////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////
    /* epoll integeration                                            */
    ///////////////////////////////////////////////////////////////////
    struct epoll_event;
    int fibtask_register_events(int fd, int events);
    int fibtask_epoll_wait(
    	struct epoll_event * events, 
    	int maxEvents, 
    	int timeout_in_ms
    );
    ///////////////////////////////////////////////////////////////////
