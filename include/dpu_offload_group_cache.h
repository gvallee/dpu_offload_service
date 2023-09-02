//
// Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef DPU_OFFLOAD_GROUP_CACHE_H_

#define GROUP_SIZE_UNKNOWN (-1)

/* GROUPS_CACHE_INIT initializes the cache that holds information about all the groups */
#define GROUPS_CACHE_INIT(_cache)                                                            \
    do                                                                                       \
    {                                                                                        \
        RESET_CACHE(_cache);                                                                 \
        (_cache)->data = kh_init(group_hash_t);                                              \
        DYN_LIST_ALLOC((_cache)->group_cache_pool, DEFAULT_NUM_GROUPS, group_cache_t, item); \
    } while (0)

#define GROUP_CACHE_TERMINATE_PENDING_LIST(_item, _pool)                    \
    do                                                                      \
    {                                                                       \
        if ((_item)->ev != NULL)                                            \
        {                                                                   \
            if ((_item)->ev->sub_events_initialized)                        \
            {                                                               \
                dpu_offload_event_t *_subev, *_next_subev;                  \
                ucs_list_for_each_safe (_subev, _next_subev,                \
                                        &((_item)->ev->sub_events), item)   \
                {                                                           \
                    ucs_list_del(&(_subev->item));                          \
                    event_return(&_subev);                                  \
                }                                                           \
            }                                                               \
            event_return(&((_item)->ev));                                   \
            DYN_LIST_RETURN((_pool),                                        \
                            (_item),                                        \
                            item);                                          \
            }                                                               \
    } while (0)

#define GROUP_CACHE_TERMINATE_PENDING_MSGS(_gp_cache)                                                           \
    do                                                                                                          \
    {                                                                                                           \
        pending_send_group_add_t *_pending_add_send = NULL, *_next_pending_add_send = NULL;                     \
        group_revoke_msg_from_sp_t *_pending_revoke_from_sp = NULL, *_next_pending_revoke_from_sp = NULL;       \
        group_revoke_msg_from_rank_t *_pending_revoke_from_rank = NULL, *_next_pending_revoke_from_rank = NULL; \
        pending_group_add_t *_pending_group_add = NULL, *_next_pending_group_add = NULL;                        \
        pending_recv_cache_entry_t *_pending_cache_entry = NULL, *_next_pending_cache_entry = NULL;             \
        /* Purge pending messages of the group since they are now irrelevant. */                                \
        /* This happens when the app is creating short-lived sub-communicators that */                          \
        /* are not used for offloading and destroyed too quickly for revoke message */                          \
        /* to go through the entire system before the app termination. */                                       \
        ucs_list_for_each_safe(_pending_revoke_from_sp, _next_pending_revoke_from_sp,                           \
                               &((_gp_cache)->persistent.pending_group_revoke_msgs_from_sps),                   \
                               item)                                                                            \
        {                                                                                                       \
            ucs_list_del(&(_pending_revoke_from_sp->item));                                                     \
            DYN_LIST_RETURN((_gp_cache)->engine->pool_group_revoke_msgs_from_sps,                               \
                            _pending_revoke_from_sp,                                                            \
                            item);                                                                              \
        }                                                                                                       \
        ucs_list_for_each_safe(_pending_revoke_from_rank, _next_pending_revoke_from_rank,                       \
                               &((_gp_cache)->persistent.pending_group_revoke_msgs_from_ranks),                 \
                               item)                                                                            \
        {                                                                                                       \
            ucs_list_del(&(_pending_revoke_from_rank->item));                                                   \
            DYN_LIST_RETURN((_gp_cache)->engine->pool_group_revoke_msgs_from_ranks,                             \
                            _pending_revoke_from_rank,                                                          \
                            item);                                                                              \
        }                                                                                                       \
        ucs_list_for_each_safe(_pending_group_add, _next_pending_group_add,                                     \
                               &((_gp_cache)->persistent.pending_group_add_msgs),                               \
                               item)                                                                            \
        {                                                                                                       \
            ucs_list_del(&(_pending_group_add->item));                                                          \
            DYN_LIST_RETURN((_gp_cache)->engine->pool_pending_recv_group_add,                                   \
                            _pending_group_add,                                                                 \
                            item);                                                                              \
        }                                                                                                       \
        ucs_list_for_each_safe(_pending_add_send, _next_pending_add_send,                                       \
                               &((_gp_cache)->persistent.pending_send_group_add_msgs),                          \
                               item)                                                                            \
        {                                                                                                       \
            ucs_list_del(&(_pending_add_send->item));                                                           \
            GROUP_CACHE_TERMINATE_PENDING_LIST(_pending_add_send,                                               \
                                               (_gp_cache)->engine->pool_pending_send_group_add);               \
        }                                                                                                       \
        ucs_list_for_each_safe(_pending_cache_entry, _next_pending_cache_entry,                                 \
                               &((_gp_cache)->persistent.pending_recv_cache_entries),                           \
                               item)                                                                            \
        {                                                                                                       \
            ucs_list_del(&(_pending_cache_entry->item));                                                        \
            DYN_LIST_RETURN((_gp_cache)->engine->pool_pending_recv_cache_entries,                               \
                            _pending_cache_entry,                                                               \
                            item);                                                                              \
        }                                                                                                       \
    } while (0)

