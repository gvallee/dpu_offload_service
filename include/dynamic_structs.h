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

#include <ucs/datastruct/list.h>

#ifndef DPU_OFFLOAD_DYNAMIC_LIST_H
#define DPU_OFFLOAD_DYNAMIC_LIST_H

#define DEFAULT_MEM_CHUNKS (1024)

typedef struct mem_chunk
{
    size_t size;
    void *ptr;
} mem_chunk_t;

typedef void (*dyn_struct_elt_init_fn)(void *);

/*****************/
/* DYNAMIC ARRAY */
/*****************/

typedef struct dyn_array
{
    void *base;
    size_t num_elts;
    size_t num_elts_alloc;
    dyn_struct_elt_init_fn element_init_fn;
} dyn_array_t;

#define DYN_ARRAY_ALLOC(_dyn_array, _num_elts_alloc, _type)             \
    do                                                                  \
    {                                                                   \
        assert(_num_elts_alloc);                                        \
        (_dyn_array)->num_elts_alloc = _num_elts_alloc;                 \
        (_dyn_array)->num_elts = _num_elts_alloc;                       \
        (_dyn_array)->element_init_fn = NULL;                           \
        (_dyn_array)->base = malloc(_num_elts_alloc * sizeof(_type));   \
        assert((_dyn_array)->base);                                     \
        memset((_dyn_array)->base, 0, _num_elts_alloc * sizeof(_type)); \
    } while (0)

#define DYN_ARRAY_ALLOC_WITH_INIT_FN(_dyn_array, _num_elts_alloc, _type, _fn) \
    do                                                                        \
    {                                                                         \
        size_t _x;                                                            \
        assert(_num_elts_alloc);                                              \
        (_dyn_array)->num_elts_alloc = _num_elts_alloc;                       \
        (_dyn_array)->num_elts = _num_elts_alloc;                             \
        (_dyn_array)->element_init_fn = _fn;                                  \
        (_dyn_array)->base = malloc(_num_elts_alloc * sizeof(_type));         \
        assert((_dyn_array)->base);                                           \
        memset((_dyn_array)->base, 0, _num_elts_alloc * sizeof(_type));       \
        _type *_a = (_type *)(_dyn_array)->base;                              \
        for (_x = 0; _x < (_dyn_array)->num_elts; _x++)                       \
        {                                                                     \
            _fn(_a[_x]);                                                      \
        }                                                                     \
    } while (0)

#define DYN_ARRAY_FREE(_dyn_array)            \
    do                                        \
    {                                         \
        assert((_dyn_array)->base);           \
        free((_dyn_array)->base);             \
        (_dyn_array)->base = NULL;            \
        (_dyn_array)->element_init_fn = NULL; \
    } while (0)

#define DYN_ARRAY_GROW(_dyn_array, _type, _size)                                                        \
    do                                                                                                  \
    {                                                                                                   \
        size_t _initial_num_elts, _new_num_elts;                                                        \
        _initial_num_elts = _new_num_elts = (_dyn_array)->num_elts;                                     \
        while (_new_num_elts * sizeof(_type) <= _size * sizeof(_type))                                  \
        {                                                                                               \
            _new_num_elts += (_dyn_array)->num_elts_alloc;                                              \
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
        (_dyn_array)->num_elts = _new_num_elts;                                                         \
    } while (0)

#define DYN_ARRAY_GET_ELT(_dyn_array, _idx, _type) ({ \
    assert(_dyn_array);                               \
    assert((_dyn_array)->num_elts);                   \
    _type *_elt = NULL;                               \
    if ((_dyn_array)->num_elts <= _idx)               \
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
        if ((_dyn_array)->num_elts >= _idx)              \
        {                                                \
            DYN_ARRAY_GROW(_dyn_array, _type, _idx);     \
        }                                                \
        _type *_ptr = (_type *)((_dyn_array)->base);     \
        memcpy(&(_ptr[_idx]), _elt, sizeof(_idx));       \
    } while (0)

/****************/
/* DYNAMIC LIST */
/****************/

typedef struct dyn_list
{
    size_t num_elts;
    size_t num_elts_alloc;
    ucs_list_link_t list;
    dyn_struct_elt_init_fn element_init_cb;
    // List of memory chunks used for the elements of the list
    size_t num_mem_chunks;
    dyn_array_t mem_chunks;
} dyn_list_t;

