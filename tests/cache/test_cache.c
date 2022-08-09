//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdint.h>

//#include "ucs/datastruct/khash.h"

#include "dpu_offload_service_daemon.h"
#include "test_cache_common.h"

/*
 * To run the test, simply execute: $ ./test_cache
 */

#if 0
KHASH_MAP_INIT_INT64(group_hash_t, group_id_t*);

void print_hash(khash_t(group_hash_t) *hash)
{
    uint64_t key;
    group_id_t *value;

    kh_foreach(hash, key, value, {
        (void)fprintf(stdout, "Hash size = %d\n", kh_size(hash));
        (void)fprintf(stdout, "%d %d\n", value->id, value->lead);
    })
}
#endif

int main(int argc, char **argv)
{
    /* Initialize everything we need for the test */
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }

    fprintf(stdout, "Populating cache...\n");
    POPULATE_CACHE(offload_engine);

    fprintf(stdout, "Checking cache...\n");
    CHECK_CACHE(offload_engine);

    offload_engine_fini(&offload_engine);

    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    //offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}