#ifndef _SPINLOCK_CMPXCHG_H
#define _SPINLOCK_CMPXCHG_H

#ifdef __SPINLOCK_USING_MUTEX__
#include <pthread.h>
typedef pthread_mutex_t spinlock_t;

#define spin_init(pmutex)  pthread_mutex_init(pmutex, NULL)
#define spin_lock(pmutex)  pthread_mutex_lock(pmutex)
#define spin_unlock(pmtx)  pthread_mutex_unlock(pmtx)
#define spin_destroy(pmtx) pthread_mutex_destroy(pmtx)

#else

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile int8_t lock;
    volatile int8_t pads[7];
} CACHE_ALIGN_POST spinlock_t;

#define SPINLOCK_ATTR static __inline __attribute__((always_inline, no_instrument_function))

/* Pause instruction to prevent excess processor bus usage */
#define cpu_relax() asm volatile("pause\n": : :"memory")

SPINLOCK_ATTR void spin_init(spinlock_t *s){
    s->lock = 0;
}

#if (1)
SPINLOCK_ATTR char __testandset(spinlock_t *p)
{
    char readval = 0;

    asm volatile (
            "lock; cmpxchgb %b2, %0"
            : "+m" (p->lock), "+a" (readval)
            : "r" (1)
            : "cc");
    return readval;
}

SPINLOCK_ATTR void spin_lock(spinlock_t *lock)
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

SPINLOCK_ATTR void spin_unlock(spinlock_t *s)
{
    s->lock = 0;
}

#else
#define spin_lock(p) while(!CAS32(&((p)->lock), 0, 1)){cpu_relax();}
#define spin_unlock(p) do {(p)->lock = 0;} while(0)
#endif


#define SPINLOCK_INITIALIZER { 0 }

SPINLOCK_ATTR void spin_destroy(spinlock_t *s){
    ;
}

#ifdef __cplusplus
};
#endif

#endif

#endif /* _SPINLOCK_CMPXCHG_H */
