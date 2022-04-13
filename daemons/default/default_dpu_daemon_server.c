#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

int main(int argc, char **argv)
{
    offloading_engine_t *offload_engine = NULL;
    execution_context_t *server = NULL;

    if (argc < 2)
    {
        fprintf(stderr, "ERROR: the test requires a argument to specify the number of expected clients");
        goto error_out;
    }
    int num_expected_clients = atoi(argv[1]);

    int rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }

    server = server_init(offload_engine, NULL);
    if (server == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }
    ADD_SERVER_TO_ENGINE(server, offload_engine);

    // Waiting for all the expected client to connect
    ECONTEXT_LOCK(server);
    size_t num_connected_clients = server->server->connected_clients.num_total_connected_clients;
    ECONTEXT_UNLOCK(server);
    while (num_connected_clients != num_expected_clients)
    {
        lib_progress(server);
        ECONTEXT_LOCK(server);
        num_connected_clients = server->server->connected_clients.num_total_connected_clients;
        ECONTEXT_UNLOCK(server);
    }

    fprintf(stderr, "The %d expected clients connected, all done\n", num_expected_clients);

    // Now wait for termination
    while (!server->server->done)
    {
        lib_progress(server);
    }

    server_fini(&server);
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "TEST SUCCEEDED\n");
    return EXIT_SUCCESS;
error_out:
    if (server)
        server_fini(&server);
    if (offload_engine)
        offload_engine_fini(&offload_engine);
    fprintf(stderr, "TEST FAILED\n");
    return EXIT_FAILURE;
}