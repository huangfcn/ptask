#ifndef __STB_CHAIN_H__
#define __STB_CHAIN_H__
#include <stddef.h>

//////////////////////////////////////////////////////////////////////////////////////////////
//  This is used to manage a chain.  A chain consists of a doubly                           //
//  linked list of zero or more nodes.                                                      //
//                                                                                          //
//  NOTE:  This implementation does not require special checks for                          //
//         manipulating the first and last elements on the chain.                           //
//         To accomplish this the chain control structure is                                //
//         treated as two overlapping chain nodes.  The permanent                           //
//         head of the chain overlays a node structure on the                               //
//         first and permanent_null (null_) fields. The permanent                           //
//         tail of the chain overlays a node structure on the                               //
//         permanent_null (null_) and last elements of the structure.                       //
//////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////////
// In C++ there can be structure lists and class lists                                      //
//////////////////////////////////////////////////////////////////////////////////////////////
#ifdef __cplusplus
#define CHAIN_TYPEOF(type) type
#else
#define CHAIN_TYPEOF(type) struct type
#endif

//////////////////////////////////////////////////////////////////////////////////////////////
// provide type & field to list operations for non-zero offseted node                       //
//////////////////////////////////////////////////////////////////////////////////////////////
#define CHAIN_ENTRY(type)                                               \
struct {                                                                \
   CHAIN_TYPEOF(type) * next;                                           \
   CHAIN_TYPEOF(type) * previous;                                       \
}

#define CHAIN_HEAD(name, type)                                          \
struct name {                                                           \
   CHAIN_TYPEOF(type) * first;                                          \
   CHAIN_TYPEOF(type) * null_;                                          \
   CHAIN_TYPEOF(type) * last;                                           \
}

#define CHAIN_INIT_EMPTY(the_list, type, fld)                           \
{                                                                       \
   CHAIN_TYPEOF(type) * __the_head, * __the_tail;                       \
                                                                        \
   __the_head = CHAIN_HEAD_NODE(the_list, type, fld);                   \
   __the_tail = CHAIN_TAIL_NODE(the_list, type, fld);                   \
                                                                        \
   (the_list)->first = __the_tail;                                      \
   (the_list)->last  = __the_head;                                      \
   (the_list)->null_ = NULL;                                            \
}

#define CHAIN_HEAD_NODE_INTERNAL(the_list, type, fld)                   \
  ((CHAIN_TYPEOF(type) *)(((uint8_t *)(the_list)) - offsetof(struct type, fld)))

#define CHAIN_HEAD_NODE(the_list, type, fld)                            \
  CHAIN_HEAD_NODE_INTERNAL((&((the_list)->first)), type, fld)

#define CHAIN_TAIL_NODE(the_list, type, fld)                            \
  CHAIN_HEAD_NODE_INTERNAL((&((the_list)->null_)), type, fld)

#define CHAIN_FRST(the_list)      ((the_list)->first       )
#define CHAIN_LAST(the_list)      ((the_list)->last        )
#define CHAIN_NEXT(the_node, fld) ((the_node)->fld.next    )
#define CHAIN_PREV(the_node, fld) ((the_node)->fld.previous)

#define CHAIN_INSERT_AFTER(after_node, the_node, type, fld) do {        \
   CHAIN_TYPEOF(type) * __before_node;                                  \
                                                                        \
   __before_node               = (after_node)->fld.next;                \
   (the_node)->fld.previous    = (after_node);                          \
   (after_node)->fld.next      = (the_node);                            \
   (the_node)->fld.next        = __before_node;                         \
   __before_node->fld.previous = (the_node);                            \
} while (0)

#define CHAIN_INSERT_BEFORE(before_node, the_node, type, fld) do {      \
   CHAIN_TYPEOF(type) * __after_node;                                   \
                                                                        \
   __after_node              = (before_node)->fld.previous;             \
   (the_node)->fld.previous  = __after_node;                            \
   __after_node->fld.next    = (the_node);                              \
   (the_node)->fld.next      = (before_node);                           \
   before_node->fld.previous = (the_node);                              \
} while (0)

#define CHAIN_REMOVE(the_node, type, fld) do {                          \
   CHAIN_TYPEOF(type) * __next;                                         \
   CHAIN_TYPEOF(type) * __previous;                                     \
                                                                        \
   __next           = (the_node)->fld.next;                             \
   __previous       = (the_node)->fld.previous;                         \
   __next->fld.previous = __previous;                                   \
   __previous->fld.next = __next;                                       \
} while (0)

