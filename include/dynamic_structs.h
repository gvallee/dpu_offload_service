//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <ucs/datastruct/list.h>
#include <ucs/sys/compiler_def.h>

#ifndef DPU_OFFLOAD_DYNAMIC_LIST_H
#define DPU_OFFLOAD_DYNAMIC_LIST_H

/* Examples/tests are available in tests/dyn_structs */

#define DEFAULT_MEM_CHUNKS (1024)

typedef struct mem_chunk
{
    size_t size;
    void *ptr;
} mem_chunk_t;

typedef void (*dyn_struct_elt_init_fn)(void *);

/***************/
/* SIMPLE LIST */
/***************/

/**
 * @brief simple_list_t aims at offering an alternative to ucs_list_link_t
 * without the huge performance impact associated to them.
 */
typedef struct simple_list
{
    ucs_list_link_t internal_list;
    size_t length;
} simple_list_t;

#define SIMPLE_LIST_INIT(_simple_list_to_init)                        \
    do                                                                \
    {                                                                 \
        ucs_list_head_init(&((_simple_list_to_init)->internal_list)); \
        (_simple_list_to_init)->length = 0;                           \
    } while (0)

#define SIMPLE_LIST_PREPEND(_sl_append, _sl_elt)                    \
    do                                                              \
    {                                                               \
        ucs_list_add_head(&((_sl_append)->internal_list), _sl_elt); \
        (_sl_append)->length++;                                     \
    } while (0)

#define SIMPLE_LIST_DEL(__sl_del, _sl_item) \
    do                                      \
    {                                       \
        ucs_list_del(_sl_item);             \
        (__sl_del)->length--;               \
    } while (0)

#define SIMPLE_LIST_EXTRACT_HEAD(_sl_eh, _sl_type, _sl_item) ({                      \
    _sl_type *_sl_elt = NULL;                                                        \
    _sl_elt = ucs_list_extract_head(&((_sl_eh)->internal_list), _sl_type, _sl_item); \
    (_sl_eh)->length--;                                                              \
    _sl_elt;                                                                         \
})

#define SIMPLE_LIST_LENGTH(__sl_len) ({     \
    size_t __list_len = (__sl_len)->length; \
    __list_len;                             \
})

#define SIMPLE_LIST_IS_EMPTY(__sl) ({ \
    bool _sl_empty = false;           \
    if ((__sl)->length == 0)          \
        _sl_empty = true;             \
    _sl_empty;                        \
})

#define SIMPLE_LIST_FOR_EACH(__sl_cur, __sl_next, __sl, __elt) \
    ucs_list_for_each_safe(__sl_cur, __sl_next, &((__sl)->internal_list), __elt)

/*****************/
/* DYNAMIC ARRAY */
/*****************/

typedef struct dyn_array
{
    void *base;
    size_t capacity;
    size_t allocation_size;
    size_t type_size;
    dyn_struct_elt_init_fn element_init_fn;
} dyn_array_t;

#define DYN_ARRAY_ALLOC(_dyn_array_alloc, _da_alloc_num_elts_alloc, _da_alloc_type)                             \
    do                                                                                                          \
    {                                                                                                           \
        assert(_da_alloc_num_elts_alloc);                                                                       \
        (_dyn_array_alloc)->allocation_size = _da_alloc_num_elts_alloc;                                          \
        (_dyn_array_alloc)->capacity = _da_alloc_num_elts_alloc;                                                \
        (_dyn_array_alloc)->type_size = sizeof(_da_alloc_type);                                                 \
        (_dyn_array_alloc)->element_init_fn = NULL;                                                             \
        (_dyn_array_alloc)->base = malloc((_dyn_array_alloc)->allocation_size * (_dyn_array_alloc)->type_size);  \
        assert((_dyn_array_alloc)->base);                                                                       \
        memset((_dyn_array_alloc)->base, 0, (_dyn_array_alloc)->allocation_size *(_dyn_array_alloc)->type_size); \
    } while (0)

#define DYN_ARRAY_ALLOC_WITH_INIT_FN(_dyn_array, _num_elts_alloc, _type, _fn)      \
    do                                                                             \
    {                                                                              \
        size_t _da_alloc_x;                                                        \
        assert(_num_elts_alloc);                                                   \
        (_dyn_array)->allocation_size = _num_elts_alloc;                            \
        (_dyn_array)->capacity = _num_elts_alloc;                                  \
        (_dyn_array)->element_init_fn = _fn;                                       \
        (_dyn_array)->base = malloc(_num_elts_alloc * sizeof(_type));              \
        assert((_dyn_array)->base);                                                \
        memset((_dyn_array)->base, 0, _num_elts_alloc * sizeof(_type));            \
        _type *_da_alloc_a = (_type *)(_dyn_array)->base;                          \
        for (_da_alloc_x = 0; _da_alloc_x < (_dyn_array)->capacity; _da_alloc_x++) \
        {                                                                          \
            _fn(_da_alloc_a[_da_alloc_x]);                                         \
        }                                                                          \
    } while (0)

#define DYN_ARRAY_FREE(_dyn_array)                \
    do                                            \
    {                                             \
        if ((_dyn_array)->capacity > 0)           \
        {                                         \
            if ((_dyn_array)->base)               \
            {                                     \
                free((_dyn_array)->base);         \
                (_dyn_array)->base = NULL;        \
            }                                     \
            (_dyn_array)->base = NULL;            \
            (_dyn_array)->element_init_fn = NULL; \
            (_dyn_array)->capacity = 0;           \
        }                                         \
    } while (0)

#define DYN_ARRAY_GROW(_dyn_array, _type, _size)                                                        \
    do                                                                                                  \
    {                                                                                                   \
        size_t _initial_num_elts, _new_num_elts;                                                        \
        _initial_num_elts = _new_num_elts = (_dyn_array)->capacity;                                     \
        while (_new_num_elts * sizeof(_type) <= _size * sizeof(_type))                                  \
        {                                                                                               \
            _new_num_elts += (_dyn_array)->allocation_size;                                              \
        }                                                                                               \
        (_dyn_array)->base = realloc((_dyn_array)->base, _new_num_elts * sizeof(_type));                \
        assert((_dyn_array)->base);                                                                     \
        char *_start = (char *)(((ptrdiff_t)((_dyn_array)->base)) + _initial_num_elts * sizeof(_type)); \
        memset(_start, 0, (_new_num_elts - _initial_num_elts) * sizeof(_type));                         \
        if ((_dyn_array)->element_init_fn != NULL)                                                      \
        {                                                                                               \
            size_t _x;                                                                                  \
            _type *_a = (_type *)_start;                                                                \
            for (_x = 0; _x < _new_num_elts - _initial_num_elts; _x++)                                  \
            {                                                                                           \
                (_dyn_array)->element_init_fn(&(_a[_initial_num_elts + _x]));                           \
            }                                                                                           \
        }                                                                                               \
        (_dyn_array)->capacity = _new_num_elts;                                                         \
    } while (0)

