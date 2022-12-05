//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef _TEST_CACHE_COMMON_H_
#define _TEST_CACHE_COMMON_H_

#include <unistd.h>

#define NUM_CACHE_ENTRIES (10) //(DEFAULT_NUM_PEERS * 2)

#define POPULATE_CACHE(_engine)                                                            \
    do                                                                                     \
    {                                                                                      \
        /* Create local cache entries */                                                   \
        cache_t *_cache = &(_engine->procs_cache);                                         \
        group_cache_t *_gp_cache = NULL;                                                   \
        group_uid_t _gp_uid;                                                               \
        size_t i;                                                                          \
        _gp_uid = 42;                                                                      \
        _gp_cache = GET_GROUP_CACHE(_cache, _gp_uid);                                      \
        for (i = 0; i < NUM_CACHE_ENTRIES; i++)                                            \
        {                                                                                  \
            peer_cache_entry_t *new_entry = NULL;                                          \
            new_entry = GET_GROUP_RANK_CACHE_ENTRY(_cache, _gp_uid, i, NUM_CACHE_ENTRIES); \
            RESET_PEER_DATA(&(new_entry->peer));                                           \
            new_entry->peer.proc_info.group_rank = i;                                      \
            new_entry->peer.proc_info.group_uid = 42;                                      \
            new_entry->peer.host_info = HASH_HOSTNAME(); \
            new_entry->set = true;                                                         \
        }                                                                                  \
                                                                                           \
        if (_cache->size != 1)                                                             \
        {                                                                                  \
            fprintf(stderr, "EP cache size is %ld instead of 1\n", _cache->size);          \
            goto error_out;                                                                \
        }                                                                                  \
    } while (0)

#define CHECK_CACHE(_engine)                                                             \
    do                                                                                   \
    {                                                                                    \
        cache_t *_cache = &(_engine->procs_cache);                                       \
        group_uid_t group_uid;                                                           \
        group_cache_t *gp42 = NULL;                                                      \
        group_uid = 42;                                                                  \
        gp42 = GET_GROUP_CACHE(_cache, group_uid);                                       \
        if (gp42->initialized == false)                                                  \
        {                                                                                \
            fprintf(stderr, "target group is not marked as initialized");                \
            goto error_out;                                                              \
        }                                                                                \
                                                                                         \
        peer_cache_entry_t *cache_entries = (peer_cache_entry_t *)gp42->ranks.base;      \
        size_t i;                                                                        \
        for (i = 0; i < NUM_CACHE_ENTRIES; i++)                                          \
        {                                                                                \
            if (cache_entries[i].peer.proc_info.group_rank != i)                         \
            {                                                                            \
                fprintf(stderr, "cache entry as rank %ld instead of %ld\n",              \
                        cache_entries[i].peer.proc_info.group_rank, i);                  \
                goto error_out;                                                          \
            }                                                                            \
                                                                                         \
            /* 42 because we can (avoid initialization to zero to be assumed all set) */ \
            if (cache_entries[i].peer.proc_info.group_uid != 42)                         \
            {                                                                            \
                fprintf(stderr, "cache entry as group ID %d instead of 42\n",            \
                        cache_entries[i].peer.proc_info.group_uid);                      \
                goto error_out;                                                          \
            }                                                                            \
        }                                                                                \
    } while (0)

#endif // _TEST_CACHE_COMMON_H_