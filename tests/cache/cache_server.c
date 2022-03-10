//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>

#include "dpu_offload_service_daemon.h"
#include "test_cache_common.h"

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

    execution_context_t *server = server_init(offload_engine, NULL);
    if (server == NULL)
    {
        fprintf(stderr, "server handle is undefined\n");
        goto error_out;
    }

    POPULATE_CACHE(offload_engine);

    ucp_ep_h remote_ep = server->server->connected_clients.clients[0].ep;
    if (remote_ep == NULL)
    {
        fprintf(stderr, "undefined destination endpoint\n");
        goto error_out;
    }
    rc = exchange_cache(server, &(offload_engine->procs_cache), remote_ep);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "exchange_cache() failed\n");
        goto error_out;
    }

    offload_engine_fini(&offload_engine);
    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}