#if NDEBUG
#define GROUP_CACHE_TERMINATE_PERSISTENT_DATA(_gp_cache)                                \
    do                                                                                  \
    {                                                                                   \
        GROUP_CACHE_TERMINATE_PENDING_MSGS(_gp_cache);                                  \
    } while (0)
#else
#define GROUP_CACHE_TERMINATE_PERSISTENT_DATA(_gp_cache)                                            \
    do                                                                                              \
    {                                                                                               \
        GROUP_CACHE_TERMINATE_PENDING_MSGS(_gp_cache);                                              \
        if (!ucs_list_is_empty(&(_gp_cache)->persistent.pending_group_revoke_msgs_from_sps))        \
        {                                                                                           \
            WARN_MSG("pending_group_revoke_msgs_from_sps for group 0x%x is NOT empty",              \
                     (_gp_cache)->group_uid);                                                       \
        }                                                                                           \
        if (!ucs_list_is_empty(&(_gp_cache)->persistent.pending_group_revoke_msgs_from_ranks))      \
        {                                                                                           \
            WARN_MSG("pending_group_revoke_msgs_from_ranks for group 0x%x is NOT empty",            \
                     (_gp_cache)->group_uid);                                                       \
        }                                                                                           \
        if (!ucs_list_is_empty(&(_gp_cache)->persistent.pending_group_add_msgs))                    \
        {                                                                                           \
            WARN_MSG("pending_group_add_msgs for group 0x%x is NOT empty",                          \
                     (_gp_cache)->group_uid);                                                       \
        }                                                                                           \
        if (!ucs_list_is_empty(&(_gp_cache)->persistent.pending_send_group_add_msgs))               \
        {                                                                                           \
            WARN_MSG("pending_send_group_add_msgs for group 0x%x is NOT empty",                     \
                     (_gp_cache)->group_uid);                                                       \
        }                                                                                           \
        if (!ucs_list_is_empty(&(_gp_cache)->persistent.pending_recv_cache_entries))                \
        {                                                                                           \
            WARN_MSG("pending_recv_cache_entries for group 0x%x is NOT empty",                      \
                     (_gp_cache)->group_uid);                                                       \
        }                                                                                           \
    } while (0)
#endif // NDEBUG

// Macro called during the termination of the engine to free all remaining
// allocated data
#define GROUPS_CACHE_FINI(_cache)                                           \
    do                                                                      \
    {                                                                       \
        int key;                                                            \
        group_cache_t *value = NULL;                                        \
        /* kh_foreach is a little picky so putting some safeguards */       \
        assert((_cache)->data);                                             \
        if (kh_size((_cache)->data) > 0)                                    \
        {                                                                   \
            kh_foreach((_cache)->data, key, value, {                        \
                if (value != NULL)                                          \
                {                                                           \
                    group_cache_t *_gp_cache = NULL;                        \
                    _gp_cache = GET_GROUP_CACHE((_cache), key);             \
                    assert(_gp_cache);                                      \
                    if (_gp_cache->rank_array_initialized)                  \
                    {                                                       \
                        DYN_ARRAY_FREE(&(_gp_cache->ranks));                \
                        _gp_cache->rank_array_initialized = false;          \
                    }                                                       \
                    if (_gp_cache->sp_array_initialized)                    \
                    {                                                       \
                        DYN_ARRAY_FREE(&(_gp_cache->sps));                  \
                        _gp_cache->sp_array_initialized = false;            \
                    }                                                       \
                    if (_gp_cache->host_array_initialized)                  \
                    {                                                       \
                        DYN_ARRAY_FREE(&(_gp_cache->hosts));                \
                        _gp_cache->host_array_initialized = false;          \
                    }                                                       \
                    /* Free hash table(s) */                                \
                    GROUP_CACHE_HASHES_FINI((_cache)->engine, _gp_cache);   \
                    /* Free the bitset for SPs */                           \
                    GROUP_CACHE_BITSET_DESTROY(_gp_cache->sps_bitset);      \
                    /* Free the bitset for Hosts */                         \
                    GROUP_CACHE_BITSET_DESTROY(_gp_cache->hosts_bitset);    \
                    /* Free the persistent data of the group cache */       \
                    GROUP_CACHE_TERMINATE_PERSISTENT_DATA(_gp_cache);       \
                }                                                           \
            }) kh_destroy(group_hash_t, (_cache)->data);                    \
        }                                                                   \
        else                                                                \
        {                                                                   \
            kh_destroy(group_hash_t, (_cache)->data);                       \
        }                                                                   \
        DYN_LIST_FREE((_cache)->group_cache_pool, group_cache_t, item);     \
        (_cache)->group_cache_pool = NULL;                                  \
        (_cache)->size = 0;                                                 \
    } while (0)

