#ifndef _SPINLOCK_CMPXCHG_H
#define _SPINLOCK_CMPXCHG_H

#ifdef __SPINLOCK_USING_MUTEX__
#include <pthread.h>
typedef pthread_mutex_t spinlock;

#define spin_init(pmutex)  pthread_mutex_init(pmutex, NULL)
#define spin_lock(pmutex)  pthread_mutex_lock(pmutex)
#define spin_unlock(pmtx)  pthread_mutex_unlock(pmtx)
#define spin_destroy(pmtx) pthread_mutex_destroy(pmtx)

#else

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile char lock;
} spinlock;

#define SPINLOCK_ATTR static __inline __attribute__((always_inline, no_instrument_function))

/* Pause instruction to prevent excess processor bus usage */
#define cpu_relax() asm volatile("pause\n": : :"memory")

SPINLOCK_ATTR char __testandset(spinlock *p)
{
    char readval = 0;

    asm volatile (
            "lock; cmpxchgb %b2, %0"
            : "+m" (p->lock), "+a" (readval)
            : "r" (1)
            : "cc");
    return readval;
}

SPINLOCK_ATTR void spin_init(spinlock *s){
    s->lock = 0;
}

SPINLOCK_ATTR void spin_lock(spinlock *lock)
{
    while (__testandset(lock)) {
        /* Should wait until lock is free before another try.
         * cmpxchg is write to cache, competing write for a sinlge cache line
         * would generate large amount of cache traffic. That's why this
         * implementation is not scalable compared to xchg based one. Otherwise,
         * they should have similar performance. */
        cpu_relax();
    }
}

SPINLOCK_ATTR void spin_unlock(spinlock *s)
{
    s->lock = 0;
}

#define SPINLOCK_INITIALIZER { 0 }

SPINLOCK_ATTR void spin_destroy(spinlock *s){
    ;
}

#ifdef __cplusplus
};
#endif

#endif

#endif /* _SPINLOCK_CMPXCHG_H */