#define DYN_ARRAY_GET_ELT(_dyn_array, _idx, _type) ({ \
    assert(_dyn_array);                               \
    assert((_dyn_array)->capacity);                   \
    _type *_elt = NULL;                               \
    if ((_dyn_array)->capacity <= _idx)               \
    {                                                 \
        DYN_ARRAY_GROW(_dyn_array, _type, _idx);      \
    }                                                 \
    _type *__ptr = (_type *)((_dyn_array)->base);     \
    _elt = (_type *)&(__ptr[_idx]);                   \
    _elt;                                             \
})

#define DYN_ARRAY_SET_ELT(_dyn_array, _idx, _type, _elt) \
    do                                                   \
    {                                                    \
        assert(_dyn_array);                              \
        if ((_dyn_array)->capacity >= _idx)              \
        {                                                \
            DYN_ARRAY_GROW(_dyn_array, _type, _idx);     \
        }                                                \
        _type *_ptr = (_type *)((_dyn_array)->base);     \
        memcpy(&(_ptr[_idx]), _elt, sizeof(_type));      \
    } while (0)

/****************/
/* DYNAMIC LIST */
/****************/

typedef struct dyn_list
{
    size_t capacity;
    size_t allocation_size;
    simple_list_t list;
    dyn_struct_elt_init_fn element_init_cb;
    // List of memory chunks used for the elements of the list
    size_t num_mem_chunks;
    dyn_array_t mem_chunks;
} dyn_list_t;

#if NDEBUG
#define GROW_DYN_LIST(__dyn_list, __type, __elt)                                                                                 \
    do                                                                                                                           \
    {                                                                                                                            \
        size_t _chunk_size = (__dyn_list)->allocation_size * sizeof(__type);                                                      \
        void *_new_chunk_buf = malloc(_chunk_size);                                                                              \
        if (_new_chunk_buf != NULL)                                                                                              \
        {                                                                                                                        \
            size_t _gdl_idx;                                                                                                     \
            mem_chunk_t *_chunk_ptr = DYN_ARRAY_GET_ELT(&((__dyn_list)->mem_chunks), (__dyn_list)->num_mem_chunks, mem_chunk_t); \
            _chunk_ptr->ptr = _new_chunk_buf;                                                                                    \
            _chunk_ptr->size = _chunk_size;                                                                                      \
            (__dyn_list)->num_mem_chunks++;                                                                                      \
            __type *_e = (__type *)(_new_chunk_buf);                                                                             \
            for (_gdl_idx = 0; _gdl_idx < (__dyn_list)->allocation_size; _gdl_idx++)                                              \
            {                                                                                                                    \
                if ((__dyn_list)->element_init_cb != NULL)                                                                       \
                {                                                                                                                \
                    (__dyn_list)->element_init_cb((void *)&(_e[_gdl_idx]));                                                      \
                }                                                                                                                \
                SIMPLE_LIST_PREPEND(&((__dyn_list)->list), &(_e[_gdl_idx].__elt));                                               \
            }                                                                                                                    \
            (__dyn_list)->capacity += (__dyn_list)->allocation_size;                                                              \
        }                                                                                                                        \
        else                                                                                                                     \
        {                                                                                                                        \
            fprintf(stderr, "[ERROR] unable to grow dynamic list, unable to allocate buffer\n");                                 \
        }                                                                                                                        \
    } while (0)
#else
#define GROW_DYN_LIST(__dyn_list, __type, __elt)                                                                                 \
    do                                                                                                                           \
    {                                                                                                                            \
        assert(__dyn_list);                                                                                                      \
        size_t _initial_list_size = SIMPLE_LIST_LENGTH(&((__dyn_list)->list));                                                   \
        size_t _chunk_size = (__dyn_list)->allocation_size * sizeof(__type);                                                      \
        void *_new_chunk_buf = malloc(_chunk_size);                                                                              \
        if (_new_chunk_buf != NULL)                                                                                              \
        {                                                                                                                        \
            size_t _gdl_idx;                                                                                                     \
            void *_ptr = NULL;                                                                                                   \
            mem_chunk_t *_chunk_ptr = DYN_ARRAY_GET_ELT(&((__dyn_list)->mem_chunks), (__dyn_list)->num_mem_chunks, mem_chunk_t); \
            assert(_chunk_ptr);                                                                                                  \
            _chunk_ptr->ptr = _new_chunk_buf;                                                                                    \
            _chunk_ptr->size = _chunk_size;                                                                                      \
            (__dyn_list)->num_mem_chunks++;                                                                                      \
            __type *_e = (__type *)(_new_chunk_buf);                                                                             \
            for (_gdl_idx = 0; _gdl_idx < (__dyn_list)->allocation_size; _gdl_idx++)                                              \
            {                                                                                                                    \
                if ((__dyn_list)->element_init_cb != NULL)                                                                       \
                {                                                                                                                \
                    (__dyn_list)->element_init_cb((void *)&(_e[_gdl_idx]));                                                      \
                }                                                                                                                \
                SIMPLE_LIST_PREPEND(&((__dyn_list)->list), &(_e[_gdl_idx].__elt));                                               \
                _ptr = &(_e[_gdl_idx]);                                                                                          \
            }                                                                                                                    \
            assert(SIMPLE_LIST_LENGTH(&((__dyn_list)->list)) == (_initial_list_size + (__dyn_list)->allocation_size));            \
            assert((((ptrdiff_t)_ptr + sizeof(__type)) - ((ptrdiff_t)_new_chunk_buf + _chunk_size)) == 0);                       \
            (__dyn_list)->capacity += (__dyn_list)->allocation_size;                                                              \
        }                                                                                                                        \
        else                                                                                                                     \
        {                                                                                                                        \
            fprintf(stderr, "[ERROR] unable to grow dynamic list, unable to allocate buffer\n");                                 \
        }                                                                                                                        \
    } while (0)
#endif

#define DYN_LIST_ALLOC(_dyn_list, _num_elts_alloc, _type, _elt)                           \
    do                                                                                    \
    {                                                                                     \
        (_dyn_list) = malloc(sizeof(dyn_list_t));                                         \
        if ((_dyn_list) != NULL)                                                          \
        {                                                                                 \
            DYN_ARRAY_ALLOC(&((_dyn_list)->mem_chunks), DEFAULT_MEM_CHUNKS, mem_chunk_t); \
            (_dyn_list)->num_mem_chunks = 0;                                              \
            (_dyn_list)->capacity = 0;                                                    \
            (_dyn_list)->allocation_size = _num_elts_alloc;                                \
            (_dyn_list)->element_init_cb = NULL;                                          \
            SIMPLE_LIST_INIT(&((_dyn_list)->list));                                       \
            GROW_DYN_LIST((_dyn_list), _type, _elt);                                      \
        }                                                                                 \
        else                                                                              \
        {                                                                                 \
            fprintf(stderr, "[ERROR] unable to allocate dynamic list\n");                 \
            (_dyn_list) = NULL;                                                           \
        }                                                                                 \
    } while (0)

