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

/*****************/
/* DYNAMIC ARRAY */
/*****************/

typedef struct dyn_array
{
    void *base;
    size_t num_elts;
    size_t num_elts_alloc;
} dyn_array_t;

#define DYN_ARRAY_ALLOC(_dyn_array, _num_elts_alloc, _type)             \
    do                                                                  \
    {                                                                   \
        assert(_num_elts_alloc);                                        \
        (_dyn_array)->num_elts_alloc = _num_elts_alloc;                 \
        (_dyn_array)->num_elts = _num_elts_alloc;                       \
        (_dyn_array)->base = malloc(_num_elts_alloc * sizeof(_type));   \
        assert((_dyn_array)->base);                                     \
        memset((_dyn_array)->base, 0, _num_elts_alloc * sizeof(_type)); \
    } while (0)

#define DYN_ARRAY_FREE(_dyn_array)  \
    do                              \
    {                               \
        assert((_dyn_array)->base); \
        free((_dyn_array)->base);   \
        (_dyn_array)->base = NULL;  \
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
        (_dyn_array)->num_elts = _new_num_elts;                                                         \
    } while (0)

#define DYN_ARRAY_GET_ELT(_dyn_array, _idx, _type, _elt)                                    \
    do                                                                                      \
    {                                                                                       \
        assert(_dyn_array);                                                                 \
        assert((_dyn_array)->num_elts);                                                     \
        if ((_dyn_array)->num_elts <= _idx)                                                 \
        {                                                                                   \
            fprintf(stderr, "Need to grow array: %ld %ld\n", (_dyn_array)->num_elts, _idx); \
            DYN_ARRAY_GROW(_dyn_array, _type, _idx);                                        \
        }                                                                                   \
        _type *_ptr = (_type *)((_dyn_array)->base);                                        \
        _elt = (_type *)&(_ptr[_idx]);                                                      \
    } while (0)

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

typedef void (*dyn_list_elt_init_fn)(void *);

typedef struct dyn_list
{
    size_t num_elts;
    size_t num_elts_alloc;
    ucs_list_link_t list;
    dyn_list_elt_init_fn element_init_cb;
    // List of memory chunks used for the elements of the list
    size_t num_mem_chunks;
    dyn_array_t mem_chunks;
} dyn_list_t;

#define GROW_DYN_LIST(__dyn_list, __type, __elt)                                                                   \
    do                                                                                                             \
    {                                                                                                              \
        assert(__dyn_list);                                                                                        \
        fprintf(stderr, "Growing list %p (adding %ld elts)...\n", (__dyn_list), (__dyn_list)->num_elts_alloc);     \
        size_t _initial_list_size = ucs_list_length(&((__dyn_list)->list));                                        \
        size_t _chunk_size = (__dyn_list)->num_elts_alloc * sizeof(__type);                                        \
        void *_new_chunk_buf = malloc(_chunk_size);                                                                \
        if (_new_chunk_buf != NULL)                                                                                \
        {                                                                                                          \
            size_t _i;                                                                                             \
            void *_ptr;                                                                                            \
            mem_chunk_t *_chunk_ptr;                                                                               \
            fprintf(stderr, "Getting mem chunk entry #%ld...\n", (__dyn_list)->num_mem_chunks);                    \
            DYN_ARRAY_GET_ELT(&((__dyn_list)->mem_chunks), (__dyn_list)->num_mem_chunks, mem_chunk_t, _chunk_ptr); \
            assert(_chunk_ptr);                                                                                    \
            _chunk_ptr->ptr = _new_chunk_buf;                                                                      \
            _chunk_ptr->size = _chunk_size;                                                                        \
            (__dyn_list)->num_mem_chunks++;                                                                        \
            fprintf(stderr, "Adding new elements to list...\n");                                                   \
            __type *_e = (__type *)(_new_chunk_buf);                                                               \
            for (_i = 0; _i < (__dyn_list)->num_elts_alloc; _i++)                                                  \
            {                                                                                                      \
                if ((__dyn_list)->element_init_cb != NULL)                                                         \
                {                                                                                                  \
                    (__dyn_list)->element_init_cb((void *)&(_e[_i]));                                                     \
                }                                                                                                  \
                ucs_list_add_tail(&((__dyn_list)->list), &(_e[_i].__elt));                                         \
                _ptr = &(_e[_i]);                                                                                  \
            }                                                                                                      \
            assert(ucs_list_length(&((__dyn_list)->list)) == (_initial_list_size + (__dyn_list)->num_elts_alloc)); \
            assert((((ptrdiff_t)_ptr + sizeof(__type)) - ((ptrdiff_t)_new_chunk_buf + _chunk_size)) == 0);         \
            (__dyn_list)->num_elts += (__dyn_list)->num_elts_alloc;                                                \
            fprintf(stderr, "List successfully grown\n");                                                          \
        }                                                                                                          \
        else                                                                                                       \
        {                                                                                                          \
            fprintf(stderr, "[ERROR] unable to grow dynamic list, unable to allocate buffer\n");                   \
        }                                                                                                          \
    } while (0)