/* GROUP_CACHE_INIT initializes the cache for a given group */
#define GROUP_CACHE_INIT(_cache, _gp_id)                                     \
    do                                                                       \
    {                                                                        \
        void *_gp_cache = _cache[_gp_id];                                    \
        DYN_ARRAY_ALLOC((dyn_array_t *)_gp_cache, 2048, peer_cache_entry_t); \
    } while (0)

// Handles the pending cache entries we received from other SPs
#define HANDLE_PENDING_CACHE_ENTRIES(_gp_cache)                                                         \
    do                                                                                                  \
    {                                                                                                   \
        int _rc;                                                                                        \
        pending_recv_cache_entry_t *_pending_cache_entry = NULL, *_next_pending = NULL;                 \
        DBG("Handling pending cache entries for group 0x%x (seq num: %ld)",                             \
            (_gp_cache)->group_uid, (_gp_cache)->persistent.num);                                       \
        ucs_list_for_each_safe(_pending_cache_entry,                                                    \
                               _next_pending,                                                           \
                               &((_gp_cache)->persistent.pending_recv_cache_entries),                   \
                               item)                                                                    \
        {                                                                                               \
            ucs_list_del(&(_pending_cache_entry->item));                                                \
            _rc = handle_peer_cache_entries_recv(_pending_cache_entry->econtext,                        \
                                                 _pending_cache_entry->sp_gid,                          \
                                                 _pending_cache_entry->payload,                         \
                                                 _pending_cache_entry->payload_size);                   \
            CHECK_ERR_RETURN((_rc != DO_SUCCESS), DO_ERROR, "handle_peer_cache_entries_recv() failed"); \
            if (_pending_cache_entry->payload)                                                          \
            {                                                                                           \
                free(_pending_cache_entry->payload);                                                    \
                _pending_cache_entry->payload = NULL;                                                   \
            }                                                                                           \
            DYN_LIST_RETURN((_gp_cache)->engine->pool_pending_recv_cache_entries,                       \
                            _pending_cache_entry,                                                       \
                            item);                                                                      \
        }                                                                                               \
    } while (0)