#define GROW_DYN_LIST(__dyn_list, __type, __elt)                                                                                 \
    do                                                                                                                           \
    {                                                                                                                            \
        assert(__dyn_list);                                                                                                      \
        size_t _initial_list_size = ucs_list_length(&((__dyn_list)->list));                                                      \
        size_t _chunk_size = (__dyn_list)->num_elts_alloc * sizeof(__type);                                                      \
        void *_new_chunk_buf = malloc(_chunk_size);                                                                              \
        if (_new_chunk_buf != NULL)                                                                                              \
        {                                                                                                                        \
            size_t _i;                                                                                                           \
            void *_ptr;                                                                                                          \
            mem_chunk_t *_chunk_ptr = DYN_ARRAY_GET_ELT(&((__dyn_list)->mem_chunks), (__dyn_list)->num_mem_chunks, mem_chunk_t); \
            assert(_chunk_ptr);                                                                                                  \
            _chunk_ptr->ptr = _new_chunk_buf;                                                                                    \
            _chunk_ptr->size = _chunk_size;                                                                                      \
            (__dyn_list)->num_mem_chunks++;                                                                                      \
            __type *_e = (__type *)(_new_chunk_buf);                                                                             \
            for (_i = 0; _i < (__dyn_list)->num_elts_alloc; _i++)                                                                \
            {                                                                                                                    \
                if ((__dyn_list)->element_init_cb != NULL)                                                                       \
                {                                                                                                                \
                    (__dyn_list)->element_init_cb((void *)&(_e[_i]));                                                            \
                }                                                                                                                \
                ucs_list_add_tail(&((__dyn_list)->list), &(_e[_i].__elt));                                                       \
                _ptr = &(_e[_i]);                                                                                                \
            }                                                                                                                    \
            assert(ucs_list_length(&((__dyn_list)->list)) == (_initial_list_size + (__dyn_list)->num_elts_alloc));               \
            assert((((ptrdiff_t)_ptr + sizeof(__type)) - ((ptrdiff_t)_new_chunk_buf + _chunk_size)) == 0);                       \
            (__dyn_list)->num_elts += (__dyn_list)->num_elts_alloc;                                                              \
        }                                                                                                                        \
        else                                                                                                                     \
        {                                                                                                                        \
            fprintf(stderr, "[ERROR] unable to grow dynamic list, unable to allocate buffer\n");                                 \
        }                                                                                                                        \
    } while (0)

#define DYN_LIST_ALLOC(_dyn_list, _num_elts_alloc, _type, _elt)                           \
    do                                                                                    \
    {                                                                                     \
        (_dyn_list) = malloc(sizeof(dyn_list_t));                                         \
        if ((_dyn_list) != NULL)                                                          \
        {                                                                                 \
            DYN_ARRAY_ALLOC(&((_dyn_list)->mem_chunks), DEFAULT_MEM_CHUNKS, mem_chunk_t); \
            (_dyn_list)->num_mem_chunks = 0;                                              \
            (_dyn_list)->num_elts = 0;                                                    \
            (_dyn_list)->num_elts_alloc = _num_elts_alloc;                                \
            (_dyn_list)->element_init_cb = NULL;                                          \
            ucs_list_head_init(&((_dyn_list)->list));                                     \
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
            (_dyn_list)->num_elts = 0;                                                    \
            (_dyn_list)->num_elts_alloc = _num_elts_alloc;                                \
            (_dyn_list)->element_init_cb = _cb;                                           \
            ucs_list_head_init(&((_dyn_list)->list));                                     \
            GROW_DYN_LIST((_dyn_list), _type, _elt);                                      \
        }                                                                                 \
        else                                                                              \
        {                                                                                 \
            fprintf(stderr, "[ERROR] unable to allocate dynamic list\n");                 \
            (_dyn_list) = NULL;                                                           \
        }                                                                                 \
    } while (0)

