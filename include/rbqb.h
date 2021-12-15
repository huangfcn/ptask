#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <stdbool.h>

#include <semaphore.h>
#include <errno.h>

#include "sysdef.h"

#ifndef __LOCKED_RBQ_MPMC_H__
#define __LOCKED_RBQ_MPMC_H__

#define RBQ_NODE(name, type)                                            \
    typedef type name##_rbqnode_t;                                      \
    typedef type name##_node_t;                                         \
    typedef type name##node_t;

#define RBQ_HEAD(name, type)                                            \
    typedef struct name##_t {                                           \
        uint64_t head;                                                  \
        uint64_t tail;                                                  \
        size_t   size;                                                  \
        sem_t    sem_dat;                                               \
        sem_t    sem_spc;                                               \
        name##_rbqnode_t * data;                                        \
    } name##_t;

#define RBQ_HEAD_STATIC(name, type, _ORDER)                             \
    typedef struct name##_t {                                           \
        uint64_t head;                                                  \
        uint64_t tail;                                                  \
        size_t   size;                                                  \
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
    };

#define RBQ_FREE_STATIC(name)                                           \
    static inline void name##_free(name##_t* rbq)                       \
    {                                                                   \
        sem_destroy(&(rbq->sem_dat));                                   \
        sem_destroy(&(rbq->sem_spc));                                   \
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
        uint64_t currWriteIndex = FAA(&(rbq->head));                    \
                                                                        \
        currWriteIndex &= (rbq->size - 1);                              \
        name##_rbqnode_t* pnode = rbq->data + currWriteIndex;           \
                                                                        \
        /* fill - exclusive */                                          \
        copyfunc(pdata, pnode);                                         \
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
        uint64_t currReadIndex = FAA(&(rbq->tail));                     \
                                                                        \
        currReadIndex &= (rbq->size - 1);                               \
        name##_rbqnode_t* pnode = rbq->data + currReadIndex;            \
                                                                        \
        /* fill - exclusive */                                          \
        copyfunc(pnode, pdata);                                         \
                                                                        \
        /* done - update status */                                      \
        sem_post(&(rbq->sem_spc));                                      \
        return true;                                                    \
    };

#define RBQ_PUSH_ACQ(name, type)                                        \
    /* push @ mutiple producers */                                      \
    static inline type * name##_push_acq(                               \
        name##_t* rbq                                                   \
    )                                                                   \
    {                                                                   \
    	if (name##_timedwait(&(rbq->sem_spc)) != 0){                    \
    		return false;                                               \
    	}                                                               \
                                                                        \
        uint64_t currWriteIndex = FAA(&(rbq->head));                    \
                                                                        \
        currWriteIndex &= (rbq->size - 1);                              \
        name##_rbqnode_t* pnode = rbq->data + currWriteIndex;           \
                                                                        \
        return pnode;                                                   \
    }

#define RBQ_PUSH_UPD(name)                                              \
    /* push @ mutiple producers */                                      \
    static inline const type * name##_push_upd(                         \
        name##_t* rbq                                                   \
    )                                                                   \
    {                                                                   \
        /* done - update status */                                      \
        sem_post(&(rbq->sem_dat));                                      \
        return true;                                                    \
    };

#define RBQ_POP_ACQ(name, type)                                         \
    /* push @ mutiple producers */                                      \
    static inline const type * name##_pop_acq(                          \
        name##_t* rbq                                                   \
    )                                                                   \
    {                                                                   \
    	if (name##_timedwait(&(rbq->sem_dat)) != 0){                    \
    		return false;                                               \
    	}                                                               \
                                                                        \
        uint64_t currReadIndex = FAA(&(rbq->tail));                     \
                                                                        \
        currReadIndex &= (rbq->size - 1);                               \
        name##_rbqnode_t* pnode = rbq->data + currReadIndex;            \
                                                                        \
        return pnode;                                                   \
    }

#define RBQ_POP_UPD(name)                                               \
    /* push @ mutiple producers */                                      \
    static inline const type * name##_pop_upd(                          \
        name##_t* rbq                                                   \
    )                                                                   \
    {                                                                   \
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

#define RBQ_PROTOTYPE_STATIC_ACQ(name, type, _tmous, _ORDER)            \
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
    RBQ_PUSH_ACQ(name, type);                                           \
    RBQ_PUSH_UPD(name);                                                 \
    RBQ_POP_ACQ (name, type);                                           \
    RBQ_POP_UPD (name);

#endif