// Handles the pending revoke messages we received from other SPs
#define HANDLE_PENDING_GROUP_REVOKE_MSGS_FROM_SPS(_gp_cache, _total_new_revokes)                \
    do                                                                                          \
    {                                                                                           \
        group_revoke_msg_from_sp_t *_pending_revoke_msg = NULL, *_next_pending = NULL;          \
        assert(_gp_cache);                                                                      \
        _total_new_revokes = 0;                                                                 \
        ucs_list_for_each_safe(_pending_revoke_msg,                                             \
                               _next_pending,                                                   \
                               &((_gp_cache)->persistent.pending_group_revoke_msgs_from_sps),   \
                               item)                                                            \
        {                                                                                       \
            size_t _rank;                                                                       \
            size_t _new_revokes;                                                                \
            DBG("Handling queued revoke message (group UID: 0x%x)",                             \
                _pending_revoke_msg->gp_uid);                                                   \
            ucs_list_del(&(_pending_revoke_msg->item));                                         \
            /* Make sure it is an applicable message, if not, drop. */                          \
            /* Note: with the group UID we have a unique */                                     \
            /* way to identify group, regardless of asynchronous events and resuse */           \
            /* of group ID (for example reuse of MPI communicator IDs). */                      \
            assert(_pending_revoke_msg->gp_uid != INT_MAX);                                     \
            assert(_pending_revoke_msg->gp_seq_num == (_gp_cache)->persistent.num);             \
            /* Update the local list of ranks that revoked the group based on the data */       \
            /* from the message */                                                              \
            if (gp_cache->revokes.ranks == NULL)                                                \
            {                                                                                   \
                /* This bitset always has a lazy initialization, like a group cache */          \
                assert((_gp_cache)->group_size != 0);                                           \
                GROUP_CACHE_BITSET_CREATE((_gp_cache)->revokes.ranks, (_gp_cache)->group_size); \
            }                                                                                   \
            assert((_gp_cache)->revokes.ranks);                                                 \
            _new_revokes = 0;                                                                   \
            for (_rank = 0; _rank < _pending_revoke_msg->num_ranks; _rank++)                    \
            {                                                                                   \
                size_t _idx = _pending_revoke_msg->rank_start + _rank;                          \
                if (_pending_revoke_msg->ranks[_rank] == 1 &&                                   \
                    !GROUP_CACHE_BITSET_TEST((_gp_cache)->revokes.ranks, _idx))                 \
                {                                                                               \
                    DBG("Marking rank %ld has revoking the group (seq num: %ld)",               \
                        _idx, _pending_revoke_msg->gp_seq_num);                                 \
                    GROUP_CACHE_BITSET_SET((_gp_cache)->revokes.ranks, _idx);                   \
                    _new_revokes++;                                                             \
                }                                                                               \
            }                                                                                   \
            (_gp_cache)->revokes.global += _new_revokes;                                        \
            _total_new_revokes += _new_revokes;                                                 \
            DYN_LIST_RETURN((_gp_cache)->engine->pool_group_revoke_msgs_from_sps,               \
                            _pending_revoke_msg,                                                \
                            item);                                                              \
        }                                                                                       \
    } while (0)

/**
 * @brief Checks whether a rank in a given group is in the cache.
 *
 * @param cache Pointer to the target cache to query
 * @param gp_uid Target group's UID
 * @param rank_id Target rank in the group
 * @param group_size Group size (can be GROUP_SIZE_UNKNOWN)
 * @return true
 * @return false
 */
bool is_in_cache(cache_t *cache, group_uid_t gp_uid, int64_t rank_id, int64_t group_size);

/**
 * @brief This function populated the cache's lookup tables. It assumes the cache is
 * fully ppopulated.
 *
 * @param[in] engine Associated offload engine
 * @param[in] gp_cache Group cache for which we need to populated the lookup tables.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t populate_group_cache_lookup_table(offloading_engine_t *engine,
                                                       group_cache_t *gp_cache);

/**
 * @brief Get the global service process identifier within a group. Note that the
 * group global service process identifier differs from the global service process identifer
 * maintained by MIMOSA. The group global service process identifier is specific
 * to a group, a group assigning a contiguous set of identifiers to
 * all service processs that are involved in the said group. Ordering is based on
 * global ordering, i.e., the ordering set by MIMOSA during bootstrapping
 * so ultimately based on the MIMOSA-level global ID.
 *
 * @param[in] engine Associated offload engine
 * @param[in] gp_uid Target group identified by its unique group identifier
 * @param[in,out] sp_id Returned group global service process identifier
 * @return dpu_offload_status_t DO_SUCCESS if the current service process is part of group;
 * DO_ERROR when the current service process is not part of the group or if an internal error occurs.
 */
dpu_offload_status_t get_global_sp_id_by_group(offloading_engine_t *engine,
                                               group_uid_t gp_uid,
                                               uint64_t *sp_id);