#define CHAIN_EXTRACT_FIRST(the_chain, type, fld) CHAIN_FRST(the_list); \
do {                                                                    \
   CHAIN_TYPEOF(type) * __new_first;                                    \
   CHAIN_TYPEOF(type) * __the_first;                                    \
                                                                        \
   __the_first               = (the_chain)->first;                      \
   __new_first               = (the_first)->fld.next;                   \
   __the_chain->first        = __new_first;                             \
   __new_first->fld.previous = __the_first->fld.previous;               \
} while(0)

#define CHAIN_EXTRACT_LAST(the_chain, type, fld) CHAIN_LAST(the_list);  \
do {                                                                    \
   CHAIN_TYPEOF(type) * __new_last;                                     \
   CHAIN_TYPEOF(type) * __the_last;                                     \
                                                                        \
   __the_last           = (the_chain)->last;                            \
   __new_last           = (the_last)->fld.previous;                     \
   __the_chain->last    = __new_last;                                   \
   __new_last->fld.next = __the_last->fld.next;                         \
} while(0)

#define CHAIN_REPLACE(the_oldnode, the_newnode, type, fld) do {         \
   CHAIN_TYPEOF(type) * __next;                                         \
   CHAIN_TYPEOF(type) * __previous;                                     \
                                                                        \
   __next                      = (the_oldnode)->fld.next;               \
   __previous                  = (the_oldnode)->fld.previous;           \
   __next->fld.previous        = (the_newnode);                         \
   __previous->fld.next        = (the_newnode);                         \
   (the_newnode)->fld.next     = __next;                                \
   (the_newnode)->fld.previous = __previous;                            \
} while (0)

#define CHAIN_APPEND(the_list, the_node, type, fld)  do {               \
   CHAIN_TYPEOF(type) * __old_last_node;                                \
                                                                        \
   (the_node)->fld.next      = CHAIN_TAIL_NODE(the_list, type, fld);    \
   __old_last_node           = (the_list)->last;                        \
   (the_list)->last          = (the_node);                              \
   __old_last_node->fld.next = (the_node);                              \
   (the_node)->fld.previous  = __old_last_node;                         \
} while (0)

#define CHAIN_PREPPEND(the_list, the_node, type, fld)  do {             \
                                                                        \
   CHAIN_TYPEOF(type) * __old_frst_node;                                \
                                                                        \
   (the_node)->fld.previous  = CHAIN_HEAD_NODE(the_list, type, fld);    \
   __old_frst_node           = (the_list)->first;                       \
   (the_list)->first         = (the_node);                              \
   __old_frst_node->fld.previous = (the_node);                          \
   (the_node)->fld.next      = __old_frst_node;                         \
} while (0)

//////////////////////////////////////////////////////////////
#define CHAIN_EMPTY(the_list, fld)                                      \
   (!CHAIN_NEXT((CHAIN_FIRST(the_list)), (fld)))

#define CHAIN_FIRST(the_list) CHAIN_FRST(the_list)

#define CHAIN_INSERT_TAIL(the_list, the_node, type, fld)                \
      CHAIN_APPEND((the_list), (the_node), type, fld)

#define CHAIN_INSERT_HEAD(the_list, the_node, type, fld)                \
      CHAIN_PREPPEND((the_list), (the_node), type, fld)

#define CHAIN_FOREACH(the_node, the_list, fld)                          \
   for ((the_node) = CHAIN_FIRST(the_list);                             \
        CHAIN_NEXT((the_node), (fld));                                  \
        (the_node) = CHAIN_NEXT((the_node), (fld)))

#define CHAIN_FOREACH_SAFE(the_node, the_list, fld, the_next)           \
   for ((the_node) = CHAIN_FIRST(the_list);                             \
       ((the_next) = CHAIN_NEXT((the_node), fld));                      \
        (the_node) = (the_next))

#define CHAIN_FOREACH_REVERSE(the_node, the_list, fld)                  \
   for ((the_node) = CHAIN_LAST(the_list);                              \
        CHAIN_PREV((the_node), fld);                                    \
        (the_node) = CHAIN_PREV((the_node), fld))

#define CHAIN_FOREACH_REVERSE_SAFE(the_node, the_list, fld, the_prev)   \
   for ((the_node) = CHAIN_LAST(the_list);                              \
       ((the_prev) = CHAIN_PREV((the_node), fld));                      \
        (the_node) = (the_prev))
//////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////////
// special case: zero offseted node in structure, simplified macros                         //
//////////////////////////////////////////////////////////////////////////////////////////////
typedef struct _chain_node_t {
   struct _chain_node_t * next;
   struct _chain_node_t * previous;
} _chain_node_t;

