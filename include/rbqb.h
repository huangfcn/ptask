#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <stdbool.h>

#include <semaphore.h>
#include <errno.h>

#include "sysdef.h"

#ifndef __BLOCKQ_MPMC_H__
#define __BLOCKQ_MPMC_H__

#define RBQ_NODE(name, type)                                            \
    typedef type name##_rbqnode_t;                                      \
    typedef type name##_node_t;                                         \
    typedef type name##node_t;

#define RBQ_HEAD(name, type)                                            \
    typedef struct name##_t {                                           \
        uint64_t head;                                                  \
        uint64_t tail;                                                  \
        size_t   size;                                                  \
        pthread_mutex_t lock;                                           \
        sem_t    sem_dat;                                               \
        sem_t    sem_spc;                                               \
        name##_rbqnode_t * data;                                        \
    } name##_t;

#define RBQ_HEAD_STATIC(name, type, _ORDER)                             \
    typedef struct name##_t {                                           \
        uint64_t head;                                                  \
        uint64_t tail;                                                  \
        size_t   size;                                                  \
        pthread_mutex_t lock;                                           \
        sem_t    sem_dat;                                               \
        sem_t    sem_spc;                                               \
        name##_rbqnode_t data[(1 << _ORDER)];                           \
    } name##_t;

