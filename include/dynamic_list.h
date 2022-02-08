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

typedef struct dyn_list
{
    size_t num_elts;
    size_t num_elts_alloc;
    ucs_list_link_t list;
    // List of memory chunks used for the elements of the list
    size_t num_mem_chunks;
    mem_chunk_t mem_chunks[MAX_MEM_CHUNKS];
} dyn_list_t;

#define GROW_DYN_LIST(_dyn_list, _type, __elt)                                       \
    do                                                                               \
    {                                                                                \
        assert(_dyn_list);                                                           \
        if (_dyn_list->num_mem_chunks + 1 < MAX_MEM_CHUNKS)                          \
        {                                                                            \
            size_t _chunk_size = _dyn_list->num_elts_alloc * sizeof(_type);          \
            void *_chunk = malloc(_chunk_size);                                      \
            if (_chunk != NULL)                                                      \
            {                                                                        \
                _dyn_list->mem_chunks[_dyn_list->num_mem_chunks].ptr = _chunk;       \
                _dyn_list->mem_chunks[_dyn_list->num_mem_chunks].size = _chunk_size; \
                _dyn_list->num_mem_chunks++;                                         \
                int _i;                                                              \
                for (_i = 0; _i < _dyn_list->num_elts_alloc; _i++)                   \
                {                                                                    \
                    _type *_e = (_type *)((ptrdiff_t)_chunk + _i * sizeof(_type));   \
                    ucs_list_add_tail(&(_dyn_list->list), &(_e->__elt));             \
                }                                                                    \
                _dyn_list->num_elts += _dyn_list->num_elts_alloc;                    \
            }                                                                        \
        }                                                                            \
    } while (0)

#define DYN_LIST_ALLOC(_dyn_list, _num_elts_alloc, _type, _elt) \
    do                                                          \
    {                                                           \
        _dyn_list = malloc(sizeof(dyn_list_t));                 \
        if (_dyn_list != NULL)                                  \
        {                                                       \
            ucs_list_head_init(&(_dyn_list->list));             \
            _dyn_list->num_elts_alloc = _num_elts_alloc;        \
            _dyn_list->num_elts = 0;                            \
            GROW_DYN_LIST(_dyn_list, _type, _elt);              \
        }                                                       \
    } while (0)

#define DYN_LIST_FREE(_dyn_list, _type, _elt)                                      \
    do                                                                             \
    {                                                                              \
        /* Release the list */                                                     \
        while (!ucs_list_is_empty(&(_dyn_list->list)))                             \
        {                                                                          \
            _type *_item = ucs_list_extract_head(&(_dyn_list->list), _type, _elt); \
            assert(_item);                                                         \
            ucs_list_del(&(_item->_elt));                                          \
        }                                                                          \
        /* Free the underlying memory chunks */                                    \
        int _i;                                                                    \
        for (_i = 0; _i < _dyn_list->num_mem_chunks; _i++)                         \
        {                                                                          \
            free(_dyn_list->mem_chunks[_i].ptr);                                   \
        }                                                                          \
        free(_dyn_list);                                                           \
        _dyn_list = NULL;                                                          \
    } while (0)

#define DYN_LIST_GET(_dyn_list, _type, _elt, _item)                     \
    do                                                                  \
    {                                                                   \
        if (ucs_list_is_empty(&(_dyn_list->list)))                      \
        {                                                               \
            GROW_DYN_LIST(_dyn_list, _type, _elt);                      \
        }                                                               \
        _item = ucs_list_extract_head(&(_dyn_list->list), _type, _elt); \
    } while (0)

#define DYN_LIST_RETURN(_dyn_list, _item, _elt)                \
    do                                                         \
    {                                                          \
        ucs_list_add_tail(&(_dyn_list->list), &(_item->_elt)); \
    } while (0)
int dynamic_list_return();

#endif // DPU_OFFLOAD_DYNAMIC_LIST_H