#define DYN_LIST_FREE(_dyn_list, _type, _elt)                                           \
    do                                                                                  \
    {                                                                                   \
        /* Release the list */                                                          \
        while (!ucs_list_is_empty(&((_dyn_list)->list)))                                \
        {                                                                               \
            _type *_item = ucs_list_extract_head(&((_dyn_list)->list), _type, _elt);    \
            assert(_item);                                                              \
            ucs_list_del(&(_item->_elt));                                               \
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

#define DYN_LIST_GET(_dyn_list, _type, _elt, _item)                       \
    do                                                                    \
    {                                                                     \
        if (ucs_list_is_empty(&((_dyn_list)->list)))                      \
        {                                                                 \
            GROW_DYN_LIST(_dyn_list, _type, _elt);                        \
        }                                                                 \
        _item = ucs_list_extract_head(&((_dyn_list)->list), _type, _elt); \
    } while (0)

#define DYN_LIST_RETURN(_dyn_list, _item, _elt)                  \
    do                                                           \
    {                                                            \
        ucs_list_add_tail(&((_dyn_list)->list), &(_item->_elt)); \
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
    ucs_list_link_t pool;
} smart_bucket_t;

#define RESET_SMART_BUCKET(__sb)        \
    do                                  \
    {                                   \
        (__sb)->sys = NULL;             \
        (__sb)->initial_size = 0;       \
        (__sb)->min_size = 0;           \
        (__sb)->max_size = 0;           \
        ucs_list_init(&((__sb)->pool)); \
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
#define SMART_BUFFS_GROW_BUCKET(__ptr)                                                  \
    do                                                                                  \
    {                                                                                   \
        size_t _i;                                                                      \
        void *_sb_new_mem_chunk = malloc((__ptr)->initial_size);                        \
        assert(_sb_new_chunk);                                                          \
        memset(_sb_new_mem_chunk, 0, (__ptr)->initial_size);                            \
        size_t __num_new_smart_chunks = (_ptr)->initial_size / (__ptr)->max_size;       \
        mem_chunk_t *_new_mem_chunk;                                                    \
        _new_mem_chunk = DYN_ARRAY_GET_ELT(&((__ptr)->sys->base_mem_chunks),            \
                                           (__ptr)->sys->num_base_mem_chunks,           \
                                           mem_chunk_t);                                \
        assert(_new_mem_chunk);                                                         \
        _new_mem_chunk->base = _sb_new_mem_chunk;                                       \
        _new_mem_chunk->size = (__ptr)->initial_size;                                   \
        (__ptr)->sys->num_mem_chunks++;                                                 \
        void *_base_ptr = _sb_new_mem_chunk;                                            \
        void *__prev = NULL;                                                            \
        smart_chunk_t *__first_chunk = NULL;                                            \
        for (_i = 0; _i < _num_new_chunks; _i++)                                        \
        {                                                                               \
            smart_chunk_t *_new_smart_chunk;                                            \
            DYN_LIST_ALLOC(&((__bdest)->pool), smart_chunk_t, super, _new_smart_chunk); \
            assert(_new_smart_chunk);                                                   \
            RESET_SMART_CHUNK(_new_smart_chunk);                                        \
            if (__first_chunk == NULL)                                                  \
                __first_chunk = _new_smart_chunk;                                       \
            _new_smart_chunk->base = _base_ptr;                                         \
            _new_smart_chunk->size = (__ptr)->max_size;                                 \
            _new_smart_chunk->prev = __prev;                                            \
            _new_smart_chunk->bucket = (struct smart_bucket *)(__ptr);                  \
            ucs_list_add_tail(&((__ptr)->pool), &(_new_smart_chunk->super));            \
            _base_ptr = (void *)((ptrdiff_t)_base_ptr + (__ptr)->max_size);             \
            __prev = (void *)_new_smart_chunk;                                          \
        }                                                                               \
        /* Close the ring */                                                            \
        __first_chunk->prev = __prev;                                                   \
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
                    ucs_list_del(&(__cur_sc->super));                                         \
                }                                                                             \
                /* Return the bigger chunk to its pool so it can be used */                   \
                __parent_chunk->in_use = false;                                               \
                ucs_list_add_tail(&(__parent_chunk->bucket->pool),                            \
                                  &(__parent_chunk->super));                                  \
            }                                                                                 \
        }                                                                                     \
    } while (0)