#define DYN_LIST_ALLOC_WITH_INIT_CALLBACK(_dyn_list, _num_elts_alloc, _type, _elt, _cb)   \
    do                                                                                    \
    {                                                                                     \
        (_dyn_list) = malloc(sizeof(dyn_list_t));                                         \
        if ((_dyn_list) != NULL)                                                          \
        {                                                                                 \
            DYN_ARRAY_ALLOC(&((_dyn_list)->mem_chunks), DEFAULT_MEM_CHUNKS, mem_chunk_t); \
            (_dyn_list)->num_mem_chunks = 0;                                              \
            (_dyn_list)->capacity = 0;                                                    \
            (_dyn_list)->allocation_size = _num_elts_alloc;                                \
            (_dyn_list)->element_init_cb = _cb;                                           \
            SIMPLE_LIST_INIT(&((_dyn_list)->list));                                       \
            GROW_DYN_LIST((_dyn_list), _type, _elt);                                      \
        }                                                                                 \
        else                                                                              \
        {                                                                                 \
            fprintf(stderr, "[ERROR] unable to allocate dynamic list\n");                 \
            (_dyn_list) = NULL;                                                           \
        }                                                                                 \
    } while (0)

#if NDEBUG
#define DYN_LIST_FREE(_dyn_list, _type, _elt)                                           \
    do                                                                                  \
    {                                                                                   \
        /* Free the underlying memory chunks */                                         \
        size_t _i;                                                                      \
        for (_i = 0; _i < (_dyn_list)->num_mem_chunks; _i++)                            \
        {                                                                               \
            mem_chunk_t *_mem_chunk_ptr = DYN_ARRAY_GET_ELT(&((_dyn_list)->mem_chunks), \
                                                            _i,                         \
                                                            mem_chunk_t);               \
            assert(_mem_chunk_ptr);                                                     \
            free(_mem_chunk_ptr->ptr);                                                  \
            _mem_chunk_ptr->ptr = NULL;                                                 \
        }                                                                               \
        DYN_ARRAY_FREE(&((_dyn_list)->mem_chunks));                                     \
        free(_dyn_list);                                                                \
        _dyn_list = NULL;                                                               \
    } while (0)
#else
#define DYN_LIST_FREE(_dyn_list, _type, _elt)                                           \
    do                                                                                  \
    {                                                                                   \
        /* Release the list */                                                          \
        while (!SIMPLE_LIST_IS_EMPTY(&((_dyn_list)->list)))                             \
        {                                                                               \
            _type *_item = SIMPLE_LIST_EXTRACT_HEAD(&((_dyn_list)->list), _type, _elt); \
            assert(_item);                                                              \
        }                                                                               \
        /* Free the underlying memory chunks */                                         \
        size_t _i;                                                                      \
        for (_i = 0; _i < (_dyn_list)->num_mem_chunks; _i++)                            \
        {                                                                               \
            mem_chunk_t *_mem_chunk_ptr = DYN_ARRAY_GET_ELT(&((_dyn_list)->mem_chunks), \
                                                            _i,                         \
                                                            mem_chunk_t);               \
            assert(_mem_chunk_ptr);                                                     \
            free(_mem_chunk_ptr->ptr);                                                  \
            _mem_chunk_ptr->ptr = NULL;                                                 \
        }                                                                               \
        DYN_ARRAY_FREE(&((_dyn_list)->mem_chunks));                                     \
        free(_dyn_list);                                                                \
        _dyn_list = NULL;                                                               \
    } while (0)
#endif

#define DYN_LIST_GET(_dyn_list, _type, _elt, _item)                          \
    do                                                                       \
    {                                                                        \
        if (SIMPLE_LIST_IS_EMPTY(&((_dyn_list)->list)))                      \
        {                                                                    \
            GROW_DYN_LIST(_dyn_list, _type, _elt);                           \
        }                                                                    \
        _item = SIMPLE_LIST_EXTRACT_HEAD(&((_dyn_list)->list), _type, _elt); \
    } while (0)

#define DYN_LIST_RETURN(_dyn_list, _item, _elt)                    \
    do                                                             \
    {                                                              \
        SIMPLE_LIST_PREPEND(&((_dyn_list)->list), &(_item->_elt)); \
    } while (0)
int dynamic_list_return();

/*****************/
/* SMART BUFFERS */
/*****************/

struct smart_buffers;
struct smart_bucket;

typedef struct
{
    ucs_list_link_t super;

    // Pointer to the bucket the chunk belongs to
    struct smart_bucket *bucket;

    // Pointer to the parent chunk when a chunk is split into sub-chunk.
    // This is therefore used to recycle/recombine the chunks when necessary.
    // Ultimately, it can be used to track the hierachy of chunks that are
    // being used.
    void *parent_chunk;

    // When a chunk is split into smaller chunks, the smaller chunks keep a ring
    // amongst themselves so we can track if they are still being used or not and
    // therefore handle recycling, i.e., re-combining smaller chunks into a bigger
    // one.
    void *prev;

    void *base;

    size_t size;

    bool in_use;
} smart_chunk_t;

#define RESET_SMART_CHUNK(__sc)      \
    do                               \
    {                                \
        (__sc)->parent_chunk = NULL; \
        (__sc)->prev = NULL;         \
        (__sc)->base = NULL;         \
        (__sc)->size = 0;            \
        (__sc)->in_use = false;      \
    } while (0)

typedef struct smart_bucket
{
    // Pointer to the smart buffer system
    struct smart_buffers *sys;

    size_t initial_size;

    // Minimum buffer size in the bucket
    uint64_t min_size;

    // Maximum buffer size in the bucket
    uint64_t max_size;

    // Pool of smart chunks in the bucket, ready to be used (type: smart_chunk_t)
    simple_list_t pool;
} smart_bucket_t;

#define RESET_SMART_BUCKET(__sb)           \
    do                                     \
    {                                      \
        (__sb)->sys = NULL;                \
        (__sb)->initial_size = 0;          \
        (__sb)->min_size = 0;              \
        (__sb)->max_size = 0;              \
        SIMPLE_LIST_INIT(&((__sb)->pool)); \
    } while (0)

typedef struct smart_buffers
{
    // Number of buckets
    size_t num_buckets;

    // Everything needed to track the big chunks of memory based by the smart buffer system
    size_t num_base_mem_chunks;
    dyn_array_t base_mem_chunks;

    // List of buckets (type: size_t)
    dyn_array_t bucket_sizes;

    // List of buckets (type: smart_bucket_t)
    dyn_array_t buckets;

    // Pool of smart chunk descriptors (type: smart_chunk_t)
    dyn_list_t *smart_chunk_desc_pool;

} smart_buffers_t;

typedef struct
{
    size_t num_buckets;

    size_t *buckets;
} smart_buffers_info_t;

