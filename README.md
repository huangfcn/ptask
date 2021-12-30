# libptask
	ptask is a symetric stackful coroutine (task/fiber) library with pthread like API. Although c++20 goes with stackless corouines, stackful coroutine with its much more elegent way of waiting/resuming still has its own fields. Since stackful coroutine (task) is very close to thread, it will be very convenient if the library can provide coroutine aware synchronization methods. But most stackful coroutine libraries in C/C++ are asymetric with very simple API (create/yield/resume) only, so here comes ptask, a thread safe symetric coroutine library with pthread like synchronization APIs.  

	1. Very compact, small code base
	2. support 1:N and M:N scheduling (Multiple threads)
	3. Fully integeraged with epoll
	4. Stack Caching & Stack Protection
	5. support coroutine aware pthread style mutex/semaphore/condition synchronization
	6. support coroutine ware timeout/sleep (usleep)
	7. support task local variables

# Pthread style API

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
	bool FiberGlobalStartup();

	/* create a task */
	FibTCB * fiber_create(
		void *(*func)(void *), 
		void * args, 
		void * stackaddr, 
		uint32_t stacksize
    );

    /* yield will yield control to other task
    * current task will suspend until resume called on it
    */
    FibTCB * fiber_yield(uint64_t code);
    uint64_t fiber_resume(FibTCB * the_task);

    /* identify current running task */
    FibTCB * fiber_ident();

    /* task usleep (accurate at ms level)*/
    void fiber_usleep(int usec);

    /* same functionality of sched_yield, yield the processor
    * sched_yield causes the calling task to relinquish the CPU.
    * The task is moved to the end of the ready queue and 
    * a new task gets to run.
    */
    FibTCB * fiber_sched_yield();

    /* mutex */
    int  fiber_mutex_init(fiber_mutex_t * the_mutex);
    bool fiber_mutex_lock(fiber_mutex_t * the_mutex);
    bool fiber_mutex_unlock(fiber_mutex_t * the_mutex);
    void fiber_mutex_destroy(fiber_mutex_t * the_mutex);

    /* sempahore */
    int  fiber_sem_init(FibSemaphore * psem, int initval);
    bool fiber_sem_wait(FibSemaphore * psem);
    bool fiber_sem_timedwait(FibSemaphore * psem, int timeout);
    bool fiber_sem_post(FibSemaphore * psem);
    void fiber_sem_destroy(FibSemaphore * psem);

    /* Conditional Variables */
    int  fiber_cond_init(FibCondition * pcond);
    bool fiber_cond_wait(FibCondition * pcond, FibMutex * pmutex);
    bool fiber_cond_timedwait(FibCondition * pcond, FibMutex * pmutex, int timeout);
    bool fiber_cond_signal(FibCondition * pcond);
    bool fiber_cond_broadcast(FibCondition * pcond);
    void fiber_cond_destroy(FibCondition * pcond);

    /* Extremely efficient bitmask based Events (ptask specific) */
    uint64_t fiber_event_wait(uint64_t events_in, int options, int timeout);
    int fiber_event_post(FibTCB * the_task, uint64_t events_in);
    ///////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////
    /* epoll integeration                                            */
    ///////////////////////////////////////////////////////////////////
    int fiber_epoll_register_events(int fd, int events);
    int fiber_epoll_unregister_event(FibTCB * the_tcb, int index);
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