#define RBQ_INIT(name)                                                  \
    static inline bool name##_init(                                     \
        name##_t* rbq, int order                                        \
    )                                                                   \
    {                                                                   \
        rbq->size = (1ULL << order);                                    \
        rbq->head = 0;                                                  \
        rbq->tail = 0;                                                  \
                                                                        \
        sem_init(&(rbq->sem_dat), 0, 0);                                \
        sem_init(&(rbq->sem_spc), 0, (1 << _ORDER));                    \
        pthread_mutex_init(&(rbq->lock), NULL);                         \
                                                                        \
        rbq->data = (name##_rbqnode_t*)_aligned_malloc(                 \
            rbq->size * sizeof(name##_rbqnode_t), 16                    \
        );                                                              \
        memset(                                                         \
            (void*)rbq->data, 0, rbq->size * sizeof(name##_rbqnode_t)   \
        );                                                              \
        /* printf("%d\n", sizeof(name##_rbqnode_t));                 */ \
        return (rbq->data != NULL);                                     \
    };

#define RBQ_INIT_STATIC(name, _ORDER)                                   \
    static inline bool name##_init(                                     \
        name##_t* rbq                                                   \
    )                                                                   \
    {                                                                   \
        rbq->size = (1ULL << _ORDER);                                   \
        rbq->head = 0;                                                  \
        rbq->tail = 0;                                                  \
                                                                        \
        sem_init(&(rbq->sem_dat), 0, 0);                                \
        sem_init(&(rbq->sem_spc), 0, (1 << _ORDER));                    \
        pthread_mutex_init(&(rbq->lock), NULL);                         \
                                                                        \
        memset(                                                         \
            (void*)(rbq->data), 0, rbq->size * sizeof(name##_rbqnode_t) \
        );                                                              \
                                                                        \
        return (true);                                                  \
    };

#define RBQ_FREE(name)                                                  \
    static inline void name##_free(name##_t* rbq)                       \
    {                                                                   \
        _aligned_free(rbq->data);                                       \
                                                                        \
        sem_destroy(&(rbq->sem_dat));                                   \
        sem_destroy(&(rbq->sem_spc));                                   \
        pthread_mutex_destroy(&(rbq->lock));                            \
    };

#define RBQ_FREE_STATIC(name)                                           \
    static inline void name##_free(name##_t* rbq)                       \
    {                                                                   \
        sem_destroy(&(rbq->sem_dat));                                   \
        sem_destroy(&(rbq->sem_spc));                                   \
        pthread_mutex_destroy(&(rbq->lock));                            \
    };

#define RBQ_TIMEDWAIT(name, us)                                         \
    static inline int name##_timedwait(sem_t * sem)                     \
    {                                                                   \
        struct timespec ts; int s;                                      \
        if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {                 \
            return false;                                               \
        }                                                               \
                                                                        \
        /* add 10ms */                                                  \
        ts.tv_nsec += us * 1000ULL;                                     \
        ts.tv_sec += ts.tv_nsec / 1000000000ULL;                        \
        ts.tv_nsec %= 1000000000ULL;                                    \
        while ((s = sem_timedwait(sem, &ts)) == -1 && errno == EINTR)   \
            continue;       /* Restart if interrupted by handler */     \
                                                                        \
        if (s == -1) {                                                  \
            return (errno == ETIMEDOUT) ? (+1) : (-1);                  \
        }                                                               \
        else{                                                           \
            return 0;                                                   \
        }                                                               \
    }

#define RBQ_FULL(name)                                                  \
    static inline bool name##_full(const name##_t* rbq)                 \
    {                                                                   \
        return (rbq->tail >= (rbq->head + rbq->size));                  \
    };

#define RBQ_EMPT(name)                                                  \
    static inline bool name##_empty(const name##_t* rbq)                \
    {                                                                   \
        return (rbq->head >= rbq->tail);                                \
    };

#define RBQ_SIZE(name)                                                  \
    static inline size_t name##_size(const name##_t* rbq)               \
    {                                                                   \
        return (( rbq->head >= rbq->tail ) ?                            \
                ( 0                      ) :                            \
                ( rbq->tail - rbq->head  ) ) ;                          \
    };

#define RBQ_PUSH(name, type, copyfunc)                                  \
    /* push @ mutiple producers */                                      \
    static inline bool name##_push(                                     \
        name##_t* rbq, const type * pdata                               \
    )                                                                   \
    {                                                                   \
        if (name##_timedwait(&(rbq->sem_spc))){                         \
            return false;                                               \
        }                                                               \
                                                                        \
        pthread_mutex_lock(&(rbq->lock));                               \
        name##_rbqnode_t* pnode = rbq->data + rbq->head;                \
        copyfunc(pdata, pnode);                                         \
        rbq->head = (rbq->head + 1) & (rbq->size - 1);                  \
        pthread_mutex_unlock(&(rbq->lock));                             \
                                                                        \
        /* done - update status */                                      \
        sem_post(&(rbq->sem_dat));                                      \
        return true;                                                    \
    };

#define RBQ_POP(name, type, copyfunc)                                   \
    /* push @ mutiple producers */                                      \
    static inline bool name##_pop(                                      \
        name##_t* rbq, type * pdata                                     \
    )                                                                   \
    {                                                                   \
        if (name##_timedwait(&(rbq->sem_dat)) != 0){                    \
            return false;                                               \
        }                                                               \
                                                                        \
        pthread_mutex_lock(&(rbq->lock));                               \
        name##_rbqnode_t* pnode = rbq->data + rbq->tail;                \
        copyfunc(pnode, pdata);                                         \
        rbq->tail = (rbq->tail + 1) & (rbq->size - 1);                  \
        pthread_mutex_unlock(&(rbq->lock));                             \
                                                                        \
        /* done - update status */                                      \
        sem_post(&(rbq->sem_spc));                                      \
        return true;                                                    \
    };

#define RBQ_PROTOTYPE_STATIC(name, type, copyfunc, _tmous, _ORDER)      \
    RBQ_NODE(name, type);                                               \
    RBQ_HEAD_STATIC(name, type, _ORDER);                                \
                                                                        \
    RBQ_INIT_STATIC(name, _ORDER);                                      \
    RBQ_FREE_STATIC(name);                                              \
                                                                        \
    RBQ_FULL(name);                                                     \
    RBQ_EMPT(name);                                                     \
    RBQ_SIZE(name);                                                     \
                                                                        \
    RBQ_TIMEDWAIT(name, _tmous);                                        \
                                                                        \
    RBQ_PUSH(name, type, copyfunc);                                     \
    RBQ_POP (name, type, copyfunc);
    
#endif