// Grow a given bucket by adding a chunk of memory equal to the initial one
#define SMART_BUFFS_GROW_BUCKET(__ptr)                                            \
    do                                                                            \
    {                                                                             \
        size_t _i;                                                                \
        void *_sb_new_mem_chunk = malloc((__ptr)->initial_size);                  \
        assert(_sb_new_chunk);                                                    \
        memset(_sb_new_mem_chunk, 0, (__ptr)->initial_size);                      \
        size_t __num_new_smart_chunks = (_ptr)->initial_size / (__ptr)->max_size; \
        mem_chunk_t *_new_mem_chunk;                                              \
        _new_mem_chunk = DYN_ARRAY_GET_ELT(&((__ptr)->sys->base_mem_chunks),      \
                                           (__ptr)->sys->num_base_mem_chunks,     \
                                           mem_chunk_t);                          \
        assert(_new_mem_chunk);                                                   \
        _new_mem_chunk->base = _sb_new_mem_chunk;                                 \
        _new_mem_chunk->size = (__ptr)->initial_size;                             \
        (__ptr)->sys->num_mem_chunks++;                                           \
        void *_base_ptr = _sb_new_mem_chunk;                                      \
        void *__prev = NULL;                                                      \
        smart_chunk_t *__first_chunk = NULL;                                      \
        for (_i = 0; _i < _num_new_chunks; _i++)                                  \
        {                                                                         \
            smart_chunk_t *_new_smart_chunk;                                      \
            DYN_LIST_GET(&((__bdest)->smart_chunk_desc_pool),                     \
                         smart_chunk_t,                                           \
                         super,                                                   \
                         _new_smart_chunk);                                       \
            assert(_new_smart_chunk);                                             \
            RESET_SMART_CHUNK(_new_smart_chunk);                                  \
            if (__first_chunk == NULL)                                            \
                __first_chunk = _new_smart_chunk;                                 \
            _new_smart_chunk->base = _base_ptr;                                   \
            _new_smart_chunk->size = (__ptr)->max_size;                           \
            _new_smart_chunk->prev = __prev;                                      \
            _new_smart_chunk->bucket = (struct smart_bucket *)(__ptr);            \
            SIMPLE_LIST_PREPEND(&((__ptr)->pool), &(_new_smart_chunk->super));    \
            _base_ptr = (void *)((ptrdiff_t)_base_ptr + (__ptr)->max_size);       \
            __prev = (void *)_new_smart_chunk;                                    \
        }                                                                         \
        /* Close the ring */                                                      \
        __first_chunk->prev = __prev;                                             \
    } while (0)

// Try to recycle a smart chunk to the parent chunk when applicable and possible
#define SMART_BUFFS_RECYCLE_SMART_CHUNK(__smart_chunk)                                        \
    do                                                                                        \
    {                                                                                         \
        bool _can_be_recycled = true;                                                         \
        if ((__smart_chunk)->in_use == false && (__smart_chunk->parent_chunk != NULL))        \
        {                                                                                     \
            /* Make sure all the sub-chunks are not in use */                                 \
            void *__prev = __smart_chunk->prev;                                               \
            while (__prev != __smart_chunk)                                                   \
            {                                                                                 \
                smart_chunk_t *__cur_sc = (smart_chunk_t *)__prev;                            \
                if (__cur_sc->in_use == true)                                                 \
                {                                                                             \
                    _can_be_recycled = false;                                                 \
                    break;                                                                    \
                }                                                                             \
                __prev = __cur_sc->prev;                                                      \
            }                                                                                 \
            if (_can_be_recycled == true)                                                     \
            {                                                                                 \
                /* Remove all the smaller chunk for the pool so they cannot be used */        \
                /* any longer. */                                                             \
                /* Make sure all the sub-chunks are not in use */                             \
                void *__prev = __smart_chunk->prev;                                           \
                assert(__smart_chunk->parent_chunk != NULL);                                  \
                smart_chunk_t *__parent_chunk = (smart_chunk_t *)__smart_chunk->parent_chunk; \
                assert(__parent_chunk->bucket != NULL);                                       \
                while (__prev != __smart_chunk)                                               \
                {                                                                             \
                    smart_chunk_t *__cur_sc = (smart_chunk_t *)__prev;                        \
                    assert(__cur_sc->in_use == false);                                        \
                    SIMPLE_LIST_DEL(&(__cur_sc->bucket->pool), &(__cur_sc->super));           \
                }                                                                             \
                /* Return the bigger chunk to its pool so it can be used */                   \
                __parent_chunk->in_use = false;                                               \
                SIMPLE_LIST_PREPEND(&(__parent_chunk->bucket->pool),                          \
                                    &(__parent_chunk->super));                                \
            }                                                                                 \
        }                                                                                     \
    } while (0)

// Take a smart chunk from a source bucket and create multiple smaller smart chunks
// for the destination bucket.
#define SMART_BUFFS_SPLIT_BUCKET(__bsrc, __bdest)                                                      \
    do                                                                                                 \
    {                                                                                                  \
        size_t _i;                                                                                     \
                                                                                                       \
        _num_new_smart_chunks = __bsrc->max_size / __bdest->max_size;                                  \
        /* Get the chunk from the source so we can divide it up */                                     \
        smart_chunk_t *_big_chunk_to_split;                                                            \
        SIMPLE_LIST_EXTRACT_HEAD(&((_bsrc)->pool),                                                     \
                                 smart_chunk_t,                                                        \
                                 super,                                                                \
                                 _big_chunk_to_split);                                                 \
        assert(_big_chunk_to_split);                                                                   \
        _big_chunk_to_split->in_use = true;                                                            \
        void *_sc_base_ptr = _big_chunk_to_split->base;                                                \
        for (_i = 0; _i < _num_new_smart_chunks; _i++)                                                 \
        {                                                                                              \
            smart_chunk_t *_new_smart_chunk;                                                           \
            DYN_LIST_GET(&((__bdest)->smart_chunk_desc_pool), smart_chunk_t, super, _new_smart_chunk); \
            assert(_new_smart_chunk);                                                                  \
            RESET_SMART_CHUNK(_new_smart_chunk);                                                       \
            _new_smart_chunk->parent_chunk = _big_chunk_to_split;                                      \
            _new_smart_chunk->base = _sc_base_ptr;                                                     \
            _new_smart_chunk->size = __bdest->max_size;                                                \
            SIMPLE_LIST_EXTRACT_HEAD(&((__bdest)->pool), &(_new_smart_chunk->super));                  \
            _sc_base_ptr = (void *)((ptrdiff_t)_sc_base_ptr + _bdest->max_size);                       \
        }                                                                                              \
    } while (0)

#define SMART_BUFFERS_DEFAULT_MEM_ALLOC_SIZE ((20 * 1024 * 1024))

