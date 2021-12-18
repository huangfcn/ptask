#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <stdbool.h>

#include "sysdef.h"

#ifndef __LOCKFREE_RBQ_MPMC_H__
#define __LOCKFREE_RBQ_MPMC_H__

#define RBQ_NODE(name, type)                                            \
    typedef struct CACHE_ALIGN_PRE name##_rbqnode_t {                   \
        type object;                                                    \
        volatile uint32_t status;                                       \
    } CACHE_ALIGN_POST name##_rbqnode_t;                                \
    typedef struct name##_rbqnode_t name##_node_t;                      \
    typedef struct name##_rbqnode_t name##node_t;

#define RBQ_HEAD(name, type)                                            \
    typedef struct name##_t {                                           \
        CACHE_ALIGN_PRE volatile uint64_t head CACHE_ALIGN_POST;        \
        CACHE_ALIGN_PRE volatile uint64_t tail CACHE_ALIGN_POST;        \
        CACHE_ALIGN_PRE size_t size CACHE_ALIGN_POST;                   \
        name##_rbqnode_t * data;                                        \
    } name##_t;

#define RBQ_HEAD_STATIC(name, type, _ORDER)                             \
    typedef struct name##_t {                                           \
        CACHE_ALIGN_PRE volatile uint64_t head CACHE_ALIGN_POST;        \
        CACHE_ALIGN_PRE volatile uint64_t tail CACHE_ALIGN_POST;        \
        CACHE_ALIGN_PRE size_t size CACHE_ALIGN_POST;                   \
        name##_rbqnode_t data[(1 << _ORDER)];                           \
    } name##_t;

#define STATUS_EMPT    (0)
#define STATUS_FILL    (1)
#define STATUS_READ    (2)
#define STATUS_FULL    (3)

#define RBQ_INIT(name)                                                  \
    static inline bool name##_init(                                     \
        name##_t* rbq, int order                                        \
    )                                                                   \
    {                                                                   \
        rbq->size = (1ULL << order);                                    \
        rbq->head = 0;                                                  \
        rbq->tail = 0;                                                  \
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
    };

#define RBQ_FREE_STATIC(name)                                           \
    static inline void name##_free(name##_t* rbq)                       \
    {                                                                   \
    };

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

#define RBQ_PUSH(name, type, copyfunc, waitfunc)                        \
    /* push @ mutiple producers */                                      \
    static inline bool name##_push(                                     \
        name##_t* rbq, const type * pdata                               \
    )                                                                   \
    {                                                                   \
        uint64_t currReadIndexA, currWriteIndex, nextWriteIndex;        \
                                                                        \
        /* check rbq queue full */                                      \
        currWriteIndex = rbq->tail;                                     \
        currReadIndexA = rbq->head;                                     \
        if (currWriteIndex >= (currReadIndexA + rbq->size)){            \
            return false;                                               \
        }                                                               \
                                                                        \
        /* reserve currWriteIndex */                                    \
        nextWriteIndex = FAA(&(rbq->tail));                             \
        currWriteIndex = (nextWriteIndex - 1) & (rbq->size - 1);        \
                                                                        \
        /* We know that space @ currWriteIndex is reserved for us. */   \
        /* In case of slow writer,                                 */   \
        /* we use CAS to ensure a correct data swap.               */   \
        name##_rbqnode_t* pnode = rbq->data + currWriteIndex;           \
        while (!CAS32(&(pnode->status), STATUS_EMPT, STATUS_FILL))      \
        {                                                               \
            waitfunc((currWriteIndex & 1) + 1);                         \
        }                                                               \
                                                                        \
        /* fill - exclusive */                                          \
        copyfunc(pdata, &(pnode->object));                              \
                                                                        \
        /* done - update status */                                      \
        CAS32(&(pnode->status), STATUS_FILL, STATUS_FULL);              \
                                                                        \
        return true;                                                    \
    };

