//
// Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef _TEST_CACHE_COMMON_H_
#define _TEST_CACHE_COMMON_H_

#include <unistd.h>

#define DEFAULT_NUM_RANKS (10)

const group_uid_t default_gp_uid = 42;

#define POPULATE_CACHE(_engine, _group_uid, _n_ranks)                               \
    do                                                                              \
    {                                                                               \
        /* Create local cache entries */                                            \
        cache_t *_cache = &(_engine->procs_cache);                                  \
        group_cache_t *_gp_cache = NULL;                                            \
        group_uid_t _gp_uid;                                                        \
        size_t i;                                                                   \
        _gp_uid = _group_uid;                                                       \
        _gp_cache = GET_GROUP_CACHE(_cache, _gp_uid);                               \
        assert(_gp_cache);                                                          \
        for (i = 0; i < _n_ranks; i++)                                              \
        {                                                                           \
            peer_cache_entry_t *new_entry = NULL;                                   \
            new_entry = GET_GROUP_RANK_CACHE_ENTRY(_cache, _gp_uid, i, _n_ranks);   \
            assert(new_entry);                                                      \
            RESET_PEER_DATA(&(new_entry->peer));                                    \
            new_entry->peer.proc_info.group_rank = i;                               \
            new_entry->peer.proc_info.group_uid = _group_uid;                       \
            new_entry->peer.host_info = HASH_LOCAL_HOSTNAME();                      \
            new_entry->set = true;                                                  \
        }                                                                           \
                                                                                    \
        if (_cache->size != 1)                                                      \
        {                                                                           \
            fprintf(stderr, "EP cache size is %ld instead of 1\n", _cache->size);   \
            goto error_out;                                                         \
        }                                                                           \
    } while (0)

#define CHECK_CACHE(_engine, _group_uid, _n_ranks)                              \
    do                                                                          \
    {                                                                           \
        cache_t *_cache = &(_engine->procs_cache);                              \
        group_uid_t group_uid;                                                  \
        group_cache_t *_gp = NULL;                                              \
        group_uid = _group_uid;                                                 \
        _gp = GET_GROUP_CACHE(_cache, group_uid);                               \
        if (_gp->initialized == false)                                          \
        {                                                                       \
            fprintf(stderr, "target group is not marked as initialized");       \
            goto error_out;                                                     \
        }                                                                       \
                                                                                \
        size_t i;                                                               \
        for (i = 0; i < _n_ranks; i++)                                          \
        {                                                                       \
            if (_gp->ranks[i].peer.proc_info.group_rank != i)                   \
            {                                                                   \
                fprintf(stderr, "cache entry as rank %ld instead of %ld\n",     \
                        _gp->ranks[i].peer.proc_info.group_rank, i);            \
                goto error_out;                                                 \
            }                                                                   \
                                                                                \
            if (_gp->ranks[i].peer.proc_info.group_uid != _group_uid)           \
            {                                                                   \
                fprintf(stderr, "cache entry as group ID %d instead of %d\n",   \
                        _gp->ranks[i].peer.proc_info.group_uid,                 \
                        _group_uid);                                            \
                goto error_out;                                                 \
            }                                                                   \
        }                                                                       \
    } while (0)

#endif // _TEST_CACHE_COMMON_H_