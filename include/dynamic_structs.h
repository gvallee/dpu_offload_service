//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <assert.h>

#include <ucs/datastruct/list.h>

#ifndef DPU_OFFLOAD_DYNAMIC_LIST_H
#define DPU_OFFLOAD_DYNAMIC_LIST_H

#define MAX_MEM_CHUNKS (1024)

typedef struct mem_chunk
{
    size_t size;
    void *ptr;
} mem_chunk_t;

/****************/
/* DYNAMIC LIST */
/****************/

typedef struct dyn_list
{
    size_t num_elts;
    size_t num_elts_alloc;
    ucs_list_link_t list;
    // List of memory chunks used for the elements of the list
    size_t num_mem_chunks;
    mem_chunk_t mem_chunks[MAX_MEM_CHUNKS];
} dyn_list_t;

#define GROW_DYN_LIST(__dyn_list, __type, __elt)                                                                           \
    do                                                                                                                     \
    {                                                                                                                      \
        assert(__dyn_list);                                                                                                \
        assert(ucs_list_length(&((__dyn_list)->list)) == 0);                                                               \
        if ((__dyn_list)->num_mem_chunks + 1 < MAX_MEM_CHUNKS)                                                             \
        {                                                                                                                  \
            size_t _chunk_size = (__dyn_list)->num_elts_alloc * sizeof(__type);                                            \
            void *_chunk = malloc(_chunk_size);                                                                            \
            if (_chunk != NULL)                                                                                            \
            {                                                                                                              \
                (__dyn_list)->mem_chunks[(__dyn_list)->num_mem_chunks].ptr = _chunk;                                       \
                (__dyn_list)->mem_chunks[(__dyn_list)->num_mem_chunks].size = _chunk_size;                                 \
                (__dyn_list)->num_mem_chunks++;                                                                            \
                int _i;                                                                                                    \
                for (_i = 0; _i < (__dyn_list)->num_elts_alloc; _i++)                                                      \
                {                                                                                                          \
                    __type *_e = (__type *)(_chunk);                                                                       \
                    ucs_list_add_tail(&((__dyn_list)->list), &(_e[_i].__elt));                                             \
                }                                                                                                          \
                if (ucs_list_length(&((__dyn_list)->list)) != ((__dyn_list)->num_elts + (__dyn_list)->num_elts_alloc))     \
                {                                                                                                          \
                    fprintf(stderr, "List size=%ld\n", ucs_list_length(&((__dyn_list)->list)));                            \
                    fprintf(stderr, "Expected number of elements: %ld, num_elts_alloc=%ld\n",                              \
                            ((__dyn_list)->num_elts + (__dyn_list)->num_elts_alloc),                                       \
                            (__dyn_list)->num_elts_alloc);                                                                 \
                }                                                                                                          \
                assert(ucs_list_length(&((__dyn_list)->list)) == ((__dyn_list)->num_elts + (__dyn_list)->num_elts_alloc)); \
                (__dyn_list)->num_elts += (__dyn_list)->num_elts_alloc;                                                    \
            }                                                                                                              \
        }                                                                                                                  \
    } while (0)

#define DYN_LIST_ALLOC(_dyn_list, _num_elts_alloc, _type, _elt) \
    do                                                          \
    {                                                           \
        (_dyn_list) = malloc(sizeof(dyn_list_t));               \
        if ((_dyn_list) != NULL)                                \
        {                                                       \
            (_dyn_list)->num_mem_chunks = 0;                    \
            (_dyn_list)->num_elts = 0;                          \
            (_dyn_list)->num_elts_alloc = _num_elts_alloc;      \
            ucs_list_head_init(&((_dyn_list)->list));           \
            GROW_DYN_LIST((_dyn_list), _type, _elt);            \
        }                                                       \
        else                                                    \
            (_dyn_list) = NULL;                                 \
    } while (0)

#define DYN_LIST_FREE(_dyn_list, _type, _elt)                                        \
    do                                                                               \
    {                                                                                \
        /* Release the list */                                                       \
        while (!ucs_list_is_empty(&((_dyn_list)->list)))                             \
        {                                                                            \
            _type *_item = ucs_list_extract_head(&((_dyn_list)->list), _type, _elt); \
            assert(_item);                                                           \
            ucs_list_del(&(_item->_elt));                                            \
        }                                                                            \
        /* Free the underlying memory chunks */                                      \
        int _i;                                                                      \
        for (_i = 0; _i < (_dyn_list)->num_mem_chunks; _i++)                         \
        {                                                                            \
            free((_dyn_list)->mem_chunks[_i].ptr);                                   \
        }                                                                            \
        free(_dyn_list);                                                             \
        _dyn_list = NULL;                                                            \
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
        char *_start = (char *)(((ptrdiff_t)((_dyn_array)->base)) + _initial_num_elts * sizeof(_type)); \
        memset(_start, 0, (_new_num_elts - _initial_num_elts) * sizeof(_type));                         \
        (_dyn_array)->num_elts = _new_num_elts;                                                         \
        assert((_dyn_array)->base);                                                                     \
    } while (0)

#define DYN_ARRAY_GET_ELT(_dyn_array, _idx, _type, _elt) \
    do                                                   \
    {                                                    \
        assert(_dyn_array);                              \
        if ((_dyn_array)->num_elts <= _idx)              \
        {                                                \
            DYN_ARRAY_GROW(_dyn_array, _type, _idx);     \
        }                                                \
        _type *_ptr = (_type *)((_dyn_array)->base);     \
        _elt = (_type *)&(_ptr[_idx]);                   \
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

#endif // DPU_OFFLOAD_DYNAMIC_LIST_H