#define DYN_LIST_ALLOC(_dyn_list, _num_elts_alloc, _type, _elt)                               \
    do                                                                                        \
    {                                                                                         \
        (_dyn_list) = malloc(sizeof(dyn_list_t));                                             \
        if ((_dyn_list) != NULL)                                                              \
        {                                                                                     \
            fprintf(stderr, "Allocating mem chunk dynamic array for list %p\n", (_dyn_list)); \
            DYN_ARRAY_ALLOC(&((_dyn_list)->mem_chunks), DEFAULT_MEM_CHUNKS, mem_chunk_t);     \
            (_dyn_list)->num_mem_chunks = 0;                                                  \
            (_dyn_list)->num_elts = 0;                                                        \
            (_dyn_list)->num_elts_alloc = _num_elts_alloc;                                    \
            (_dyn_list)->element_init_cb = NULL;                                              \
            ucs_list_head_init(&((_dyn_list)->list));                                         \
            GROW_DYN_LIST((_dyn_list), _type, _elt);                                          \
        }                                                                                     \
        else                                                                                  \
        {                                                                                     \
            fprintf(stderr, "[ERROR] unable to allocate dynamic list\n");                     \
            (_dyn_list) = NULL;                                                               \
        }                                                                                     \
    } while (0)

#define DYN_LIST_ALLOC_WITH_INIT_CALLBACK(_dyn_list, _num_elts_alloc, _type, _elt, _cb)       \
    do                                                                                        \
    {                                                                                         \
        (_dyn_list) = malloc(sizeof(dyn_list_t));                                             \
        if ((_dyn_list) != NULL)                                                              \
        {                                                                                     \
            fprintf(stderr, "Allocating mem chunk dynamic array for list %p\n", (_dyn_list)); \
            DYN_ARRAY_ALLOC(&((_dyn_list)->mem_chunks), DEFAULT_MEM_CHUNKS, mem_chunk_t);     \
            (_dyn_list)->num_mem_chunks = 0;                                                  \
            (_dyn_list)->num_elts = 0;                                                        \
            (_dyn_list)->num_elts_alloc = _num_elts_alloc;                                    \
            (_dyn_list)->element_init_cb = _cb;                                               \
            ucs_list_head_init(&((_dyn_list)->list));                                         \
            GROW_DYN_LIST((_dyn_list), _type, _elt);                                          \
        }                                                                                     \
        else                                                                                  \
        {                                                                                     \
            fprintf(stderr, "[ERROR] unable to allocate dynamic list\n");                     \
            (_dyn_list) = NULL;                                                               \
        }                                                                                     \
    } while (0)

#define DYN_LIST_FREE(_dyn_list, _type, _elt)                                               \
    do                                                                                      \
    {                                                                                       \
        /* Release the list */                                                              \
        while (!ucs_list_is_empty(&((_dyn_list)->list)))                                    \
        {                                                                                   \
            _type *_item = ucs_list_extract_head(&((_dyn_list)->list), _type, _elt);        \
            assert(_item);                                                                  \
            ucs_list_del(&(_item->_elt));                                                   \
        }                                                                                   \
        /* Free the underlying memory chunks */                                             \
        size_t _i;                                                                          \
        for (_i = 0; _i < (_dyn_list)->num_mem_chunks; _i++)                                \
        {                                                                                   \
            mem_chunk_t *_mem_chunk_ptr;                                                    \
            DYN_ARRAY_GET_ELT(&((_dyn_list)->mem_chunks), _i, mem_chunk_t, _mem_chunk_ptr); \
            assert(_mem_chunk_ptr);                                                         \
            free(_mem_chunk_ptr->ptr);                                                      \
            _mem_chunk_ptr->ptr = NULL;                                                     \
        }                                                                                   \
        DYN_ARRAY_FREE(&((_dyn_list)->mem_chunks));                                         \
        _dyn_list = NULL;                                                                   \
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

#endif // DPU_OFFLOAD_DYNAMIC_LIST_H