#define SMART_BUFFERS_ALLOC_SMART_CHUNKS(__sb_sys, _smart_bucket_to_populate, __bucket_allocated_mem_size, __sc_ptr_base, __num_chunks) \
    do                                                                                                                                  \
    {                                                                                                                                   \
        size_t __sb_sc_idx;                                                                                                             \
        for (__sb_sc_idx = 0; __sb_sc_idx < (__bucket_allocated_mem_size / _smart_bucket_to_populate->max_size); __sb_sc_idx++)         \
        {                                                                                                                               \
            void *__sb_sc_mem_ptr = (void *)((ptrdiff_t)__sc_ptr_base + __sb_sc_idx * _smart_bucket_to_populate->max_size);             \
            smart_chunk_t *_smart_chunk;                                                                                                \
            DYN_LIST_GET((__sb_sys)->smart_chunk_desc_pool, smart_chunk_t, super, _smart_chunk);                                        \
            assert(_smart_chunk);                                                                                                       \
            RESET_SMART_CHUNK(_smart_chunk);                                                                                            \
            _smart_chunk->base = __sb_sc_mem_ptr;                                                                                       \
            _smart_chunk->size = _smart_bucket_to_populate->max_size;                                                                   \
            _smart_chunk->bucket = (struct smart_bucket *)_smart_bucket_to_populate;                                                    \
            SIMPLE_LIST_PREPEND(&((_smart_bucket_to_populate)->pool), &(_smart_chunk->super));                                          \
            __num_chunks++;                                                                                                             \
        }                                                                                                                               \
    } while (0)

#define SMART_BUFFS_HANDLE_INFO(__smart_bufs_sys, _info_in)                                                                     \
    do                                                                                                                          \
    {                                                                                                                           \
        if (_info_in == NULL)                                                                                                   \
        {                                                                                                                       \
            /* Default buckets, sizes in bytes */                                                                               \
            size_t _smart_bufs_default_buckets[10] = {8, 16, 64, 128, 512, 1024, 4096, 8192, 65536, 1048576};                   \
            size_t _info_n_bucket;                                                                                              \
            /* Copy meta-data from info object */                                                                               \
            (__smart_bufs_sys)->num_buckets = 10;                                                                               \
            DYN_ARRAY_ALLOC(&((__smart_bufs_sys)->bucket_sizes), (__smart_bufs_sys)->num_buckets, size_t);                      \
            for (_info_n_bucket = 0; _info_n_bucket < 10; _info_n_bucket++)                                                     \
            {                                                                                                                   \
                size_t *_bucket_max_size = DYN_ARRAY_GET_ELT(&((__smart_bufs_sys)->bucket_sizes), _info_n_bucket, size_t);      \
                *_bucket_max_size = _smart_bufs_default_buckets[_info_n_bucket];                                                \
            }                                                                                                                   \
        }                                                                                                                       \
        else                                                                                                                    \
        {                                                                                                                       \
            size_t _info_copy_n_bucket;                                                                                         \
            /* Copy meta-data from info object */                                                                               \
            smart_buffers_info_t *__info_in = (smart_buffers_info_t *)(_info_in);                                               \
            (__smart_bufs_sys)->num_buckets = (__info_in)->num_buckets;                                                         \
            DYN_ARRAY_ALLOC(&((__smart_bufs_sys)->bucket_sizes), (__smart_bufs_sys)->num_buckets, size_t);                      \
            for (_info_copy_n_bucket = 0; _info_copy_n_bucket < (__smart_bufs_sys)->num_buckets; _info_copy_n_bucket++)         \
            {                                                                                                                   \
                size_t *_bucket_max_size = DYN_ARRAY_GET_ELT(&((__smart_bufs_sys)->bucket_sizes), _info_copy_n_bucket, size_t); \
                *_bucket_max_size = (__info_in)->buckets[_info_copy_n_bucket];                                                  \
            }                                                                                                                   \
        }                                                                                                                       \
    } while (0)

#define SMART_BUFFS_INIT(__smart_bufs_sys, _info_in)                                                                           \
    do                                                                                                                         \
    {                                                                                                                          \
        size_t _n_bucket;                                                                                                      \
        uint64_t _bucket_min_size = 0;                                                                                         \
        SMART_BUFFS_HANDLE_INFO(__smart_bufs_sys, _info_in);                                                                   \
        /* Initialize internals */                                                                                             \
        DYN_ARRAY_ALLOC(&((__smart_bufs_sys)->buckets), (__smart_bufs_sys)->num_buckets + 1, smart_bucket_t);                  \
        /* Allocate the base memory chunk that will be used to populate the buckets */                                         \
        DYN_ARRAY_ALLOC(&((__smart_bufs_sys)->base_mem_chunks), 8, mem_chunk_t);                                               \
        /* Allocate the pool of descriptors */                                                                                 \
        size_t __n_descs = SMART_BUFFERS_DEFAULT_MEM_ALLOC_SIZE / 2;                                                           \
        DYN_LIST_ALLOC((__smart_bufs_sys)->smart_chunk_desc_pool, __n_descs, smart_chunk_t, super);                            \
        mem_chunk_t *_base_mem_chunk = DYN_ARRAY_GET_ELT(&((__smart_bufs_sys)->base_mem_chunks), 0, mem_chunk_t);              \
        _base_mem_chunk->ptr = calloc(SMART_BUFFERS_DEFAULT_MEM_ALLOC_SIZE, sizeof(char));                                     \
        _base_mem_chunk->size = SMART_BUFFERS_DEFAULT_MEM_ALLOC_SIZE;                                                          \
        (__smart_bufs_sys)->num_base_mem_chunks = 1;                                                                           \
        size_t _bucket_mem_size = SMART_BUFFERS_DEFAULT_MEM_ALLOC_SIZE / (__smart_bufs_sys)->num_buckets;                      \
        for (_n_bucket = 0; _n_bucket < (__smart_bufs_sys)->num_buckets; _n_bucket++)                                          \
        {                                                                                                                      \
            size_t *_bucket_max_size = DYN_ARRAY_GET_ELT(&((__smart_bufs_sys)->bucket_sizes), _n_bucket, size_t);              \
            assert(_bucket_max_size);                                                                                          \
            smart_bucket_t *_smart_bucket = DYN_ARRAY_GET_ELT(&((__smart_bufs_sys)->buckets), _n_bucket, smart_bucket_t);      \
            assert(_smart_bucket);                                                                                             \
            RESET_SMART_BUCKET(_smart_bucket);                                                                                 \
            _smart_bucket->min_size = (uint64_t)_bucket_min_size;                                                              \
            _smart_bucket->max_size = (uint64_t)(*_bucket_max_size);                                                           \
            _bucket_min_size = (uint64_t)((*_bucket_max_size) + 1);                                                            \
            /* Add the smart chunks */                                                                                         \
            size_t _sb_allocated_chunks = 0;                                                                                   \
            void *_sc_alloc_ptr_base = (void *)((ptrdiff_t)_base_mem_chunk->ptr + _n_bucket * _bucket_mem_size);               \
            SMART_BUFFERS_ALLOC_SMART_CHUNKS(__smart_bufs_sys,                                                                 \
                                             _smart_bucket,                                                                    \
                                             _bucket_mem_size,                                                                 \
                                             _sc_alloc_ptr_base,                                                               \
                                             _sb_allocated_chunks);                                                            \
            _smart_bucket->initial_size = _sb_allocated_chunks;                                                                \
        }                                                                                                                      \
        /* Initialize last bucket */                                                                                           \
        smart_bucket_t *_last_smart_bucket = DYN_ARRAY_GET_ELT(&((__smart_bufs_sys)->buckets), _n_bucket, smart_bucket_t);     \
        assert(_last_smart_bucket);                                                                                            \
        _last_smart_bucket->min_size = (uint64_t)_bucket_min_size;                                                             \
        _last_smart_bucket->max_size = UINT64_MAX; /* The last bucket does not have a known size and is populated on demand */ \
    } while (0)