/**
 * @brief Get the local service process identifier within a group.
 * Based on the definition of group global service process ids, this function
 * returns the local identifier for a specific service process. Local identifiers
 * are contiguous, from 0 to the number of service process on the target DPU, based
 * on the ordering from the group global service process identifiers.
 *
 * @param[in] engine Associated offload engine
 * @param[in] gp_uid Target group identified by its unique group identifier
 * @param[in] sp_gp_guid Group global identifier of the target service process
 * @param[in,out] sp_gp_lid Returned group local service process identifier
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_local_sp_id_by_group(offloading_engine_t *engine,
                                              group_uid_t gp_uid,
                                              uint64_t sp_gp_guid,
                                              uint64_t *sp_gp_lid);

/**
 * @brief Get the host index within a group of the current associated host.
 * That identifier is based on a ordered, contiguous list of all the
 * hosts involved in the group. The list is based on the ordered of all
 * the hosts that MIMOSA creates from the job configuration (in most
 * cases, the order from the configuration file). All host in the list
 * are numbered from 0 to the number of hosts involved in the group.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in,out] host_idx Host index from the ordered list of hosts that are involved in the group.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_host_idx_by_group(offloading_engine_t *engine,
                                           group_uid_t group_uid,
                                           size_t *host_idx);

/**
 * @brief Get the number of service process on a specific host within a specific group.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_idx Target host identifier using the index in the contiguous ordered array of host involved in the group.
 * @param[in,out] num_sps Returned number of service process on the target host;
 * returns 0 is the target host is not involved in the group
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_num_sps_by_group_host_idx(offloading_engine_t *engine,
                                                   group_uid_t group_uid,
                                                   size_t host_idx,
                                                   size_t *num_sps);

/**
 * @brief Get the number of ranks assigned to a specific service process within a group.
 *
 * @param engine Associated offload engine
 * @param group_uid Target group identified by its unique group identifier
 * @param[in] sp_gp_gid Global group service process identifier (for example as returned by get_global_sp_id_by_group())
 * @param[in,out] num_ranks Number of ranks assigned to the target service process;
 * should always be strictly greater than 0 since the global group service process
 * identifier is assumed to be valid for the target group
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_num_ranks_for_group_sp(offloading_engine_t *engine,
                                                group_uid_t group_uid,
                                                uint64_t sp_gp_gid,
                                                size_t *num_ranks);

/**
 * @brief Get the number of ranks running assigned to a specific service process that
 * is identifier through its local group identifier (local host service process identifier)
 * and the host index from the contiguous ordered array of hosts.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_idx Index of the host from the contiguous ordered array of hosts involved in the group
 * @param[in] local_host_sp_id Index of the service process from the contiguous ordered list of service processes associated with the host
 * @param[in,out] num_ranks Number of ranks assigned to the target service process; should
 * always be strictly greater than 0 since the local service process identifier has to be
 * valid otherwise the service process would not be in the contiguous ordered array of service
 * processes being involved. The host index is assumed to be similarily always defined and valid.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_num_ranks_for_group_host_local_sp(offloading_engine_t *engine,
                                                           group_uid_t group_uid,
                                                           size_t host_idx,
                                                           uint64_t local_host_sp_id,
                                                           size_t *num_ranks);

/**
 * @brief Get the number of ranks running on a target host within a specific group. The host is
 * identified by its index in the contiguous ordered array of all the hosts involved in the group.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_idx Index of the host from the contiguous ordered array of hosts involved in the group
 * @param[in,out] num_ranks Returned number of ranks or 0 if the host is not involved in the group
 * @return dpu_offload_status_t DO_SUCCESS or DO_ERROR if case of an error during execution of the function.
 * If the host is not part of the group, it does not raise an exception.
 */
dpu_offload_status_t get_num_ranks_for_group_host_idx(offloading_engine_t *engine,
                                                      group_uid_t group_uid,
                                                      size_t host_idx,
                                                      size_t *num_ranks);

/**
 * @brief Get the index associated to a rank for the contiguous, sorted array of ranks running on the target host.
 * The host is identified by its index in the contiguous ordered array of hosts involved in the group.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_idx Index of the host from the contiguous ordered array of hosts involved in the group
 * @param[in] rank Target rank in the group
 * @param[in,out] idx Returned index associated to the rank from the array of ranks running on the host.
 * If the rank is not running on the host or beyond the group size, an internal error is raised
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_rank_idx_by_group_host_idx(offloading_engine_t *engine,
                                                    group_uid_t group_uid,
                                                    size_t host_idx,
                                                    int64_t rank,
                                                    uint64_t *idx);

/**
 * @brief Get the rank index of a given rank associated to a service process on a specific node, all within a group.
 *
 * @param engine Associated offload engine
 * @param group_uid Target group identified by its unique group identifier
 * @param sp_gp_gid Global group service process identifier (for example as returned by get_global_sp_id_by_group())
 * @param rank Rank in the communicator/group
 * @param rank_idx Index of the rank in the contiguous ordered array of ranks associated with the target service process
 * @return dpu_offload_status_t DO_ERROR if the rank is not associated to the service process or in case of an internat error. If the rank is not associated to the service process, the rank index is set to UINT32_MAX.
 */
dpu_offload_status_t get_rank_idx_by_group_sp_id(offloading_engine_t *engine,
                                                 group_uid_t group_uid,
                                                 uint64_t sp_gp_gid,
                                                 int64_t rank,
                                                 size_t *rank_idx);

