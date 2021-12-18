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
        uint32_t head;                                                  \
        uint32_t tail;                                                  \
        uint32_t size;                                                  \
        volatile int32_t ndat;                                          \
        pthread_mutex_t lock;                                           \
        pthread_cond_t  full;                                           \
        pthread_cond_t  empt;                                           \
        name##_rbqnode_t * data;                                        \
    } name##_t;

#define RBQ_HEAD_STATIC(name, type, _ORDER)                             \
    typedef struct name##_t {                                           \
        uint32_t head;                                                  \
        uint32_t tail;                                                  \
        uint32_t size;                                                  \
        volatile int32_t ndat;                                          \
        pthread_mutex_t lock;                                           \
        pthread_cond_t  full;                                           \
        pthread_cond_t  empt;                                           \
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
        rbq->ndat = 0;                                                  \
                                                                        \
        pthread_mutex_init(&(rbq->lock), NULL);                         \
        pthread_cond_init (&(rbq->full), NULL);                         \
        pthread_cond_init (&(rbq->empt), NULL);                         \
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
        pthread_mutex_init(&(rbq->lock), NULL);                         \
        pthread_cond_init (&(rbq->full), NULL);                         \
        pthread_cond_init (&(rbq->empt), NULL);                         \
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
        pthread_mutex_destroy(&(rbq->lock));                            \
    };

#define RBQ_FREE_STATIC(name)                                           \
    static inline void name##_free(name##_t* rbq)                       \
    {                                                                   \
        pthread_mutex_destroy(&(rbq->lock));                            \
    };

#define RBQ_TIMEDWAIT(name, us)                                         \
    static inline int name##_timedwait(                                 \
        pthread_cond_t  * cond,                                         \
        pthread_mutex_t * mutex                                         \
        )                                                               \
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
        while ((s = pthread_cond_timedwait(cond, mutex, &ts)) == -1 &&  \
               (errno == EINTR))                                        \
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
        return (rbq->ndat >= (rbq->size));                              \
    };

#define RBQ_EMPT(name)                                                  \
    static inline bool name##_empty(const name##_t* rbq)                \
    {                                                                   \
        return (rbq->ndat == 0);                                        \
    };

#define RBQ_SIZE(name)                                                  \
    static inline size_t name##_size(const name##_t* rbq)               \
    {                                                                   \
        return (rbq->ndat) ;                                            \
    };

#define RBQ_PUSH(name, type, copyfunc)                                  \
    /* push @ mutiple producers */                                      \
    static inline bool name##_push(                                     \
        name##_t* rbq, const type * pdata                               \
    )                                                                   \
    {                                                                   \
        pthread_mutex_lock(&(rbq->lock));                               \
        while (rbq->ndat >= rbq->size){                                 \
            pthread_cond_wait(&(rbq->full), &(rbq->lock));              \
        }                                                               \
                                                                        \
        name##_rbqnode_t* pnode = rbq->data + rbq->head;                \
        copyfunc(pdata, pnode);                                         \
        rbq->head = (rbq->head + 1) & (rbq->size - 1);                  \
        rbq->ndat = (rbq->ndat + 1);                                    \
        pthread_cond_signal (&(rbq->empt));                             \
        pthread_mutex_unlock(&(rbq->lock));                             \
                                                                        \
        return true;                                                    \
    };

#define RBQ_POP(name, type, copyfunc)                                   \
    /* push @ mutiple producers */                                      \
    static inline bool name##_pop(                                      \
        name##_t* rbq, type * pdata                                     \
    )                                                                   \
    {                                                                   \
        pthread_mutex_lock(&(rbq->lock));                               \
        if (rbq->ndat == 0){                                            \
            name##_timedwait(&(rbq->empt), &(rbq->lock));               \
            if (rbq->ndat == 0){                                        \
                pthread_mutex_unlock(&(rbq->lock));                     \
                return false;                                           \
            }                                                           \
        }                                                               \
                                                                        \
        name##_rbqnode_t* pnode = rbq->data + rbq->tail;                \
        copyfunc(pnode, pdata);                                         \
        rbq->tail = (rbq->tail + 1) & (rbq->size - 1);                  \
        rbq->ndat = (rbq->ndat - 1);                                    \
        pthread_cond_signal (&(rbq->full));                             \
        pthread_mutex_unlock(&(rbq->lock));                             \
                                                                        \
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