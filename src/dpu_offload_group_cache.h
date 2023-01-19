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
 * @brief Get the global SP identifier within a group. Note that the
 * group global SP identifier differs from the global SP identifer
 * maintained by MIMOSA. The group global SP identifier is specific
 * to a group, a group assigning a contiguous set of identifiers to
 * all SPs that are involved in the said group. Ordering is based on
 * global ordering, i.e., the ordering set by MIMOSA during bootstrapping
 * so ultimately based on the MIMOSA-level global ID.
 * 
 * @param[in] engine Associated offload engine
 * @param[in] gp_uid Target group identified by its unique group identifier
 * @param[in,out] sp_id Returned group global SP identifier
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_global_sp_id_by_group(offloading_engine_t *engine,
                                               group_uid_t gp_uid,
                                               uint64_t *sp_id);

/**
 * @brief Get the local SP identifier within a group.
 * Based on the definition of group global SP ids, this function
 * returns the local identifier for a specific SP. Local identifiers
 * are contiguous, from 0 to the number of SP on the target DPU, based
 * on the ordering from the group global SP identifiers.
 * 
 * @param[in] engine Associated offload engine
 * @param[in] gp_uid Target group identified by its unique group identifier
 * @param[in] sp_gp_guid Group global identifier of the target SP
 * @param[in,out] sp_gp_lid Returned group local SP identifier
 * @return dpu_offload_status_t 
 */
dpu_offload_status_t get_local_sp_id_by_group(offloading_engine_t *engine,
                                              group_uid_t gp_uid,
                                              uint64_t sp_gp_giuid,
                                              uint64_t *sp_gp_lid);

dpu_offload_status_t get_ordered_host_id_by_group(offloading_engine_t *engine,
                                                  group_uid_t group_id,
                                                  host_info_t *host_id);

dpu_offload_status_t get_num_sps_by_group_host(offloading_engine_t *engine,
                                               group_uid_t group_id,
                                               size_t *num_sps);

dpu_offload_status_t get_num_ranks_for_group_sp(offloading_engine_t *engine,
                                                group_uid_t group_id,
                                                size_t *num_ranks);

dpu_offload_status_t get_num_ranks_for_group_host(offloading_engine_t *engine,
                                                  group_uid_t group_id,
                                                  host_info_t my_host_id,
                                                  size_t *num_ranks);

dpu_offload_status_t get_n_for_rank_by_group_host(offloading_engine_t *engine,
                                                  group_uid_t group_id,
                                                  host_info_t my_host_id,
                                                  int64_t *idx);

dpu_offload_status_t get_all_sps_by_group_host(offloading_engine_t *engine,
                                               group_uid_t group_id,
                                               host_info_t host_id,
                                               dyn_array_t *sps,
                                               size_t *num_sps);

dpu_offload_status_t get_all_hosts_by_group(offloading_engine_t *engine,
                                            group_uid_t group_id,
                                            dyn_array_t *hosts,
                                            size_t *num_hosts);

dpu_offload_status_t get_all_ranks_by_group_sp(offloading_engine_t *engine,
                                               group_uid_t group_id,
                                               uint64_t remote_global_group_sp_id,
                                               dyn_array_t *ranks,
                                               size_t *num_ranks);

dpu_offload_status_t get_nth_sp_by_group_host(offloading_engine_t *engine,
                                              group_uid_t group_id,
                                              host_info_t host_id,
                                              uint64_t *global_group_sp_id);

#endif // DPU_OFFLOAD_GROUP_CACHE_H_