/**
 * @brief Get all the service processes involved in a group that are associated to a specific host.
 * The host is identified via its index in the contiguous ordered list of hosts involed in the group.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_idx Index of the host from the contiguous ordered array of hosts involved in the group
 * @param[in,out] sps Pointer to the internal lookup array associated to the list of service processes for the target host.
 * Since the table is exposed directly from the cache, the caller must not free the array, it will be freed when the group cache is freed.
 * If no service process on the target host is involved in the group, the pointer is set to NULL and no exception is raised.
 * The array is composed of pointers of pointer of sp_cache_data_t structures.
 * @param[in,out] num_sps Number of service processes associated to the target host within the group;
 * 0 if no service process on the host is involved in the group.
 * @return dpu_offload_status_t
 *
 * @code{.unparsed}
 *  // Get the data of the first SP involved in the group and associated to the first host
 *  dyn_array_t *sps = NULL;
 *  sp_cache_data_t **sp_data = NULL;
 *  size_t n_sps;
 *  get_all_sps_by_group_host_idx(engine, group_uid, 0, &sps, &n_sps);
 *  sp_data = DYN_ARRAY_GET_ELT(sps, 0, sp_cache_data_t *);
 */
dpu_offload_status_t get_all_sps_by_group_host_idx(offloading_engine_t *engine,
                                                   group_uid_t group_uid,
                                                   size_t host_idx,
                                                   dyn_array_t **sps,
                                                   size_t *num_sps);

/**
 * @brief Get all the hosts involved in a group. The resulting array is contiguous and ordered.
 * Ordering is based on the configuration that is loaded at bootstrapping time; in most cases
 * based on the content of the configuration file used for the job.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in,out] hosts Pointer to the internal lookup array associated to the list of hosts that are involved in the group.
 * The caller must not free the array at any point since it is an internal array directly exposed to users. It is freed when
 * the group cache is being freed. The array is composed of pointers of pointer of host_info_t structures.
 * @param[in,out] num_hosts Number of hosts involed in the group. The returned value must strictly be greater than 0 since groups
 * are created only when a rank from the group triggers its creatiom, guaranteeing that all known groups must have a minimum of 1 rank.
 * @return dpu_offload_status_t
 *
 * @code{.unparsed}
 *  // Get the data of the first host involved in the group
 *  dyn_array_t *hosts = NULL;
 *  host_info_t **host_data = NULL;
 *  size_t n_hosts;
 *  get_all_hosts_by_group(engine, group_uid, &hosts, *n_hosts);
 *  host_data = DYN_ARRAY_GET_ELT(hosts, 0, host_info_t *);
 */
dpu_offload_status_t get_all_hosts_by_group(offloading_engine_t *engine,
                                            group_uid_t group_uid,
                                            dyn_array_t **hosts,
                                            size_t *num_hosts);

/**
 * @brief Get all the ranks associated to a specific service process involed in a given group, using the
 * service process group global id.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] sp_group_gid Global group service process identifer, as for example returned by get_local_sp_id_by_group(). Must be valid.
 * @param[in,out] ranks Pointer to the internal lookup array associated to the list of ranks associated with a given service process.
 * The caller must not free the array at any point since it is an internal array directly exposed to users. It is freed when
 * the group cache is being freed. The array is composed of pointers of pointer of peer_cache_entry_t structures.
 * @param[in,out] num_ranks Number of ranks associated to the target service process. The returned value must strictly be greater than
 * 0 since the global group service identifier is assumed valid and therefore a strict minimum of one rank must be associated to the
 * service process (otherwise the global group service process would be invalid)
 * @return dpu_offload_status_t
 *
 * @code{.unparsed}
 *  // Get the data of the first rank involved in the group and associated with the first SP in the group
 *  peer_cache_entry_t **rank_data = NULL;
 *  dyn_array_t *ranks = NULL;
 *  size_t n_ranks;
 *  get_all_ranks_by_group_sp_gid(engine, group_uid, 0, &ranks, &n_ranks);
 *  rank_data = DYN_ARRAY_GET_ELT(ranks, 0, peer_cache_entry_t *)
 */
dpu_offload_status_t get_all_ranks_by_group_sp_gid(offloading_engine_t *engine,
                                                   group_uid_t group_uid,
                                                   uint64_t sp_group_gid,
                                                   dyn_array_t **ranks,
                                                   size_t *num_ranks);

