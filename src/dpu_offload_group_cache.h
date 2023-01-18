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
#endif // DPU_OFFLOAD_GROUP_CACHE_H_