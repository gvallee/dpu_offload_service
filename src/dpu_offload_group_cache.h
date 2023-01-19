#ifndef DPU_OFFLOAD_GROUP_CACHE_H_

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
                                              uint64_t sp_gp_giuid,
                                              uint64_t *sp_gp_lid);

/**
 * @brief Get the host ID within a group of the current associated host.
 * That identifier is based on a ordered, contiguous list of all the 
 * hosts involved in the group. The list is based on the ordered of all
 * the hosts that MIMOSA creates from the job configuration (in most
 * cases, the order from the configuration file). All host in the list
 * are numbered from 0 to the number of hosts involved in the group.
 * 
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in,out] host_id Host identifier from the ordered list of hosts that are involved in the group
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_ordered_host_id_by_group(offloading_engine_t *engine,
                                                  group_uid_t group_uid,
                                                  host_info_t *host_id);

/**
 * @brief Get the number of service process on a specific host within a specific group.
 * 
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_id Target host identifier
 * @param[in,out] num_sps Returned number of service process on the target host;
 * returns 0 is the target host is not involved in the group
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_num_sps_by_group_host(offloading_engine_t *engine,
                                               group_uid_t group_uid,
                                               host_info_t host_id,
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
 * @brief Get the num ranks running on a target host within a specific group.
 * 
 * @param[in] engine Associated offload engine
 * @param[in] group_uid Target group identified by its unique group identifier
 * @param[in] host_id Target host within the group
 * @param[in,out] num_ranks Returned number of ranks or 0 if the host is not involved in the group
 * @return dpu_offload_status_t DO_SUCCESS or DO_ERROR if case of an error during execution of the function.
 * If the host is not part of the group, it does not raise an exception.
 */
dpu_offload_status_t get_num_ranks_for_group_host(offloading_engine_t *engine,
                                                  group_uid_t group_uid,
                                                  host_info_t host_id,
                                                  size_t *num_ranks);

dpu_offload_status_t get_n_for_rank_by_group_host(offloading_engine_t *engine,
                                                  group_uid_t group_uid,
                                                  host_info_t my_host_id,
                                                  int64_t *idx);

dpu_offload_status_t get_all_sps_by_group_host(offloading_engine_t *engine,
                                               group_uid_t group_uid,
                                               host_info_t host_id,
                                               dyn_array_t *sps,
                                               size_t *num_sps);

dpu_offload_status_t get_all_hosts_by_group(offloading_engine_t *engine,
                                            group_uid_t group_uid,
                                            dyn_array_t *hosts,
                                            size_t *num_hosts);

dpu_offload_status_t get_all_ranks_by_group_sp(offloading_engine_t *engine,
                                               group_uid_t group_uid,
                                               uint64_t remote_global_group_sp_id,
                                               dyn_array_t *ranks,
                                               size_t *num_ranks);

dpu_offload_status_t get_nth_sp_by_group_host(offloading_engine_t *engine,
                                              group_uid_t group_uid,
                                              host_info_t host_id,
                                              uint64_t *global_group_sp_id);

#endif // DPU_OFFLOAD_GROUP_CACHE_H_