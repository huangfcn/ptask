#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <stdbool.h>

#include "task.h"

#ifndef __FIBERQ_RBQ_H__
#define __FIBERQ_RBQ_H__

#define FIBERQ_HEAD(name, type)                                         \
    typedef struct name##_t {                                           \
        int32_t head;                                                   \
        int32_t tail;                                                   \
        size_t  ndat;                                                   \
        size_t  size;                                                   \
        fiber_mutex_t lock;                                             \
        fiber_cond_t  empt;                                             \
        fiber_cond_t  full;                                             \
        type   * data;                                                  \
    } name##_t;

#define FIBERQ_HEAD_STATIC(name, type, _SIZE)                           \
    typedef struct name##_t {                                           \
        int32_t head;                                                   \
        int32_t tail;                                                   \
        size_t  ndat;                                                   \
        size_t  size;                                                   \
        fiber_mutex_t lock;                                             \
        fiber_cond_t  empt;                                             \
        fiber_cond_t  full;                                             \
        type     data[_SIZE];                                           \
    } name##_t;

#define FIBERQ_INIT(name, type)                                         \
    static inline bool name##_init(                                     \
        name##_t* rbq, size_t qsize                                     \
    )                                                                   \
    {                                                                   \
        rbq->size = qsize;                                              \
        rbq->head = 0;                                                  \
        rbq->tail = 0;                                                  \
        rbq->ndat = 0;                                                  \
                                                                        \
        fiber_cond_init (&(rbq->full));                                 \
        fiber_cond_init (&(rbq->empt));                                 \
        fiber_mutex_init(&(rbq->lock));                                 \
                                                                        \
        rbq->data = (type *)_aligned_malloc(                            \
            rbq->size * sizeof(type), 16                                \
        );                                                              \
        memset(                                                         \
            (void*)rbq->data, 0, rbq->size * sizeof(type)               \
        );                                                              \
        return (rbq->data != NULL);                                     \
    };

#define FIBERQ_INIT_STATIC(name, type, _SIZE)                           \
    static inline bool name##_init(                                     \
        name##_t* rbq                                                   \
    )                                                                   \
    {                                                                   \
        rbq->size = _SIZE;                                              \
        rbq->head = 0;                                                  \
        rbq->tail = 0;                                                  \
        rbq->ndat = 0;                                                  \
                                                                        \
        fiber_cond_init (&(rbq->full));                                 \
        fiber_cond_init (&(rbq->empt));                                 \
        fiber_mutex_init(&(rbq->lock));                                 \
                                                                        \
        memset(                                                         \
            (void*)rbq->data, 0, rbq->size * sizeof(type)               \
        );                                                              \
        return (true);                                                  \
    };

#define FIBERQ_FREE(name)                                               \
    static inline void name##_free(name##_t* rbq)                       \
    {                                                                   \
        _aligned_free(rbq->data);                                       \
                                                                        \
        fiber_cond_destroy (&(rbq->full));                              \
        fiber_cond_destroy (&(rbq->empt));                              \
        fiber_mutex_destroy(&(rbq->lock));                              \
    };

#define FIBERQ_FREE_STATIC(name)                                        \
    static inline void name##_free(name##_t* rbq)                       \
    {                                                                   \
        fiber_cond_destroy (&(rbq->full));                              \
        fiber_cond_destroy (&(rbq->empt));                              \
        fiber_mutex_destroy(&(rbq->lock));                              \
    };

#define FIBERQ_FULL(name)                                               \
    static inline bool name##_full(const name##_t* rbq)                 \
    {                                                                   \
        return (rbq->ndat >= rbq->size);                                \
    };

#define FIBERQ_EMPT(name)                                               \
    static inline bool name##_empty(const name##_t* rbq)                \
    {                                                                   \
        return (rbq->ndat <= 0);                                        \
    };

#define FIBERQ_SIZE(name)                                               \
    static inline size_t name##_size(const name##_t* rbq)               \
    {                                                                   \
        return (rbq->ndat) ;                                            \
    };

#define FIBERQ_PUSH(name, type, copyfunc)                               \
    /* push @ mutiple producers */                                      \
    static inline bool name##_push(                                     \
        name##_t* rbq, const type * pdata                               \
    )                                                                   \
    {                                                                   \
        fiber_mutex_lock(&(rbq->lock));                                 \
        while (rbq->ndat >= rbq->size) {                                \
            fiber_cond_wait(&rbq->full, &rbq->lock);                    \
        }                                                               \
                                                                        \
        type * pnode = rbq->data + rbq->head;                           \
        copyfunc(pdata, pnode);                                         \
        rbq->head = (rbq->head + 1) % (rbq->size);                      \
        rbq->ndat = (rbq->ndat + 1) ;                                   \
                                                                        \
        fiber_cond_signal (&rbq->empt);                                 \
        fiber_mutex_unlock(&rbq->lock);                                 \
        return true;                                                    \
    };

#define FIBERQ_POP(name, type, copyfunc)                                \
    /* push @ mutiple producers */                                      \
    static inline bool name##_pop(                                      \
        name##_t* rbq, type * pdata                                     \
    )                                                                   \
    {                                                                   \
        fiber_mutex_lock(&(rbq->lock));                                 \
        while (rbq->ndat <= 0) {                                        \
            fiber_cond_wait(&rbq->empt, &rbq->lock);                    \
        }                                                               \
                                                                        \
        type * pnode = rbq->data + rbq->tail;                           \
        copyfunc(pnode, pdata);                                         \
        rbq->tail = (rbq->tail + 1) % (rbq->size);                      \
        rbq->ndat = (rbq->ndat - 1) ;                                   \
                                                                        \
        fiber_cond_signal (&rbq->full);                                 \
        fiber_mutex_unlock(&rbq->lock);                                 \
        return true;                                                    \
    };

#define FIBERQ_PROTOTYPE(name, type, copyfunc)                          \
    FIBERQ_HEAD(name, type);                                            \
                                                                        \
    FIBERQ_INIT(name, type);                                            \
    FIBERQ_FREE(name);                                                  \
                                                                        \
    FIBERQ_FULL(name);                                                  \
    FIBERQ_EMPT(name);                                                  \
    FIBERQ_SIZE(name);                                                  \
                                                                        \
    FIBERQ_PUSH(name, type, copyfunc);                                  \
    FIBERQ_POP (name, type, copyfunc);

#define FIBERQ_PROTOTYPE_STATIC(name, type, copyfunc, _SIZE)            \
    FIBERQ_HEAD_STATIC(name, type, _SIZE);                              \
                                                                        \
    FIBERQ_INIT_STATIC(name, type, _SIZE);                              \
    FIBERQ_FREE_STATIC(name);                                           \
                                                                        \
    FIBERQ_FULL(name);                                                  \
    FIBERQ_EMPT(name);                                                  \
    FIBERQ_SIZE(name);                                                  \
                                                                        \
    FIBERQ_PUSH(name, type, copyfunc);                                  \
    FIBERQ_POP (name, type, copyfunc);
    
#endif