#define SMART_BUFF_GET(__sys, __sz)                                                                       \
    ({                                                                                                    \
        size_t _i;                                                                                        \
        smart_bucket_t *_target_bucket = NULL;                                                            \
        smart_chunk_t *_target_chunk = NULL;                                                              \
        for (_i = 0; _i < (__sys)->num_buckets; _i++)                                                     \
        {                                                                                                 \
            smart_bucket_t *__b = DYN_ARRAY_GET_ELT(&((__sys)->buckets), _i, smart_bucket_t);             \
            assert(__b);                                                                                  \
            if ((__b)->min_size <= (__sz) && (__sz) <= (__b)->max_size)                                   \
            {                                                                                             \
                _target_bucket = __b;                                                                     \
                break;                                                                                    \
            }                                                                                             \
        }                                                                                                 \
        if (_target_bucket == NULL || _target_bucket->max_size == UINT64_MAX)                             \
        {                                                                                                 \
            /* Need to create a new bucket, the size is bigger than anything we handle right now */       \
            /* We take the larger size from the next size aligned on a page or twice the maximum size */  \
            /* of the previous bucket */                                                                  \
            size_t *__last_bucket_sz = DYN_ARRAY_GET_ELT(&((__sys)->bucket_sizes),                        \
                                                         (__sys)->num_buckets - 1,                        \
                                                         size_t);                                         \
            uint64_t _new_bucket_max_size = (((__sz) / 4096) + 1) * 4096;                                 \
            if (_new_bucket_max_size < *__last_bucket_sz * 2)                                             \
                _new_bucket_max_size = *__last_bucket_sz * 2;                                             \
            size_t *__new_bucket_sz;                                                                      \
            smart_bucket_t *__new_bucket;                                                                 \
            mem_chunk_t *_mem_chunk_desc;                                                                 \
            __new_bucket_sz = DYN_ARRAY_GET_ELT(&((__sys)->bucket_sizes), (__sys)->num_buckets, size_t);  \
            assert(__new_bucket_sz);                                                                      \
            *__new_bucket_sz = _new_bucket_max_size;                                                      \
            __new_bucket = DYN_ARRAY_GET_ELT(&((__sys)->buckets), (__sys)->num_buckets, smart_bucket_t);  \
            assert(__new_bucket);                                                                         \
            SIMPLE_LIST_INIT(&(__new_bucket->pool));                                                      \
            __new_bucket->min_size = (uint64_t)(*__last_bucket_sz + 1);                                   \
            __new_bucket->max_size = _new_bucket_max_size;                                                \
            _mem_chunk_desc = DYN_ARRAY_GET_ELT(&((__sys)->base_mem_chunks),                              \
                                                (__sys)->num_base_mem_chunks,                             \
                                                mem_chunk_t);                                             \
            assert(_mem_chunk_desc);                                                                      \
            /* Calculate the total number of memory for the bucket */                                     \
            size_t _bucket_mem_size = 2 * _new_bucket_max_size;                                           \
            _mem_chunk_desc->ptr = calloc(_bucket_mem_size, sizeof(char));                                \
            (__sys)->num_base_mem_chunks++;                                                               \
            /* Add the smart chunks */                                                                    \
            size_t _j, __n = 0;                                                                           \
            for (_j = 0; _j <= (_bucket_mem_size / __new_bucket->max_size); _j++)                         \
            {                                                                                             \
                void *_mem_ptr = (void *)((ptrdiff_t)_mem_chunk_desc->ptr + _j * __new_bucket->max_size); \
                smart_chunk_t *_smart_chunk;                                                              \
                DYN_LIST_GET((__sys)->smart_chunk_desc_pool, smart_chunk_t, super, _smart_chunk);         \
                assert(_smart_chunk);                                                                     \
                RESET_SMART_CHUNK(_smart_chunk);                                                          \
                _smart_chunk->base = _mem_ptr;                                                            \
                _smart_chunk->size = __new_bucket->max_size;                                              \
                SIMPLE_LIST_PREPEND(&(__new_bucket->pool), &(_smart_chunk->super));                       \
                __n++;                                                                                    \
            }                                                                                             \
            __new_bucket->initial_size = __n;                                                             \
            (__sys)->num_buckets++;                                                                       \
            _target_bucket = __new_bucket;                                                                \
        }                                                                                                 \
        assert(_target_bucket);                                                                           \
        _target_chunk = SIMPLE_LIST_EXTRACT_HEAD(&(_target_bucket->pool), smart_chunk_t, super);          \
        _target_chunk->in_use = true;                                                                     \
        _target_chunk;                                                                                    \
    })

#define SMART_BUFF_RETURN(__sys, __sz, __sc) ({                                                     \
    size_t _sb_return_i;                                                                            \
    smart_bucket_t *_target_bucket = NULL;                                                          \
    assert(__sc->in_use == true);                                                                   \
    /* Find the associated bucket */                                                                \
    for (_sb_return_i = 0; _sb_return_i < (__sys)->num_buckets; _sb_return_i++)                     \
    {                                                                                               \
        smart_bucket_t *__b = DYN_ARRAY_GET_ELT(&((__sys)->buckets), _sb_return_i, smart_bucket_t); \
        assert(__b);                                                                                \
        if ((__b)->min_size <= __sz && __sz <= (__b)->max_size)                                     \
        {                                                                                           \
            _target_bucket = __b;                                                                   \
            break;                                                                                  \
        }                                                                                           \
    }                                                                                               \
    assert(_target_bucket);                                                                         \
    __sc->in_use = false;                                                                           \
    SIMPLE_LIST_PREPEND(&(_target_bucket->pool), &(__sc->super));                                   \
    /* Check if it makes sense to try to recycle smart buffers */                                   \
    size_t _recycle_threshold = _target_bucket->initial_size + _target_bucket->initial_size / 2;    \
    if (SIMPLE_LIST_LENGTH(&(_target_bucket->pool)) > _recycle_threshold)                           \
        SMART_BUFFS_RECYCLE_SMART_CHUNK(__sc);                                                      \
    /* Make sure the pointer cannot be used anymore */                                              \
    __sc = NULL;                                                                                    \
})