/**
 * @brief Get all the ranks associated to a specific service process involed in a given group, using the
 * service process group global id.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_idx Index of the host from the contiguous ordered array of hosts involved in the group.
 * @param[in] sp_group_lid Local group service process identifer, as for example returned by get_local_sp_id_by_group(). Must be valid.
 * @param[in,out] ranks Pointer to the internal lookup array associated to the list of ranks associated with a given service process.
 * The caller must not free the array at any point since it is an internal array directly exposed to users. It is freed when
 * the group cache is being freed. The array is composed of pointers of pointer of peer_cache_entry_t structures.
 * @param[in,out] num_ranks Number of ranks associated to the target service process. The returned value must strictly be greater than
 * 0 since the global group service identifier is assumed valid and therefore a strict minimum of one rank must be associated to the
 * service process (otherwise the global group service process would be invalid)
 * @return dpu_offload_status_t
 *
 * @code{.unparsed}
 *  // Get the list of ranks associated to the first SP on the first host that is involved in the group
 *  dyn_array_t *ranks = NULL;
 *  size_t n_ranks;
 *  peer_cache_entry_t **rank_info = NULL;
 *  get_all_ranks_by_group_sp_lid(engine, group_uid, 0, 0, &ranks, &n_ranks);
 *  ranks_info = DYN_ARRAY_GET_ELT(ranks, 0, peer_cache_entry_t *);
 */
dpu_offload_status_t get_all_ranks_by_group_sp_lid(offloading_engine_t *engine,
                                                   group_uid_t group_uid,
                                                   size_t host_idx,
                                                   uint64_t sp_group_lid,
                                                   dyn_array_t **ranks,
                                                   size_t *num_ranks);

/**
 * @brief Get the n^th service process on a given host that is involved in the target group.
 *
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_idx Index of the host from the contiguous ordered array of hosts involved in the group.
 * @param[in] n Which service process on the host to look up (0 indexed)
 * @param[in,out] sp_group_gid Global group identifier of the target service process
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_nth_sp_by_group_host_idx(offloading_engine_t *engine,
                                                  group_uid_t group_uid,
                                                  size_t host_idx,
                                                  size_t n,
                                                  uint64_t *global_group_sp_id);

/**
 * @brief Convert a global service process identifier to a group global identifier
 * 
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] sp_gid Service process global identifier (set of startup time)
 * @param[in,out] sp_gp_gid Global group identifier of the service process
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_sp_group_gid(offloading_engine_t *engine,
                                      group_uid_t group_uid,
                                      uint64_t sp_gid,
                                      uint64_t *sp_gp_gid);

/**
 * @brief Checks whether a group's cache is fully populated
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @return true
 * @return false
 */
bool group_cache_populated(offloading_engine_t *engine, group_uid_t gp_uid);

/**
 * @brief Handle data related to a new rank in a group and make sure the group cache's topology
 * is properly updated.
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] group_gp Target group cache
 * @param[in] group_rank New rank to be added to the cache
 * @param[in] sp_id Global identifier of the service process assigned to the new rank
 * @param[in] host_uid Unique identifier of the host where the rank is running
 */
dpu_offload_status_t update_topology_data(offloading_engine_t *engine, group_cache_t *gp_cache, int64_t group_rank, uint64_t sp_gid, host_uid_t host_uid);

/**
 * @brief Function that can be called on the host to add a local rank to the local cache.
 * 
 * @param[in] engine Offloading engine associated to the target cache
 * @param[in] rank_info Data about the rank to add to the cache
 * @return dpu_offload_status_t
 */
dpu_offload_status_t host_add_local_rank_to_cache(offloading_engine_t *engine, rank_info_t *rank_info);


/**
 * @brief Get the dpu ID by host rank object. That ID can then be used to look up the corresponding endpoint.
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank Target rank in the group
 * @param[in] sp_idx In case of multiple service processes per host, index of the target shadow service process for the group/rank
 * @param[out] cb Associated callback. If the event completes right away, the callback is still being invoked. The user is in charge of returning the object related to the request.
 * @return dpu_offload_status_t
 *
 * Example:
 *      - To issue the request for the first service process attached to the group/rank `gp_id` and `rank_id`
 *          get_cache_entry_by_group_rank(offload_engine, gp_id, rank_id, 0, my_completion_cb);
 *      - Completion callback example:
 *          void my_completion_cb(void *data)
 *          {
 *              assert(data);
 *              cache_entry_request_t *cache_entry_req = (cache_entry_request_t*)data;
 *              assert(cache_entry_req->offload_engine);
 *              offloading_engine_t *engine = (offloading_engine_t*)cache_entry_req->offload_engine;
 *              ucp_ep_h target_sp_ep = NULL;
 *              execution_context_t *target_sp_econtext = NULL;
 *              uint64_t notif_dest_id;
 *              get_sp_ep_by_id(engine, cache_entry_req->target_sp_idx, &target_sp_econtext, &target_sp_econtext, &notif_dest_id);
 *              assert(target_sp_ep == NULL);
 *              DYN_LIST_RETURN(engine->free_cache_entry_requests, cache_entry_req, item);
 *          }
 */