// Take a smart chunk from a source bucket and create multiple smaller smart chunks
// for the destination bucket.
#define SMART_BUFFS_SPLIT_BUCKET(__bsrc, __bdest)                                           \
    do                                                                                      \
    {                                                                                       \
        size_t _i;                                                                          \
                                                                                            \
        _num_new_smart_chunks = __bsrc->max_size / __bdest->max_size;                       \
        /* Get the chunk from the source so we can divide it up */                          \
        smart_chunk_t *_big_chunk_to_split;                                                 \
        ucs_list_extract_head(&((_bsrc)->pool), smart_chunk_t, super, _big_chunk_to_split); \
        assert(_big_chunk_to_split);                                                        \
        _big_chunk_to_split->in_use = true;                                                 \
        void *_sc_base_ptr = _big_chunk_to_split->base;                                     \
        for (_i = 0; _i < _num_new_smart_chunks; _i++)                                      \
        {                                                                                   \
            smart_chunk_t *_new_smart_chunk;                                                \
            DYN_LIST_ALLOC(&((__bdest)->pool), smart_chunk_t, super, _new_smart_chunk);     \
            assert(_new_smart_chunk);                                                       \
            RESET_SMART_CHUNK(_new_smart_chunk);                                            \
            _new_smart_chunk->parent_chunk = _big_chunk_to_split;                           \
            _new_smart_chunk->base = _sc_base_ptr;                                          \
            _new_smart_chunk->size = __bdest->max_size;                                     \
            ucs_list_add_tail(&((__bdest)->pool), &(_new_smart_chunk->super));              \
            _sc_base_ptr = (void *)((ptrdiff_t)_sc_base_ptr + _bdest->max_size);            \
        }                                                                                   \
    } while (0)

#define SMART_BUFFS_INIT(__ptr, __info_in)                                                                      \
    do                                                                                                          \
    {                                                                                                           \
        smart_buffers_info_t *__info;                                                                           \
        /* Default buckets, sizes in bytes */                                                                   \
        const size_t _smart_bufs_default_buckets[10] = {8, 16, 64, 128, 512, 1024, 4096, 8192, 65536, 1048576}; \
        const size_t _smart_bufs_default_total_size = 20 * 1024 * 1024;                                         \
        if (__info_in == NULL)                                                                                  \
        {                                                                                                       \
            smart_buffers_info_t __sb_default_info;                                                             \
            __sb_default_info.buckets = (size_t *)_smart_bufs_default_buckets;                                  \
            __sb_default_info.num_buckets = 10;                                                                 \
            __info = &__sb_default_info;                                                                        \
        }                                                                                                       \
        else                                                                                                    \
        {                                                                                                       \
            __info = __info_in;                                                                                 \
        }                                                                                                       \
        if (__ptr != NULL && __info != NULL)                                                                    \
        {                                                                                                       \
            /* Initialize internals */                                                                          \
            DYN_ARRAY_ALLOC(&((__ptr)->bucket_sizes), (__info)->num_buckets, size_t);                           \
            DYN_ARRAY_ALLOC(&((__ptr)->buckets), (__info)->num_buckets + 1, smart_bucket_t);                    \
            /* Allocate the base memory chunk that will be used to populate the buckets */                      \
            DYN_ARRAY_ALLOC(&((__ptr)->base_mem_chunks), 1, mem_chunk_t);                                       \
            /* Allocate the two pool of descriptors */                                                          \
            size_t __n_descs = 10 * 1024 * 1024;                                                                \
            DYN_LIST_ALLOC((__ptr)->smart_chunk_desc_pool, __n_descs, smart_chunk_t, super);                    \
            mem_chunk_t *_base_mem_chunk = DYN_ARRAY_GET_ELT(&((__ptr)->base_mem_chunks), 1, mem_chunk_t);      \
            size_t _total_mem = _smart_bufs_default_total_size;                                                 \
            _base_mem_chunk->ptr = calloc(_total_mem, sizeof(char));                                            \
            (__ptr)->num_base_mem_chunks = 1;                                                                   \
            size_t _bucket_mem_size = _total_mem / (__info)->num_buckets;                                       \
                                                                                                                \
            /* Copy meta-data from info object */                                                               \
            (__ptr)->num_buckets = (__info)->num_buckets;                                                       \
            size_t _i;                                                                                          \
            uint64_t _bucket_min_size = 0;                                                                      \
            for (_i = 0; _i < (__info)->num_buckets; _i++)                                                      \
            {                                                                                                   \
                size_t *_bucket_max_size = DYN_ARRAY_GET_ELT(&((__ptr)->bucket_sizes), _i, size_t);             \
                assert(_bucket_max_size);                                                                       \
                *_bucket_max_size = (__info)->buckets[_i];                                                      \
                smart_bucket_t *_smart_bucket = DYN_ARRAY_GET_ELT(&((__ptr)->buckets), _i, smart_bucket_t);     \
                assert(_smart_bucket);                                                                          \
                _smart_bucket->min_size = (uint64_t)_bucket_min_size;                                           \
                _smart_bucket->max_size = (uint64_t)*_bucket_max_size;                                          \
                _bucket_min_size = (uint64_t)((*_bucket_max_size) + 1);                                         \
                /* Add the smart chunks */                                                                      \
                size_t _j, _n = 0;                                                                              \
                void *_ptr_base = (void *)((ptrdiff_t)_base_mem_chunk->ptr + _i * _bucket_mem_size);            \
                for (_j = 0; _j < (_bucket_mem_size / _smart_bucket->max_size); _j++)                           \
                {                                                                                               \
                    void *_mem_ptr = (void *)((ptrdiff_t)_ptr_base + _j * _smart_bucket->max_size);             \
                    smart_chunk_t *_smart_chunk;                                                                \
                    DYN_LIST_GET((__ptr)->smart_chunk_desc_pool, smart_chunk_t, super, _smart_chunk);           \
                    assert(_smart_chunk);                                                                       \
                    RESET_SMART_CHUNK(_smart_chunk);                                                            \
                    _smart_chunk->base = _mem_ptr;                                                              \
                    _smart_chunk->size = _smart_bucket->max_size;                                               \
                    _smart_chunk->bucket = (struct smart_bucket *)_smart_bucket;                                \
                    _n++;                                                                                       \
                }                                                                                               \
                _smart_bucket->initial_size = _n;                                                               \
            }                                                                                                   \
            /* Initialize last bucket */                                                                        \
            smart_bucket_t *_last_smart_bucket = DYN_ARRAY_GET_ELT(&((__ptr)->buckets), _i, smart_bucket_t);    \
            assert(_last_smart_bucket);                                                                         \
            _last_smart_bucket->min_size = (uint64_t)_bucket_min_size;                                          \
            _last_smart_bucket->max_size = UINT64_MAX;                                                          \
            /* The last bucket does not have a known size and is populated on demand */                         \
        }                                                                                                       \
    } while (0)

