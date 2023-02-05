//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>

#include "dpu_offload_service_daemon.h"
#include "test_cache_common.h"

const group_uid_t gp_uid = 42;

int main(int argc, char **argv)
{
    /* Initialize everything we need for the test */
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "ERROR: offload_engine_init() failed\n");
        goto error_out;
    }

    execution_context_t *server = server_init(offload_engine, NULL);
    if (server == NULL)
    {
        fprintf(stderr, "ERROR: server handle is undefined\n");
        goto error_out;
    }
    ADD_SERVER_TO_ENGINE(server, offload_engine);

    ECONTEXT_LOCK(server);
    int connected_clients = server->server->connected_clients.num_total_connected_clients;
    ECONTEXT_UNLOCK(server);
    while (connected_clients == 0)
    {
        lib_progress(server);
        ECONTEXT_LOCK(server);
        connected_clients = server->server->connected_clients.num_total_connected_clients;
        ECONTEXT_UNLOCK(server);
    }

    POPULATE_CACHE(offload_engine, default_gp_uid);

    peer_info_t *peer_info = DYN_ARRAY_GET_ELT(&(server->server->connected_clients.clients), 0UL, peer_info_t);
    assert(peer_info);
    ucp_ep_h remote_ep = peer_info->ep;
    if (remote_ep == NULL)
    {
        fprintf(stderr, "ERROR: undefined destination endpoint\n");
        goto error_out;
    }

    fprintf(stderr, "Exchanging cache...\n");
    dpu_offload_event_t *ev;
    rc = event_get(server->event_channels, NULL, &ev);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "ERROR: event_get() failed\n");
        goto error_out;
    }
    if (ev == NULL)
    {
        fprintf(stderr, "ERROR: undefined event\n");
        goto error_out;
    }
    // fixme
    /*
    rc = send_cache(server, &(offload_engine->procs_cache), remote_ep, ev);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "ERROR: send_cache() failed\n");
        goto error_out;
    }
    */

    fprintf(stderr, "Waiting for client to terminate...\n");
    while (!EXECUTION_CONTEXT_DONE(server))
    {
        lib_progress(server);
    }

    event_return(&ev);

    server_fini(&server);
    offload_engine_fini(&offload_engine);
    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    server_fini(&server);
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}