typedef struct _chain_head_t {
   struct _chain_node_t * first;
   struct _chain_node_t * null_;
   struct _chain_node_t * last;
} _chain_head_t;

#define _CHAIN_ENTRY(type)                                              \
struct {                                                                \
   CHAIN_TYPEOF(type) * next;                                           \
   CHAIN_TYPEOF(type) * previous;                                       \
}

#define _CHAIN_HEAD(name, type)                                         \
struct name {                                                           \
   CHAIN_TYPEOF(type) * first;                                          \
   CHAIN_TYPEOF(type) * null_;                                          \
   CHAIN_TYPEOF(type) * last;                                           \
}

#define _CHAIN_INIT_EMPTY(_the_list)                                    \
{                                                                       \
   _chain_node_t * the_head, * the_tail;                                \
   _chain_head_t * the_list = (_chain_head_t *)(_the_list);             \
                                                                        \
   the_head = _CHAIN_HEAD_NODE(the_list, _chain_node_t);                \
   the_tail = _CHAIN_TAIL_NODE(the_list, _chain_node_t);                \
                                                                        \
   (the_list)->first = the_tail;                                        \
   (the_list)->last  = the_head;                                        \
   (the_list)->null_ = NULL;                                            \
}

#define _CHAIN_HEAD_NODE(the_list, type) ((CHAIN_TYPEOF(type) *)(the_list))
#define _CHAIN_TAIL_NODE(the_list, type) ((CHAIN_TYPEOF(type) *)(&((the_list)->null_)))

#define _CHAIN_FRST(the_list)  ((the_list)->first)
#define _CHAIN_LAST(the_list)  ((the_list)->last )
#define _CHAIN_NEXT(the_node)  ((void *)(((_chain_node_t *)(the_node))->next    ))
#define _CHAIN_PREV(the_node)  ((void *)(((_chain_node_t *)(the_node))->previous))

#define _CHAIN_IS_EMPTY(the_list) (((_chain_node_t *)((the_list)->first)) == _CHAIN_TAIL_NODE(the_list, _chain_node_t))

#define _CHAIN_INSERT_AFTER(_after_node, _the_node) do {                \
   _chain_node_t * after_node, * the_node, * before_node;               \
                                                                        \
   after_node                = (_chain_node_t *)(_after_node);          \
   the_node                  = (_chain_node_t *)(_the_node);            \
                                                                        \
   (before_node)             = (after_node)->next;                      \
   (the_node)->previous      = (after_node);                            \
   (after_node)->next        = (the_node);                              \
   (the_node)->next          = (before_node);                           \
   before_node->previous     = (the_node);                              \
} while (0)

#define _CHAIN_INSERT_BEFORE(_before_node, _the_node) do {              \
   _chain_node_t * after_node, * the_node, * before_node;               \
                                                                        \
   before_node               = (_chain_node_t *)(_before_node);         \
   the_node                  = (_chain_node_t *)(_the_node);            \
                                                                        \
   (after_node)              = (before_node)->previous;                 \
   (the_node)->previous      = (after_node);                            \
   (after_node)->next        = (the_node);                              \
   (the_node)->next          = (before_node);                           \
   before_node->previous     = (the_node);                              \
} while (0)

#define _CHAIN_REMOVE(_the_node) do {                                   \
                                                                        \
   _chain_node_t * next;                                                \
   _chain_node_t * previous;                                            \
   _chain_node_t * the_node = (_chain_node_t *)(_the_node);             \
                                                                        \
   next           = (the_node)->next;                                   \
   previous       = (the_node)->previous;                               \
   next->previous = previous;                                           \
   previous->next = next;                                               \
} while (0)

#define _CHAIN_REPLACE(_the_oldnode, _the_newnode) do {                 \
                                                                        \
   _chain_node_t * next;                                                \
   _chain_node_t * previous;                                            \
   _chain_node_t * the_oldnode = (_chain_node_t *)(_the_oldnode);       \
   _chain_node_t * the_newnode = (_chain_node_t *)(_the_newnode);       \
                                                                        \
   next                    = (the_oldnode)->next;                       \
   previous                = (the_oldnode)->previous;                   \
   next->previous          = the_newnode;                               \
   previous->next          = the_newnode;                               \
   (the_newnode)->next     = next;                                      \
   (the_newnode)->previous = previous;                                  \
} while (0)