#define SMART_BUFF_GET(__sys, __sz, __bf) ({                                                                      \
    size_t _i;                                                                                                    \
    smart_bucket_t *_target_bucket = NULL;                                                                        \
    smart_chunk_t *_target_chunk = NULL;                                                                          \
    for (_i = 0; _i < (__sys)->num_buckets; _i++)                                                                 \
    {                                                                                                             \
        smart_bucket_t *__b = DYN_ARRAY_GET_ELT((__sys)->buckets, i, smart_bucket_t);                             \
        assert(__b);                                                                                              \
        if ((__b)->min_size <= __sz && __sz <= (__b)->max_size)                                                   \
        {                                                                                                         \
            _target_bucket = __b;                                                                                 \
            break;                                                                                                \
        }                                                                                                         \
    }                                                                                                             \
    if (_target_bucket == NULL || _target_bucket->max_size == UINT64_MAX)                                         \
    {                                                                                                             \
        /* Need to create a new bucket, the size is bigger than anything we handle right now */                   \
        uint64_t _new_bucket_max_size = ((__sz / 4096) + 1) * 4096;                                               \
        size_t *__new_bucket_sz, *__prev_bucket_sz;                                                               \
        smart_bucket_t *__new_bucket;                                                                             \
        __new_bucket_sz = DYN_ARRAY_GET_ELT(&((__sys)->bucket_sizes), (__sys)->num_buckets, smart_bucket_t);      \
        assert(__new_bucket_sz);                                                                                  \
        __prev_bucket_sz = DYN_ARRAY_GET_ELT(&((__sys)->bucket_sizes), (__sys)->num_buckets - 1, smart_bucket_t); \
        assert(__prev_bucket_sz);                                                                                 \
        *__new_bucket_sz = _new_bucket_max_size;                                                                  \
        __new_bucket = DYN_ARRAY_GET_ELT(&((__sys)->buckets), (__sys)->num_buckets, super);                       \
        assert(_new_bucket);                                                                                      \
        _new_bucket->min_size = __prev_bucket_sz + 1;                                                             \
        _new_bucket->max_size = __new_bucket_sz;                                                                  \
        _mem_chunk_desc = DYN_ARRAY_GET_ELT((__sys)->base_mem_chunks, (__sys)->num_base_mem_chunks, mem_chunk_t); \
        assert(_mem_chunk_desc);                                                                                  \
        size_t _bucket_mem_size = 10 * _new_bucket_max_size;                                                      \
        _mem_chunk_desc->ptr = calloc(_bucket_mem_size, sizeof(char));                                            \
        (__sys)->num_base_mem_chunks++;                                                                           \
        /* Add the smart chunks */                                                                                \
        size_t _j, __n = 0;                                                                                       \
        for (_j = 0; _j < (_bucket_mem_size / _new_bucket->max_size); _j++)                                       \
        {                                                                                                         \
            void *_mem_ptr = (void *)((ptrdiff_t)_mem_chunk_desc->ptr + _j * _new_bucket->max_size);              \
            smart_chunk_t *_smart_chunk;                                                                          \
            DYN_LIST_GET(&((__ptr)->smart_chunk_desc_pool), smart_chunk_t, super, _smart_chunk);                  \
            assert(_smart_chunk);                                                                                 \
            RESET_SMART_CHUNK(_smart_chunk);                                                                      \
            _smart_chunk->base = _mem_ptr;                                                                        \
            _smart_chunk->size = _new_bucket->max_size;                                                           \
            ucs_list_add_tail(&(__new_bucket->pool), &(_smart_chunk->super));                                     \
            __n++;                                                                                                \
        }                                                                                                         \
        __new_bucket->initial_size = __n;                                                                         \
        _target_bucket = __new_bucket;                                                                            \
    }                                                                                                             \
    assert(_target_bucket);                                                                                       \
    _target_chunk = ucs_list_extract_head(&(_target_bucket->pool), smart_chunk_t, super);                         \
    _target_chunk->is_use = true;                                                                                 \
    __bf = _target_chunk->base;                                                                                   \
})