#define SMART_BUFFS_FINI(__sys)                                                                   \
    do                                                                                            \
    {                                                                                             \
        size_t __i;                                                                               \
        /* Return all the smart chunks */                                                         \
        for (__i = 0; __i < (__sys)->num_buckets; __i++)                                          \
        {                                                                                         \
            smart_bucket_t *__sb = DYN_ARRAY_GET_ELT(&((__sys)->buckets), __i, smart_bucket_t);   \
            assert(__sb);                                                                         \
            while (!SIMPLE_LIST_IS_EMPTY(&(__sb->pool)))                                          \
            {                                                                                     \
                smart_chunk_t *__sc;                                                              \
                __sc = SIMPLE_LIST_EXTRACT_HEAD(&(__sb->pool), smart_chunk_t, super);             \
                if (__sc->parent_chunk == NULL)                                                   \
                {                                                                                 \
                    DYN_LIST_RETURN((__sys)->smart_chunk_desc_pool, __sc, super);                 \
                }                                                                                 \
                else                                                                              \
                {                                                                                 \
                    SIMPLE_LIST_PREPEND(&(__sb->pool), &(__sc->super));                           \
                    SMART_BUFFS_RECYCLE_SMART_CHUNK(__sc);                                        \
                }                                                                                 \
            }                                                                                     \
        }                                                                                         \
        /* Free all the structures */                                                             \
        DYN_LIST_FREE((__sys)->smart_chunk_desc_pool, smart_chunk_t, super);                      \
        DYN_ARRAY_FREE(&((__sys)->buckets));                                                      \
        DYN_ARRAY_FREE(&((__sys)->bucket_sizes));                                                 \
        for (__i = 0; __i < (__sys)->num_base_mem_chunks; __i++)                                  \
        {                                                                                         \
            mem_chunk_t *__mc = DYN_ARRAY_GET_ELT(&((__sys)->base_mem_chunks), __i, mem_chunk_t); \
            assert(__mc);                                                                         \
            free(__mc->ptr);                                                                      \
        }                                                                                         \
        DYN_ARRAY_FREE(&((__sys)->base_mem_chunks));                                              \
    } while (0)

/*********************************************************************************************/
/* SMART ARRAYS: SIMILAR TO DYN_ARRAY BUT BASED ON SMART_BUFFERS. ALSO BASED ON SIZES RATHER */
/* THAN TYPES                                                                                */
/*********************************************************************************************/

typedef struct smart_array
{
    void *base;
    size_t capacity;
    size_t allocation_size;
    size_t elt_size;
    // Underlying smart chunk used to provide the buffer
    smart_chunk_t *smart_chunk;
} smart_array_t;

#define SMART_ARRAY_ALLOC(_smart_buffer_sys, _smart_array_alloc, _sa_alloc_num_elts_alloc, _sa_alloc_elt_sz)                       \
    do                                                                                                                             \
    {                                                                                                                              \
        assert(_sa_alloc_num_elts_alloc);                                                                                          \
        (_smart_array_alloc)->allocation_size = _sa_alloc_num_elts_alloc;                                                           \
        (_smart_array_alloc)->capacity = _sa_alloc_num_elts_alloc;                                                                 \
        (_smart_array_alloc)->elt_size = _sa_alloc_elt_sz;                                                                         \
        (_smart_array_alloc)->smart_chunk = SMART_BUFF_GET(_smart_buffer_sys,                                                      \
                                                           (_smart_array_alloc)->allocation_size * (_smart_array_alloc)->elt_size); \
        (_smart_array_alloc)->base = (_smart_array_alloc)->smart_chunk->base;                                                      \
        assert((_smart_array_alloc)->base);                                                                                        \
        memset((_smart_array_alloc)->base, 0, (_smart_array_alloc)->allocation_size *(_smart_array_alloc)->elt_size);               \
    } while (0)

#define SMART_ARRAY_FREE(_smart_buffer_sys, _smart_array)                           \
    do                                                                              \
    {                                                                               \
        assert((_smart_array)->base);                                               \
        assert((_smart_array)->smart_chunk);                                        \
        SMART_BUFF_RETURN(_smart_buffer_sys,                                        \
                          (_smart_array)->allocation_size *(_smart_array)->elt_size, \
                          (_smart_array)->smart_chunk);                             \
        assert((_smart_array)->smart_chunk == NULL);                                \
        (_smart_array)->base = NULL;                                                \
        (_smart_array)->element_init_fn = NULL;                                     \
        (_smart_array)->elt_size = 0;                                               \
    } while (0)

#define SMART_ARRAY_GROW(_smart_buffer_sys, _smart_array, _requested_new_elts)                                    \
    do                                                                                                            \
    {                                                                                                             \
        size_t _initial_num_elts, _new_num_elts;                                                                  \
        _initial_num_elts = _new_num_elts = (_dyn_array)->capacity;                                               \
        while (_new_num_elts * (_smart_array)->elt_size <= _requested_new_elts * (_smart_array)->elt_size)        \
        {                                                                                                         \
            _new_num_elts += (_smart_array)->allocation_size;                                                      \
        }                                                                                                         \
        /* Because of the nature of the smart buffers, the only option is to request a new bigger chunk of */     \
        /* memory and copy the data */                                                                            \
        smart_chunk_t *_new_smart_chunk;                                                                          \
        _new_smart_chunk = SMART_BUFF_GET((_smart_buffer_sys), _new_num_elts * (_smart_array)->elt_size);         \
        assert(_new_smart_chunk);                                                                                 \
        memcpy(_new_smart_chunk->base, (_smart_array)->smart_chunk->base, (_smart_array)->smart_chunk->max_size); \
        SMART_BUFF_RETURN((_smart_buffer_sys),                                                                    \
                          (_smart_array)->allocation_size *(_smart_array)->elt_size,                               \
                          (_smart_array)->smart_chunk);                                                           \
        (_smart_array)->smart_chunk = _new_smart_chunk;                                                           \
        (_smart_array)->base = (_smart_array)->smart_chunk->base;                                                 \
        (_smart_array)->capacity = _new_num_elts;                                                                 \
    } while (0)

#define SMART_ARRAY_GET_ELT(_smart_buffer_sys, _sa_get, _sa_get_idx) ({                \
    assert(_sa_get);                                                                   \
    assert((_sa_get)->capacity);                                                       \
    void *_ptr = NULL;                                                                 \
    if ((_sa_get)->capacity <= (_sa_get_idx))                                          \
    {                                                                                  \
        SMART_ARRAY_GROW((_smart_buffer_sys), (_sa_get), (_sa_get_idx));               \
    }                                                                                  \
    _ptr = (void *)((ptrdiff_t)(_sa_get)->base + (_sa_get_idx) * (_sa_get)->elt_size); \
    _ptr;                                                                              \
})

/****************************************************************/
/* SMART LISTS: SIMILAR TO DYN_LIST BUT BASED ON SMART_BUFFERS. */
/****************************************************************/

typedef struct smart_list
{
    size_t capacity;
    size_t allocation_size;
    size_t elt_size;
    // Smart chunk used to store the object of the smart list itself
    // When initializing, the smart list buffer is invalid and during
    // initialization, we grab a smart chunk and use it to store this
    // type, the update this pointer to be able to return it when the
    // smart list is freed.
    smart_chunk_t *base_smart_chunk;
    simple_list_t list;
    // Array of smart chunks used for the elements of the list
    size_t num_smart_chunks;
    // Array of pointers of pointers of smart chunks, the smart chunks
    // themselves come from the smart buffer system (type: smart_chunk_t*)
    smart_array_t smart_chunks;
} smart_list_t;