#define RBQ_PUSH_ACQ(name, type, waitfunc)                              \
    /* push @ mutiple producers */                                      \
    static inline name##_rbqnode_t* name##_push_acq(                    \
        name##_t* rbq                                                   \
    )                                                                   \
    {                                                                   \
        uint64_t currReadIndexA, currWriteIndex, nextWriteIndex;        \
                                                                        \
        /* check rbq queue full */                                      \
        currWriteIndex = rbq->tail;                                     \
        currReadIndexA = rbq->head;                                     \
        if (currWriteIndex >= (currReadIndexA + rbq->size)){            \
            return (NULL);                                              \
        }                                                               \
                                                                        \
        /* reserve currWriteIndex */                                    \
        nextWriteIndex = FAA(&(rbq->tail));                             \
        currWriteIndex = (nextWriteIndex - 1) & (rbq->size - 1);        \
                                                                        \
        /* We know that space @ currWriteIndex is reserved for us. */   \
        /* In case of slow writer,                                 */   \
        /* we use CAS to ensure a correct data swap.               */   \
        name##_rbqnode_t* pnode = rbq->data + currWriteIndex;           \
        while (!CAS32(&(pnode->status), STATUS_EMPT, STATUS_FILL))      \
        {                                                               \
            waitfunc((currWriteIndex & 1) + 1);                         \
        }                                                               \
                                                                        \
        return pnode;                                                   \
    };

#define RBQ_PUSH_UPD(name)                                              \
    /* push @ mutiple producers */                                      \
    static inline bool name##_push_upd(                                 \
        name##_t* rbq, name##_rbqnode_t* pnode                          \
    )                                                                   \
    {                                                                   \
        /* done - update status */                                      \
        pnode->status = STATUS_FULL;                                    \
                                                                        \
        return true;                                                    \
    };

#define RBQ_POP(name, type, copyfunc, waitfunc)                         \
    /* pop @ mutiple consumers */                                       \
    static inline bool name##_pop(                                      \
        name##_t* rbq, type * pdata                                     \
    )                                                                   \
    {                                                                   \
        uint64_t currWritIndex, currReadIndex, nextReadIndex;           \
                                                                        \
        /* check queue empty */                                         \
        currReadIndex = rbq->head;                                      \
        currWritIndex = rbq->tail;                                      \
        if (currReadIndex >= currWritIndex){return false;}              \
                                                                        \
        /* now perfrom the FAA operation on the read index.          */ \
        /* the Space @ currReadIndex will be reserved for us.        */ \
        nextReadIndex = FAA(&(rbq->head));                              \
        currReadIndex = (nextReadIndex - 1) & (rbq->size - 1);          \
                                                                        \
        /* We know that space @ currReadIndex is reserved for us.    */ \
        /* In case of slow writer,                                   */ \
        /* we use CAS to ensure a correct data swap                  */ \
        name##_rbqnode_t* pnode = rbq->data + currReadIndex;            \
        while (!CAS32(&(pnode->status), STATUS_FULL, STATUS_READ))      \
        {                                                               \
            waitfunc((currReadIndex & 1) + 1);                          \
        }                                                               \
                                                                        \
        /* read - exclusive */                                          \
        copyfunc(&(pnode->object), pdata);                              \
                                                                        \
        /* done - update status */                                      \
        CAS32(&(pnode->status), STATUS_READ, STATUS_EMPT);              \
                                                                        \
        /* return - data */                                             \
        return true;                                                    \
    };

#define RBQ_POP_ACQ(name, type, waitfunc)                               \
    /* pop @ mutiple consumers */                                       \
    static inline name##_rbqnode_t* name##_pop_acq(                     \
        name##_t* rbq                                                   \
    )                                                                   \
    {                                                                   \
        uint64_t currWritIndex, currReadIndex, nextReadIndex;           \
                                                                        \
        /* check queue empty */                                         \
        currReadIndex = rbq->head;                                      \
        currWritIndex = rbq->tail;                                      \
        if (currReadIndex >= currWritIndex){return NULL;}               \
                                                                        \
        /* now perfrom the FAA operation on the read index.          */ \
        /* the Space @ currReadIndex will be reserved for us.        */ \
        nextReadIndex = FAA(&(rbq->head));                              \
        currReadIndex = (nextReadIndex - 1) & (rbq->size - 1);          \
                                                                        \
        /* We know that space @ currReadIndex is reserved for us.    */ \
        /* In case of slow writer,                                   */ \
        /* we use CAS to ensure a correct data swap                  */ \
        name##_rbqnode_t* pnode = rbq->data + currReadIndex;            \
        while (!CAS32(&(pnode->status), STATUS_FULL, STATUS_READ))      \
        {                                                               \
            waitfunc((currReadIndex & 1) + 1);                          \
        }                                                               \
                                                                        \
        /* read - exclusive */                                          \
        return pnode;                                                   \
    };

#define RBQ_POP_UPD(name)                                               \
    /* pop @ mutiple consumers */                                       \
    static inline bool name##_pop_upd(                                  \
        name##_t* rbq, name##_rbqnode_t* pnode                          \
    )                                                                   \
    {                                                                   \
        /* done - update status */                                      \
        pnode->status = STATUS_EMPT;                                    \
                                                                        \
        /* return - data */                                             \
        return true;                                                    \
    };