dpu_offload_status_t get_cache_entry_by_group_rank(offloading_engine_t *engine, group_uid_t gp_uid, int64_t rank, int64_t sp_idx, request_compl_cb_t cb);

/**
 * @brief Get the global service process ID (not the group service process global ID) that is associated with a specific rank in a group.
 * The global ID can then be used to look up the corresponding endpoint, for example to issue a XGVMI operation.
 * The global identifier is the identifier set a startup time; it is not a group-level identifier.
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank Target rank in the group
 * @param[in] dpu_idx In case of multiple service processes per host, index of the target shadow service process for the group/rank
 * @param[out] dpu_id Resulting service process identifier
 * @param[out] ev Associated event. If NULL, the DPU identifier is available right away. If not, it is required to call the function again once the event has completed. The caller is in charge of returning the event after completion. The event cannot be added to any list since it is already put on a list.
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_sp_id_by_group_rank(offloading_engine_t *engine,
                                             group_uid_t gp_uid,
                                             int64_t rank,
                                             int64_t sp_idx,
                                             int64_t *sp_id,
                                             dpu_offload_event_t **ev);

/**
 * @brief Get the group ranks on a host from the local cache
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] host_id Target host ID
 * @param[in,out] n_ranks Pointer to the variable that will hold the number of ranks on the target host
 * @param[in,out] ranks Pointer to the dynamic array of int64_t that will store all the ranks
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_group_ranks_on_host(offloading_engine_t *engine,
                                             group_uid_t gp_uid,
                                             uint64_t host_id,
                                             size_t *n_ranks,
                                             dyn_array_t *ranks);

/**
 * @brief Get the list of all local SPs for a given rank in a group.
 * Note that the function returns all the local SPs, including the SPs
 * the rank may not be directly connected to.
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank Target rank in the group
 * @param[in,out] n_sps Pointer to the variable that will hold the number of local SPs
 * @param[in,out] sps Pointer to the dynamic array of uint64_t that will store all the service processes global ID
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_group_rank_sps(offloading_engine_t *engine,
                                        group_uid_t gp_uid,
                                        uint64_t rank,
                                        size_t *n_sps,
                                        dyn_array_t **sps);

/**
 * @brief Get the local SPs for a given group. Can be used only is the context of SPs.
 * This can be used to know how many and which local SPs are involved in a given group.
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in,out] n_sps Pointer to the variable that will hold the number of local SPs
 * @param[in,out] sps Pointer to the dunamic array of uint64_t that will store all the service processes global ID
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_group_local_sps(offloading_engine_t *engine,
                                         group_uid_t gp_uid,
                                         size_t *n_sps,
                                         dyn_array_t *sps);

/**
 * @brief Get the group rank host identifier from the local cache
 *
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank Target rank in the group
 * @param[out] host_id ID of the host where the rank is running (64-bit hash)
 * @return dpu_offload_status_t
 */
dpu_offload_status_t get_group_rank_host(offloading_engine_t *engine,
                                         group_uid_t gp_uid,
                                         int64_t rank,
                                         uint64_t *host_id);

/**
 * @brief Checks whether two ranks of a same group are on the same host.
 * 
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank1 First rank to use for the comparison
 * @param[in] rank2 Second rank to use for the comparison
 * @return false if the two ranks are not on the same host or in the context of an error
 * @return true if the two ranks are on the same host
 */
bool on_same_host(offloading_engine_t *engine,
                  group_uid_t gp_uid,
                  int64_t rank1,
                  int64_t rank2);

/**
 * @brief Checks whether two ranks of a same group are associated to the same service process
 * 
 * @param[in] engine Offloading engine for the query
 * @param[in] gp_uid Target group's UID
 * @param[in] rank1 First rank to use for the comparison
 * @param[in] rank2 Second rank to use for the comparison
 * @return false if the two ranks are not associated to the same service process or in the context of an error
 * @return true if the two ranks are associated to the same service process
 */
bool on_same_sp(offloading_engine_t *engine,
                group_uid_t gp_uid,
                int64_t rank1,
                int64_t rank2);

void display_group_cache(cache_t *cache, group_uid_t gp_uid);

#endif // DPU_OFFLOAD_GROUP_CACHE_H_