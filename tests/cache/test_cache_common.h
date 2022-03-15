//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef _TEST_CACHE_COMMON_H_
#define _TEST_CACHE_COMMON_H_

#define NUM_CACHE_ENTRIES (10) //(DEFAULT_NUM_PEERS * 2)

#define POPULATE_CACHE(_engine)                                                                  \
    do                                                                                           \
    {                                                                                            \
        /* Create local cache entries */                                                         \
        cache_t *_cache = &(_engine->procs_cache);                                               \
        size_t i;                                                                                \
        for (i = 0; i < NUM_CACHE_ENTRIES; i++)                                                  \
        {                                                                                        \
            peer_cache_entry_t *new_entry;                                                       \
            DYN_LIST_GET(_engine->free_peer_cache_entries, peer_cache_entry_t, item, new_entry); \
            if (new_entry == NULL)                                                               \
            {                                                                                    \
                fprintf(stderr, "Unable to get cache entry #%ld\n", i);                          \
                goto error_out;                                                                  \
            }                                                                                    \
                                                                                                 \
            new_entry->peer.proc_info.group_rank = i;                                            \
            new_entry->peer.proc_info.group_id = 42;                                             \
            new_entry->set = true;                                                               \
            SET_PEER_CACHE_ENTRY(_cache, new_entry);                                             \
        }                                                                                        \
                                                                                                 \
        if (_cache->size != 1)                                                                   \
        {                                                                                        \
            fprintf(stderr, "EP cache size is %ld instead of 1\n", _cache->size);                \
            goto error_out;                                                                      \
        }                                                                                        \
    } while (0)

#define CHECK_CACHE(_engine)                                                             \
    do                                                                                   \
    {                                                                                    \
        group_cache_t *gp_caches = (group_cache_t *)_engine->procs_cache.data.base;      \
        group_cache_t *gp42 = &(gp_caches[42]);                                          \
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
            if (cache_entries[i].peer.proc_info.group_id != 42)                          \
            {                                                                            \
                fprintf(stderr, "cache entry as rank %ld instead of 42\n",               \
                        cache_entries[i].peer.proc_info.group_id);                       \
                goto error_out;                                                          \
            }                                                                            \
        }                                                                                \
                                                                                         \
    } while (0)

#endif // _TEST_CACHE_COMMON_H_