#define SMART_BUFF_RETURN(__sys, __sz, __bf) ({                                                  \
    size_t _i;                                                                                   \
    smart_chunk_t *_sc = ucs_derived_of(__bf);                                                   \
    assert(_sc->in_use == true);                                                                 \
    /* Find the associated bucket */                                                             \
    for (_i = 0; _i < (__sys)->num_buckets; _i++)                                                \
    {                                                                                            \
        smart_bucket_t *__b = DYN_ARRAY_GET_ELT((__sys)->buckets, i, smart_bucket_t);            \
        assert(__b);                                                                             \
        if ((__b)->min_size <= __sz && __sz <= (__b)->max_size)                                  \
        {                                                                                        \
            _target_bucket = __b;                                                                \
            break;                                                                               \
        }                                                                                        \
    }                                                                                            \
    assert(_target_bucket);                                                                      \
    _sc->in_use = false;                                                                         \
    ucs_list_add_tail(&(_target_bucket->pool), &(_sc->super));                                   \
    /* Check if it makes sense to try to recycle smart buffers */                                \
    size_t _recycle_threshold = _target_bucket->initial_size + _target_bucket->initial_size / 2; \
    if (ucs_list_length(&(_target_bucket->pool)) > _recycle_threshold)                           \
        SMART_BUFFS_RECYCLE_SMART_CHUNK(_sc);                                                    \
    /* Make sure the pointer cannot be used anymore */                                           \
    __bf = NULL;                                                                                 \
})

#define SMART_BUFFS_FINI(__sys)                                                                   \
    do                                                                                            \
    {                                                                                             \
        size_t __i;                                                                               \
        /* Return all the smart chunks */                                                         \
        for (__i = 0; __i < (__sys)->num_buckets; __i++)                                          \
        {                                                                                         \
            smart_bucket_t *__sb = DYN_ARRAY_GET_ELT(&((__sys)->buckets), __i, smart_bucket_t);   \
            while (!ucs_list_is_empty(&(__sb->pool)))                                             \
            {                                                                                     \
                smart_chunk_t *__sc;                                                              \
                __sc = ucs_list_extract_head(&(__sb->pool), smart_chunk_t, super);                \
                if (__sc->parent_chunk == NULL)                                                   \
                {                                                                                 \
                    DYN_LIST_RETURN((__sys)->smart_chunk_desc_pool, __sc, super);                 \
                }                                                                                 \
                else                                                                              \
                {                                                                                 \
                    ucs_list_add_tail(&(__sb->pool), &(__sc->super));                             \
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

#endif // DPU_OFFLOAD_DYNAMIC_LIST_H
