# ptask
	ptask is a symetric stackful coroutine (task/fiber) library with pthread like API. 
	Although C++20 goes with stackless corouines, stackful coroutine with a much more 
	elegent way of yielding/resuming still has its ground. Since stackful 
	coroutine (task) is very close to thread, it will be convenient if the library 
	can provide coroutine aware synchronization methods. But most stackful coroutine 
	libraries in C/C++ are asymetric with very simple API (create/yield/resume) only, 
	so here comes ptask, a thread safe symetric coroutine library with pthread like 
	synchronization APIs.  

# Features:

	1. Very compact, small code base (< 1500 lines)
	2. Support 1:N and M:N scheduling (thread safe)
	3. Stack caching & stack protection
	4. Support coroutine aware pthread style mutex/semaphore/condition synchronization
	5. Support bitmask event synchronization to natually integerate async events 
	6. Support coroutine ware timeout/sleep (usleep)
	7. Support task local variables
    8. Fully integeraged with epoll
	
# Pthread Style API
	```C++
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
	/* called @ system startup */
	bool FiberGlobalStartup();

	/* create a task */
	fiber_t fiber_create(
		void *(*func)(void *), 
		void * args, 
		void * stackaddr, 
		uint32_t stacksize
    );

    /* yield will yield control to other task
    * current task will suspend until resume called on it
    */
    fiber_t fiber_yield(uint64_t code);
    uint64_t fiber_resume(fiber_t the_task);

    /* identify current running task */
    fiber_t fiber_ident();

    /* task usleep (accurate at ms level)*/
    void fiber_usleep(int usec);

    /* same functionality of sched_yield, yield the processor
    * sched_yield causes the calling task to relinquish the CPU.
    * The task is moved to the end of the ready queue and 
    * a new task gets to run.
    */
    fiber_t fiber_sched_yield();

    /* mutex */
    int  fiber_mutex_init(fiber_mutex_t * the_mutex);
    bool fiber_mutex_lock(fiber_mutex_t * the_mutex);
    bool fiber_mutex_unlock(fiber_mutex_t * the_mutex);
    void fiber_mutex_destroy(fiber_mutex_t * the_mutex);

    /* sempahore */
    int  fiber_sem_init(fiber_sem_t * psem, int initval);
    bool fiber_sem_wait(fiber_sem_t * psem);
    bool fiber_sem_timedwait(fiber_sem_t * psem, int timeout);
    bool fiber_sem_post(fiber_sem_t * psem);
    void fiber_sem_destroy(fiber_sem_t * psem);

    /* Conditional Variables */
    int  fiber_cond_init(fiber_cond_t * pcond);
    bool fiber_cond_wait(fiber_cond_t * pcond, fiber_mutex_t * pmutex);
    bool fiber_cond_timedwait(fiber_cond_t * pcond, fiber_mutex_t * pmutex, int timeout);
    bool fiber_cond_signal(fiber_cond_t * pcond);
    bool fiber_cond_broadcast(fiber_cond_t * pcond);
    void fiber_cond_destroy(fiber_cond_t * pcond);

    /* Extremely efficient bitmask based Events (ptask specific) 
    *  A task can wait for up to 64 events by specifying waiting any event or all events
    */
    uint64_t fiber_event_wait(uint64_t events_bitmask_waitingfor, int options, int timeout);
    int fiber_event_post(fiber_t the_task, uint64_t events_bitmask_in);
    ///////////////////////////////////////////////////////////////////
    ```
# epoll Integeration

    ///////////////////////////////////////////////////////////////////
    /* epoll integeration                                            */
    ///////////////////////////////////////////////////////////////////
    int fiber_epoll_register_events(int fd, int events);
    int fiber_epoll_unregister_event(fiber_t the_tcb, int index);
    int fiber_epoll_wait(
        struct epoll_event * events, 
        int maxEvents, 
        int timeout_in_ms
        );
    int fiber_epoll_post(
        int nEvents,
        struct epoll_event * events
        );
    ///////////////////////////////////////////////////////////////////