#define _CHAIN_APPEND(_the_list, _the_node)  do {                       \
                                                                        \
   _chain_node_t * old_last_node;                                       \
   _chain_head_t * the_list = (_chain_head_t *)(_the_list);             \
   _chain_node_t * the_node = (_chain_node_t *)(_the_node);             \
                                                                        \
   (the_node)->next     = _CHAIN_TAIL_NODE(the_list, _chain_node_t);    \
   old_last_node        = (the_list)->last;                             \
   (the_list)->last     = the_node;                                     \
   old_last_node->next  = the_node;                                     \
   (the_node)->previous = old_last_node;                                \
} while (0)

#define _CHAIN_PREPPEND(_the_list, _the_node)  do {                     \
                                                                        \
   _chain_node_t * old_first_node;                                      \
   _chain_head_t * the_list = (_chain_head_t *)(_the_list);             \
   _chain_node_t * the_node = (_chain_node_t *)(_the_node);             \
                                                                        \
   (the_node)->previous = _CHAIN_HEAD_NODE(the_list, _chain_node_t);    \
   old_first_node       = (the_list)->first;                            \
   (the_list)->first    = the_node;                                     \
   old_first_node->previous = the_node;                                 \
   (the_node)->next     = old_first_node;                               \
} while (0)

#define _CHAIN_EXTRACT_FIRST(_the_list)                                 \
   (_CHAIN_IS_EMPTY(_the_list) ? (NULL) : (_the_list)->first);          \
                                                                        \
   if (!_CHAIN_IS_EMPTY(_the_list)) {                                   \
                                                                        \
       _chain_node_t * next;                                            \
       _chain_node_t * previous;                                        \
       _chain_node_t * the_node = (_chain_node_t *)((_the_list)->first);\
                                                                        \
       next           = (the_node)->next;                               \
       previous       = (the_node)->previous;                           \
       next->previous = previous;                                       \
       previous->next = next;                                           \
   } while (0)

#define _CHAIN_EXTRACT_LAST(_the_list)                                  \
   (((_chain_node_t *)(_the_list)->last)->previous ?                    \
      (_the_list)->last : NULL);                                        \
                                                                        \
   if (((_chain_node_t *)(_the_list)->last)->previous) {                \
                                                                        \
       _chain_node_t * next;                                            \
       _chain_node_t * previous;                                        \
       _chain_node_t * the_node = (_chain_node_t *)((_the_list)->last); \
                                                                        \
       next           = (the_node)->next;                               \
       previous       = (the_node)->previous;                           \
       next->previous = previous;                                       \
       previous->next = next;                                           \
   } while (0)

//////////////////////////////////////////////////////////////
#define _CHAIN_FOREACH(the_node, the_list, type)                        \
   for ((the_node) = ((CHAIN_TYPEOF(type) *)_CHAIN_FIRST(the_list));    \
        _CHAIN_NEXT(the_node);                                          \
        (the_node) = ((CHAIN_TYPEOF(type) *)_CHAIN_NEXT(the_node)))

#define _CHAIN_FOREACH_REVERSE(the_node, the_list, type)                \
   for ((the_node) = ((CHAIN_TYPEOF(type) *)_CHAIN_LAST(the_list));     \
        _CHAIN_PREV(the_node);                                          \
        (the_node) = ((CHAIN_TYPEOF(type) *)_CHAIN_PREV(the_node)))

#define _CHAIN_FOREACH_SAFE(the_node, the_list, type, the_next)         \
   for ((the_node) = ((CHAIN_TYPEOF(type) *)_CHAIN_FIRST(the_list));    \
       ((the_next) = ((CHAIN_TYPEOF(type) *)_CHAIN_NEXT(the_node)));    \
        (the_node) = (the_next))

#define _CHAIN_FOREACH_REVERSE_SAFE(the_node, the_list, type, the_prev) \
   for ((the_node) = ((CHAIN_TYPEOF(type) *)_CHAIN_LAST(the_list));     \
        (the_prev) = ((CHAIN_TYPEOF(type) *)_CHAIN_PREV(the_node));     \
        (the_node) = (the_prev))
//////////////////////////////////////////////////////////////
#define _CHAIN_INIT(the_list)     _CHAIN_INIT_EMPTY(the_list)
#define _CHAIN_EMPTY(the_list)    _CHAIN_IS_EMPTY(the_list)
#define _CHAIN_FIRST(the_list)    _CHAIN_FRST(the_list)

#define _CHAIN_INSERT_TAIL(the_list, the_node)                          \
      _CHAIN_APPEND((the_list), (the_node))

#define _CHAIN_INSERT_HEAD(the_list, the_node)                          \
      _CHAIN_PREPPEND((the_list), (the_node))
//////////////////////////////////////////////////////////////

#endif