#define SMART_LIST_GROW(__smart_buf_sys, __smart_list, __type, __elt)                                      \
    do                                                                                                     \
    {                                                                                                      \
        assert(__smart_buf_sys);                                                                           \
        assert(__smart_list);                                                                              \
        size_t _initial_list_size = SIMPLE_LIST_LENGTH(&((__smart_list)->list));                           \
        /* Get a new buffer from the smart buffer system, no allocation */                                 \
        size_t _chunk_size = (__smart_list)->allocation_size * (__smart_list)->elt_size;                    \
        smart_chunk_t *_new_smart_chunk = (smart_chunk_t *)SMART_BUFF_GET((__smart_buf_sys), _chunk_size); \
        if (_new_smart_chunk != NULL)                                                                      \
        {                                                                                                  \
            size_t _i;                                                                                     \
            void *_ptr;                                                                                    \
            /* Save the pointer to that chunk so we can return it when we free the smart list */           \
            smart_chunk_t **_chunk_ptr;                                                                    \
            _chunk_ptr = (smart_chunk_t **)SMART_ARRAY_GET_ELT(&((__smart_list)->mem_chunks),              \
                                                               (__smart_list)->num_mem_chunks,             \
                                                               smart_chunk_t *);                           \
            assert(_chunk_ptr);                                                                            \
            *_chunk_ptr = _new_smart_chunk;                                                                \
            (__smart_list)->num_mem_chunks++;                                                              \
            /* Based on the new smart chunk, use the associated buffer to add elements to the list */      \
            __type *_e = (__type *)(_new_smart_chunk->base);                                               \
            for (_i = 0; _i < (__smart_list)->allocation_size; _i++)                                        \
            {                                                                                              \
                SIMPLE_LIST_PREPEND(&((__smart_list)->list), &(_e[_i].__elt));                             \
                _ptr = &(_e[_i]);                                                                          \
            }                                                                                              \
            assert(SIMPLE_LIST_LENGTH(&((__smart_list)->list)) ==                                          \
                   (_initial_list_size + (__smart_list)->allocation_size));                                 \
            /* All the new elements are added to the list, update the number of elements in the list*/     \
            (__smart_list)->capacity += (__smart_list)->allocation_size;                                    \
        }                                                                                                  \
        else                                                                                               \
        {                                                                                                  \
            fprintf(stderr, "[ERROR] unable to grow smart list, unable to allocate buffer\n");             \
        }                                                                                                  \
    } while (0)

#define SMART_LIST_ALLOC(_smart_buf_sys, _smart_list, _num_elts_alloc, _type, _elt)               \
    do                                                                                            \
    {                                                                                             \
        /* Get a small buffer to store the smart_list_t object */                                 \
        smart_chunk_t *_list_smart_chunk = SMART_BUFF_GET(_smart_buf_sys, sizeof(smart_list_t));  \
        (_smart_list) = (smart_list_t *)(_list_smart_chunk->base);                                \
        /* Cycle of pointers so we can track that initial smart chunk to be able to return it */  \
        /* when the smart list is freed. */                                                       \
        (_smart_list)->base_smart_chunk = _list_smart_chunk;                                      \
        if ((_smart_list) != NULL)                                                                \
        {                                                                                         \
            /* Create a smart array of pointer of pointer to smart_chunk_t so we can track the */ \
            /* smart chunks used to create the list. */                                           \
            SMART_ARRAY_ALLOC((_smart_buf_sys),                                                   \
                              &((_smart_list)->smart_chunks),                                     \
                              DEFAULT_MEM_CHUNKS,                                                 \
                              smart_chunk_t *);                                                   \
            (_dyn_list)->num_smart_chunks = 0;                                                    \
            (_dyn_list)->capacity = 0;                                                            \
            (_dyn_list)->allocation_size = _num_elts_alloc;                                        \
            (_dyn_list)->element_init_cb = NULL;                                                  \
            SIMPLE_LIST_INIT(&((_dyn_list)->list));                                               \
            GROW_DYN_LIST((_dyn_list), _type, _elt);                                              \
        }                                                                                         \
        else                                                                                      \
        {                                                                                         \
            fprintf(stderr, "[ERROR] unable to allocate dynamic list\n");                         \
            (_dyn_list) = NULL;                                                                   \
        }                                                                                         \
    } while (0)

#define SMART_LIST_FREE(_smart_buf_sys, _smart_list, _type, _elt)                                \
    do                                                                                           \
    {                                                                                            \
        /* Release the list, not the underlying buffers */                                       \
        while (!SIMPLE_LIST_IS_EMPTY(&((_smart_list)->list)))                                    \
        {                                                                                        \
            _type *_item = SIMPLE_LIST_EXTRACT_HEAD(&((_smart_list)->list), _type, _elt);        \
            assert(_item);                                                                       \
            SIMPLE_LIST_DEL(&((_smart_list)->list), &(_item->_elt));                             \
        }                                                                                        \
        /* Free the underlying memory chunks */                                                  \
        size_t _i;                                                                               \
        for (_i = 0; _i < (_smart_list)->num_mem_chunks; _i++)                                   \
        {                                                                                        \
            smart_chunk_t **_smart_chunk_ptr = DYN_ARRAY_GET_ELT(&((_smart_list)->smart_chunks), \
                                                                 _i,                             \
                                                                 smart_chunk_t *);               \
            assert(_smart_chunk_ptr);                                                            \
            SMART_BUFF_RETURN((_smart_buf_sys), sizeof(smart_chunk_t *), (*_smart_chunk_ptr));   \
            *_smart_chunk_ptr = NULL;                                                            \
        }                                                                                        \
        DYN_ARRAY_FREE(&((_smart_list)->smart_chunks));                                          \
        SMART_BUFF_RETURN((_smart_buf_sys), (_smart_list)->base_smart_chunk);                    \
        _smart_list = NULL;                                                                      \
    } while (0)

#define SMART_LIST_GET(_smart_buf_sys, _smart_list, _type, _elt, _item)        \
    do                                                                         \
    {                                                                          \
        if (SIMPLE_LIST_IS_EMPTY(&((_smart_list)->list)))                      \
        {                                                                      \
            SMART_LIST_GROW((_smart_buf_sys), (_smart_list), (_type), (_elt)); \
        }                                                                      \
        _item = SIMPLE_LIST_EXTRACT_HEAD(&((_dyn_list)->list), _type, _elt);   \
    } while (0)

#define SMART_LIST_RETURN(_smart_list, _item, _elt)                  \
    do                                                               \
    {                                                                \
        SIMPLE_LIST_PREPEND(&((_smart_list)->list), &(_item->_elt)); \
    } while (0)

#endif // DPU_OFFLOAD_DYNAMIC_LIST_H
