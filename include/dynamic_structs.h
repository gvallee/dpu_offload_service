//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>

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

typedef struct
{
    ucs_list_link_t item;

    // Pointer to the parent chunk when a chunk is split into sub-chunk.
    // This is therefore used to recycle/recombine the chunks when necessary.
    // Ultimately, it can be used to track the hierachy of chunks that are
    // being used.
    void *parent_chunk;

    void *base;

    size_t size;
} smart_chunk_t;

typedef struct
{
    // Minimum buffer size in the bucket
    uint64_t min_size;

    // Maximum buffer size in the bucket
    uint64_t max_size;

    // Pool of smart chunks in the bucket, ready to be used (type: smart_chunk_t)
    ucs_list_link_t pool;
} smart_bucket_t;

typedef struct
{
    // Number of buckets
    size_t num_buckets;

    size_t num_base_mem_chunks;
    dyn_array_t base_mem_chunks;

    // List of buckets (type: size_t)
    dyn_array_t bucket_sizes;

    // List of buckets (type: smart_bucket_t)
    dyn_array_t bucket_desc;

    // Pool of smart_bucket_descriptors (type: smart_bucket_t)
    dyn_list_t smart_bucket_desc_pool;
} smart_buffers_t;

typedef struct
{
    size_t num_buckets;

    size_t *buckets;
} smart_buffers_info_t;

#define SMART_BUFFS_GROW_BUCKET(__ptr) \
    do                                 \
    {                                  \
    } while (0)

#define SMART_BUFFS_SPLIT_BUCKET(__bsrc, __bdest) \
    do                                            \
    {                                             \
    } while (0)

#define SMART_BUFFS_INIT(__ptr, __info)                                                                          \
    do                                                                                                           \
    {                                                                                                            \
        if (__ptr != NULL && __info != NULL)                                                                     \
        {                                                                                                        \
            /* Initialize internals */                                                                           \
            DYN_ARRAY_ALLOC(&((__ptr)->bucket_sizes), (__info)->num_buckets, size_t);                            \
            DYN_ARRAY_ALLOC(&((__ptr)->bucket_desc), (__info)->num_buckets + 1, smart_bucket_t);                 \
            /* Alocate the base memory chunk that will be used to populate the buckets */                        \
            DYN_ARRAY_ALLOC(&((__ptr)->base_mem_chunks), 1, mem_chunk_t);                                        \
            mem_chunk_t *_base_mem_chunk = DYN_ARRAY_GET_ELT(&((__ptr)->base_mem_chunks), 1, mem_chunk_t);       \
            size_t _total_mem = 10 * 1024 * 1024;                                                                \
            _base_mem_chunk->ptr = calloc(_total_mem, sizeof(char));                                             \
            (__ptr)->num_base_mem_chunks = 1;                                                                    \
            size_t v = _total_mem / (__info)->num_buckets;                                                       \
                                                                                                                 \
            /* Copy meta-data from info object */                                                                \
            (__ptr)->num_buckets = (__info)->num_buckets;                                                        \
            size_t _i;                                                                                           \
            (uint64_t) _bucket_min_size = 0;                                                                     \
            for (_i = 0; _i < (__info)->num_buckets; _i++)                                                       \
            {                                                                                                    \
                size_t *_bucket_max_size = DYN_ARRAY_GET_ELT(&((__ptr)->bucket_sizes), _i, size_t);              \
                assert(_bucket_max_size);                                                                        \
                *_bucket_max_size = (__info)->buckets[_i];                                                       \
                smart_bucket_t *_smart_bucket = DYN_ARRAY_GET_ELT(&((__ptr)->bucket_desc), _i, smart_bucket_t);  \
                assert(_smart_bucket);                                                                           \
                _smart_bucket->min_size = (uint64_t)_bucket_min_size;                                            \
                _smart_bucket->max_size = (uint64_t)*_bucket_max_size;                                           \
                _buck_min_size = (uint64_t)((*_bucket_max_size) + 1);                                            \
                /* Add the smart chunks */                                                                       \
                size_t _j;                                                                                       \
                void *_ptr_base = (void *)((ptrdiff_t)_base_mem_chunk->ptr + _i * _bucket_mem_size);             \
                for (_j = 0; _j < (_bucket_mem_size / _smart_bucket->max_size); _j++)                            \
                {                                                                                                \
                    void *_mem_ptr = (void *)((ptrdiff_t)_ptr_base + _j * _smart_bucket->max_size);              \
                    smart_chunk_t *_smart_chunk;                                                                 \
                    DYN_LIST_GET(&((__ptr)->smart_chunk_pool), smart_chunk_t, item, _smart_chunk);               \
                    assert(_smart_chunk);                                                                        \
                    RESET_SMART_CHUNK(_smart_chunk);                                                             \
                    _smart_chunk->base = _mem_ptr;                                                               \
                    _smart_chunk->size = _smart_bucket->max_size;                                                \
                }                                                                                                \
            }                                                                                                    \
            /* Initialize last bucket */                                                                         \
            smart_bucket_t *_last_smart_bucket = DYN_ARRAY_GET_ELT(&((__ptr)->bucket_desc), _i, smart_bucket_t); \
            assert(_last_smart_bucket);                                                                          \
            _last_smart_bucket->min_size = (uint64_t)_bucket_min_size;                                           \
            _last_smart_bicket->max_size = UINT64_MAX;                                                           \
            /* The last bucket does not have a known size and is populated on demand */                          \
        }                                                                                                        \
    } while (0)

#define SMART_BUFF_GET(__sys, __sz, __bf)                                                                             \
    do                                                                                                                \
    {                                                                                                                 \
        size_t _i;                                                                                                    \
        smart_bucket_t *_target_bucket = NULL;                                                                        \
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
            __new_bucket = DYN_ARRAY_GET_ELT(&((__sys)->buckets), (__sys)->num_buckets, item);                        \
            assert(_new_bucket);                                                                                      \
            _new_bucket->min_size = __prev_bucket_sz + 1;                                                             \
            _new_bucket->max_size = __new_bucket_sz;                                                                  \
            _mem_chunk_desc = DYN_ARRAY_GET_ELT((__sys)->base_mem_chunks, (__sys)->num_base_mem_chunks, mem_chunk_t); \
            assert(_mem_chunk_desc);                                                                                  \
            size_t _bucket_mem_size = 10 * _new_bucket_max_size;                                                      \
            _mem_chunk_desc->ptr = calloc(_bucket_mem_size, sizeof(char));                                            \
            (__sys)->num_base_mem_chunks++;                                                                           \
            /* Add the smart chunks */                                                                                \
            size_t _j;                                                                                                \
            for (_j = 0; _j < (_bucket_mem_size / _new_bucket->max_size); _j++)                                       \
            {                                                                                                         \
                void *_mem_ptr = (void *)((ptrdiff_t)_mem_chunk_desc->ptr + _j * _new_bucket->max_size);              \
                smart_chunk_t *_smart_chunk;                                                                          \
                DYN_LIST_GET(&((__ptr)->smart_chunk_pool), smart_chunk_t, item, _smart_chunk);                        \
                assert(_smart_chunk);                                                                                 \
                RESET_SMART_CHUNK(_smart_chunk);                                                                      \
                _smart_chunk->base = _mem_ptr;                                                                        \
                _smart_chunk->size = _new_bucket->max_size;                                                           \
            }                                                                                                         \
        }                                                                                                             \
    } while (0)

#define SMART_BUFFS_FINI(__sys) \
    do                          \
    {                           \
    } while (0)

#endif // DPU_OFFLOAD_DYNAMIC_LIST_H