#define RBQ_PUSHSP(name, type, copyfunc)                                \
    /* push @ single producer single consumer */                        \
    static inline bool name##_pushspsc(                                 \
        name##_t* rbq, const type * pdata                               \
    )                                                                   \
    {                                                                   \
        /* check rbq queue full */                                      \
        uint64_t currWriteIndex = rbq->tail;                            \
        uint64_t currReadIndexA = rbq->head;                            \
        if (currWriteIndex >= (currReadIndexA + rbq->size)){            \
            return false;                                               \
        }                                                               \
                                                                        \
        currWriteIndex = currWriteIndex & (rbq->size - 1);              \
        name##_rbqnode_t* pnode = rbq->data + currWriteIndex;           \
                                                                        \
        copyfunc(pdata, &(pnode->object));                              \
                                                                        \
        rbq->tail = currWriteIndex + 1;                                 \
        return true;                                                    \
    };

#define RBQ_POPSC(name, type, copyfunc)                                 \
    /* pop @ mutiple consumers */                                       \
    static inline bool name##_popspsc(                                  \
        name##_t* rbq, type * pdata                                     \
    )                                                                   \
    {                                                                   \
        /* check queue empty */                                         \
        uint64_t currReadIndex = rbq->head;                             \
        uint64_t currWritIndex = rbq->tail;                             \
        if (currReadIndex >= currWritIndex){return false;}              \
                                                                        \
        currReadIndex = currReadIndex & (rbq->size - 1);                \
        name##_rbqnode_t* pnode = rbq->data + currReadIndex;            \
        copyfunc(&(pnode->object), pdata);                              \
                                                                        \
        rbq->head = currReadIndex + 1;                                  \
        return true;                                                    \
    };

#define RBQ_PROTOTYPE(name, type, copyfunc, waitfunc)                   \
    RBQ_NODE(name, type);                                               \
    RBQ_HEAD(name, type);                                               \
                                                                        \
    RBQ_INIT(name);                                                     \
    RBQ_FREE(name);                                                     \
                                                                        \
    RBQ_FULL(name);                                                     \
    RBQ_EMPT(name);                                                     \
    RBQ_SIZE(name);                                                     \
                                                                        \
    RBQ_PUSH_ACQ(name, type, waitfunc);                                 \
    RBQ_PUSH_UPD(name);                                                 \
    RBQ_POP_ACQ (name, type, waitfunc);                                 \
    RBQ_POP_UPD (name);                                                 \
                                                                        \
    RBQ_PUSH(name, type, copyfunc, waitfunc);                           \
    RBQ_POP (name, type, copyfunc, waitfunc);                           \
                                                                        \
    RBQ_PUSHSP(name, type, copyfunc);                                   \
    RBQ_POPSC (name, type, copyfunc);

#define RBQ_PROTOTYPE_ACQ(name, type, waitfunc)                         \
    RBQ_NODE(name, type);                                               \
    RBQ_HEAD(name, type);                                               \
                                                                        \
    RBQ_INIT(name);                                                     \
    RBQ_FREE(name);                                                     \
                                                                        \
    RBQ_FULL(name);                                                     \
    RBQ_EMPT(name);                                                     \
    RBQ_SIZE(name);                                                     \
                                                                        \
    RBQ_PUSH_ACQ(name, type, waitfunc);                                 \
    RBQ_PUSH_UPD(name);                                                 \
    RBQ_POP_ACQ (name, type, waitfunc);                                 \
    RBQ_POP_UPD (name);

#define RBQ_PROTOTYPE_STATIC(name, type, copyfunc, waitfunc, _ORDER)    \
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
    RBQ_PUSH_ACQ(name, type, waitfunc);                                 \
    RBQ_PUSH_UPD(name);                                                 \
    RBQ_POP_ACQ (name, type, waitfunc);                                 \
    RBQ_POP_UPD (name);                                                 \
                                                                        \
    RBQ_PUSH(name, type, copyfunc, waitfunc);                           \
    RBQ_POP (name, type, copyfunc, waitfunc);

#define RBQ_PROTOTYPE_STATIC_ACQ(name, type, waitfunc, _ORDER)          \
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
    RBQ_PUSH_ACQ(name, type, waitfunc);                                 \
    RBQ_PUSH_UPD(name);                                                 \
    RBQ_POP_ACQ (name, type, waitfunc);                                 \
    RBQ_POP_UPD (name);

#endif